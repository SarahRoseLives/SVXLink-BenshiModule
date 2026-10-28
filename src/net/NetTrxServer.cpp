// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/NetTrxServer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace benshi {

using namespace nettrx;

NetTrxServer::NetTrxServer(Logger &logger, const Config &config)
    : logger_(logger), config_(config) {
  bindAddress_ = config_.getString("SVXLINK", "BIND", "127.0.0.1");
  const int port = config_.getInt("SVXLINK", "PORT", 5210);
  port_ = static_cast<uint16_t>(std::clamp(port, 1, 65535));
  authKey_ = config_.getString("SVXLINK", "AUTH_KEY", "");
}

NetTrxServer::~NetTrxServer() { stop(); }

bool NetTrxServer::start(std::string &error) {
  stop();
  running_.store(true);

  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  const std::string portText = std::to_string(port_);
  struct addrinfo *result = nullptr;
  const int status = getaddrinfo(
      bindAddress_.empty() ? nullptr : bindAddress_.c_str(), portText.c_str(),
      &hints, &result);
  if (status != 0) {
    error = "Cannot resolve bind address: " + std::string(gai_strerror(status));
    running_.store(false);
    return false;
  }

  int fd = -1;
  for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 4) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    error = "Cannot bind SVXLink NetTrx server to " + bindAddress_ + ":" +
            portText + ": " + std::strerror(errno);
    running_.store(false);
    return false;
  }

  listen_fd_ = fd;
  logger_.info("SVXLink NetTrx server listening on " + bindAddress_ + ":" +
               portText);
  if (authKey_.empty()) {
    logger_.warning(
        "SVXLINK/AUTH_KEY is empty; any local process can control the radio");
  }
  accept_thread_ = std::thread([this]() { acceptLoop(); });
  return true;
}

void NetTrxServer::stop() {
  if (!running_.exchange(false)) {
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    return;
  }

  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }
  disconnectClient();

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
}

void NetTrxServer::acceptLoop() {
  while (running_.load()) {
    struct pollfd pfd = {};
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 500);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (!running_.load()) {
      break;
    }
    if (ready == 0 || (pfd.revents & POLLIN) == 0) {
      continue;
    }

    struct sockaddr_storage address = {};
    socklen_t length = sizeof(address);
    const int fd =
        accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&address),
               &length);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (client_fd_.load() >= 0) {
      logger_.warning("Rejecting extra SVXLink NetTrx client");
      close(fd);
      continue;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval sendTimeout = {};
    sendTimeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

    client_fd_.store(fd);
    serveClient(fd);
  }
}

void NetTrxServer::serveClient(int fd) {
  logger_.info("SVXLink NetTrx client connected");
  rx_codec_.store(CodecNone);
  tx_codec_.store(CodecNone);
  mute_state_.store(MuteNone);

  state_.store(StateVersionSent);
  sendProtoVersion();
  if (authKey_.empty()) {
    sendAuthOk();
    state_.store(StateReady);
  } else {
    AuthChallenge challenge{};
    challenge.header.type = static_cast<uint32_t>(MessageType::AuthChallenge);
    challenge.header.size = sizeof(challenge);
    RAND_bytes(challenge.challenge, kAuthChallengeLen);
    std::memcpy(auth_challenge_, challenge.challenge, kAuthChallengeLen);
    sendRaw(&challenge, sizeof(challenge));
    state_.store(StateAuthWait);
  }

  std::vector<uint8_t> buffer;
  auto lastReceived = std::chrono::steady_clock::now();
  auto lastHeartbeat = lastReceived;

  while (running_.load() && client_fd_.load() == fd) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 500);

    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastHeartbeat >= std::chrono::seconds(5)) {
      Heartbeat heartbeat{};
      heartbeat.header.type = static_cast<uint32_t>(MessageType::Heartbeat);
      sendMessage(heartbeat);
      lastHeartbeat = now;
    }
    if (ready == 0) {
      if (state_.load() == StateAuthWait &&
          now - lastReceived > std::chrono::seconds(10)) {
        logger_.warning("SVXLink NetTrx authentication timed out");
        break;
      }
      if (state_.load() == StateReady &&
          now - lastReceived > std::chrono::seconds(35)) {
        logger_.warning("SVXLink NetTrx heartbeat timed out");
        break;
      }
      continue;
    }

    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      break;
    }
    if ((pfd.revents & POLLIN) == 0) {
      continue;
    }

    uint8_t chunk[4096];
    const ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0) {
      break;
    }
    lastReceived = now;
    buffer.insert(buffer.end(), chunk, chunk + count);

    bool keepGoing = true;
    while (buffer.size() >= sizeof(Header)) {
      Header header{};
      std::memcpy(&header, buffer.data(), sizeof(header));
      if (header.size < sizeof(Header) || header.size > 1024 * 1024) {
        logger_.warning("Invalid SVXLink NetTrx message size");
        keepGoing = false;
        break;
      }
      if (buffer.size() < header.size) {
        break;
      }
      if (!handleMessage(buffer.data(), header.size)) {
        keepGoing = false;
        break;
      }
      buffer.erase(buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(header.size));
    }
    if (!keepGoing) {
      break;
    }
  }

  disconnectClient();
  logger_.info("SVXLink NetTrx client disconnected");
}

bool NetTrxServer::handleMessage(const uint8_t *data, size_t size) {
  Header header{};
  std::memcpy(&header, data, sizeof(header));

  if (state_.load() == StateAuthWait) {
    if (header.type == static_cast<uint32_t>(MessageType::AuthResponse)) {
      return handleAuthResponse(data, size);
    }
    return true;
  }

  if (state_.load() != StateReady) {
    return true;
  }

  switch (static_cast<MessageType>(header.type)) {
  case MessageType::Heartbeat:
    return true;

  case MessageType::SetMuteState: {
    if (size < sizeof(SetMuteState)) {
      return true;
    }
    SetMuteState message{};
    std::memcpy(&message, data, sizeof(message));
    mute_state_.store(message.mute_state);
    if (callbacks_.mute_state) {
      callbacks_.mute_state(message.mute_state);
    }
    return true;
  }

  case MessageType::SetTxCtrlMode: {
    if (size < sizeof(SetTxCtrlMode)) {
      return true;
    }
    SetTxCtrlMode message{};
    std::memcpy(&message, data, sizeof(message));
    if (callbacks_.tx_mode) {
      callbacks_.tx_mode(message.mode);
    }
    return true;
  }

  case MessageType::Audio: {
    if (size < sizeof(Header) + sizeof(int32_t)) {
      return true;
    }
    int32_t payloadSize = 0;
    std::memcpy(&payloadSize, data + sizeof(Header), sizeof(payloadSize));
    if (payloadSize < 0 ||
        static_cast<size_t>(payloadSize) > kMaxAudioBytes ||
        size != sizeof(Header) + sizeof(int32_t) +
                    static_cast<size_t>(payloadSize)) {
      logger_.warning("Invalid SVXLink NetTrx audio message");
      return true;
    }
    if (payloadSize > 0 && callbacks_.tx_audio) {
      const auto *payload = data + sizeof(Header) + sizeof(int32_t);
      const int codec = tx_codec_.load();
      if (codec == CodecRaw) {
        callbacks_.tx_audio(reinterpret_cast<const float *>(payload),
                            static_cast<size_t>(payloadSize) / sizeof(float));
      } else if (codec == CodecS16) {
        const size_t sampleCount =
            static_cast<size_t>(payloadSize) / sizeof(int16_t);
        std::vector<float> samples(sampleCount);
        const auto *s16 = reinterpret_cast<const int16_t *>(payload);
        for (size_t i = 0; i < sampleCount; ++i) {
          samples[i] = static_cast<float>(s16[i]) / 32768.0f;
        }
        callbacks_.tx_audio(samples.data(), samples.size());
      }
    }
    return true;
  }

  case MessageType::Flush:
    if (callbacks_.flush) {
      callbacks_.flush();
    }
    return true;

  case MessageType::Reset:
    if (callbacks_.reset) {
      callbacks_.reset();
    }
    return true;

  case MessageType::RxAudioCodecSelect:
  case MessageType::TxAudioCodecSelect: {
    if (size < sizeof(Header) + 32) {
      return true;
    }
    char name[33] = {0};
    std::memcpy(name, data + sizeof(Header), 32);
    int codec = CodecNone;
    if (std::strcmp(name, "RAW") == 0) {
      codec = CodecRaw;
    } else if (std::strcmp(name, "S16") == 0) {
      codec = CodecS16;
    }
    if (header.type ==
        static_cast<uint32_t>(MessageType::RxAudioCodecSelect)) {
      rx_codec_.store(codec);
    } else {
      tx_codec_.store(codec);
    }
    if (codec == CodecNone) {
      logger_.error("SVXLink requested unsupported audio codec \"" +
                    std::string(name) + "\"; use CODEC=S16 or CODEC=RAW");
    } else {
      logger_.info(std::string("SVXLink audio codec: ") +
                   (header.type ==
                            static_cast<uint32_t>(
                                MessageType::RxAudioCodecSelect)
                        ? "RX "
                        : "TX ") +
                   name);
    }
    return true;
  }

  case MessageType::SetRxFq: {
    if (size >= sizeof(SetRxFq)) {
      SetRxFq message{};
      std::memcpy(&message, data, sizeof(message));
      if (callbacks_.rx_frequency) {
        callbacks_.rx_frequency(message.frequency);
      }
    }
    return true;
  }

  case MessageType::SetTxFq: {
    if (size >= sizeof(SetTxFq)) {
      SetTxFq message{};
      std::memcpy(&message, data, sizeof(message));
      if (callbacks_.tx_frequency) {
        callbacks_.tx_frequency(message.frequency);
      }
    }
    return true;
  }

  case MessageType::EnableCtcss:
  case MessageType::AddToneDetector:
  case MessageType::SendDtmf:
  case MessageType::TransmittedSignalStrength:
  case MessageType::SetRxModulation:
  case MessageType::SetTxModulation:
    return true;

  default:
    logger_.debug("Ignoring SVXLink NetTrx message type " +
                  std::to_string(header.type));
    return true;
  }
}

bool NetTrxServer::handleAuthResponse(const uint8_t *data, size_t size) {
  if (size < sizeof(AuthResponse)) {
    logger_.warning("Short SVXLink NetTrx authentication response");
    return false;
  }

  AuthResponse response{};
  std::memcpy(&response, data, sizeof(response));

  unsigned char digest[EVP_MAX_MD_SIZE] = {0};
  unsigned int digestLength = 0;
  HMAC(EVP_sha1(), authKey_.data(), static_cast<int>(authKey_.size()),
       auth_challenge_, kAuthChallengeLen, digest, &digestLength);
  if (digestLength != kAuthDigestLen ||
      CRYPTO_memcmp(digest, response.digest, kAuthDigestLen) != 0) {
    logger_.error("SVXLink NetTrx authentication failed");
    return false;
  }

  sendAuthOk();
  state_.store(StateReady);
  logger_.info("SVXLink NetTrx client authenticated");
  return true;
}

void NetTrxServer::disconnectClient() {
  const int fd = client_fd_.exchange(-1);
  state_.store(StateDisconnected);
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
}

bool NetTrxServer::sendRaw(const void *data, size_t size) {
  if (data == nullptr || size == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(write_mutex_);
  const int fd = client_fd_.load();
  if (fd < 0 || state_.load() == StateDisconnected) {
    return false;
  }

  const auto *ptr = static_cast<const uint8_t *>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written =
        send(fd, ptr + offset, size - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      shutdown(fd, SHUT_RDWR);
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool NetTrxServer::sendProtoVersion() {
  ProtoVer message{};
  message.header.type = static_cast<uint32_t>(MessageType::ProtoVer);
  message.header.size = sizeof(message);
  return sendRaw(&message, sizeof(message));
}

bool NetTrxServer::sendAuthOk() {
  AuthOk message{};
  message.header.type = static_cast<uint32_t>(MessageType::AuthOk);
  message.header.size = sizeof(message);
  return sendRaw(&message, sizeof(message));
}

void NetTrxServer::sendSquelch(bool is_open, float signal_strength, char rx_id,
                               const std::string &activity) {
  if (state_.load() != StateReady) {
    return;
  }
  Squelch message{};
  message.header.type = static_cast<uint32_t>(MessageType::Squelch);
  message.header.size = sizeof(message);
  message.is_open = is_open;
  message.signal_strength = signal_strength;
  message.sql_rx_id = rx_id;
  std::strncpy(message.activity_info, activity.c_str(),
               sizeof(message.activity_info) - 1);
  sendRaw(&message, sizeof(message));
}

void NetTrxServer::sendSiglev(float signal_strength, char rx_id) {
  if (state_.load() != StateReady) {
    return;
  }
  SiglevUpdate message{};
  message.header.type = static_cast<uint32_t>(MessageType::SiglevUpdate);
  message.header.size = sizeof(message);
  message.signal_strength = signal_strength;
  message.sql_rx_id = rx_id;
  sendRaw(&message, sizeof(message));
}

void NetTrxServer::sendDtmf(char digit, int duration_ms) {
  if (state_.load() != StateReady || mute_state_.load() != MuteNone) {
    return;
  }
  Dtmf message{};
  message.header.type = static_cast<uint32_t>(MessageType::Dtmf);
  message.header.size = sizeof(message);
  message.digit = digit;
  message.duration = duration_ms;
  sendRaw(&message, sizeof(message));
}

void NetTrxServer::sendRxAudio(const float *samples, size_t count) {
  const int codec = rx_codec_.load();
  if (samples == nullptr || count == 0 ||
      state_.load() != StateReady || mute_state_.load() != MuteNone ||
      codec == CodecNone) {
    return;
  }

  std::vector<int16_t> s16;
  if (codec == CodecS16) {
    s16.resize(count);
    for (size_t i = 0; i < count; ++i) {
      const float value = std::clamp(samples[i], -1.0f, 1.0f);
      s16[i] = static_cast<int16_t>(value * 32767.0f);
    }
  }

  const auto *bytes =
      codec == CodecRaw
          ? reinterpret_cast<const uint8_t *>(samples)
          : reinterpret_cast<const uint8_t *>(s16.data());
  size_t remaining =
      codec == CodecRaw ? count * sizeof(float) : s16.size() * sizeof(int16_t);
  while (remaining > 0) {
    const size_t chunk = std::min(remaining, kMaxAudioBytes);
    Audio message{};
    message.header.type = static_cast<uint32_t>(MessageType::Audio);
    message.header.size =
        static_cast<uint32_t>(sizeof(Header) + sizeof(int32_t) + chunk);
    message.payload_size = static_cast<int32_t>(chunk);
    std::memcpy(message.data, bytes, chunk);
    if (!sendRaw(&message, sizeof(Header) + sizeof(int32_t) + chunk)) {
      return;
    }
    bytes += chunk;
    remaining -= chunk;
  }
}

void NetTrxServer::sendTransmitterState(bool transmitting) {
  if (state_.load() != StateReady) {
    return;
  }
  TransmitterStateChange message{};
  message.header.type =
      static_cast<uint32_t>(MessageType::TransmitterStateChange);
  message.header.size = sizeof(message);
  message.is_transmitting = transmitting;
  sendRaw(&message, sizeof(message));
}

void NetTrxServer::sendAllSamplesFlushed() {
  if (state_.load() != StateReady) {
    return;
  }
  AllSamplesFlushed message{};
  message.header.type = static_cast<uint32_t>(MessageType::AllSamplesFlushed);
  message.header.size = sizeof(message);
  sendRaw(&message, sizeof(message));
}

} // namespace benshi
