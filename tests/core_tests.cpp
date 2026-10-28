// SPDX-License-Identifier: GPL-2.0-or-later
#include "Config.h"
#include "Logger.h"
#include "audio/DtmfDecoder.h"
#include "audio/Resampler.h"
#include "net/NetTrxMessages.h"
#include "net/NetTrxServer.h"
#include "radio/AudioFraming.h"
#include "radio/GaiaProtocol.h"
#include "radio/SbcCodec.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

void testConfig() {
  const auto path =
      std::filesystem::temp_directory_path() / "svxlink_benshi_core_test.conf";
  {
    std::ofstream out(path);
    out << "[RADIO]\nMAC=AA:BB:CC:DD:EE:FF\nDUAL_WATCH=0\n"
        << "[SVXLINK]\nPORT=5210\nAUTH_KEY=secret\n"
        << "[BRIDGE]\nSQUELCH_MODE=auto\nVOX_THRESHOLD_DB=-38\n";
  }

  benshi::Config config;
  std::string error;
  check(config.load(path.string(), error), "config loads");
  check(config.getString("RADIO", "MAC") == "AA:BB:CC:DD:EE:FF",
        "config string");
  check(config.getInt("SVXLINK", "PORT", 0) == 5210, "config int");
  check(config.getInt("BRIDGE", "VOX_THRESHOLD_DB", 0) == -38,
        "config negative int");
  check(config.getInt("RADIO", "DUAL_WATCH", 1) == 0, "config zero int");
  std::filesystem::remove(path);
}

void testGaia() {
  const auto command = benshi::gaia::buildCommandByte(
      benshi::gaia::kGroupBasic,
      static_cast<uint16_t>(benshi::gaia::Command::GetHtStatus), 0x42);
  check(command.size() == 5, "gaia command size");
  check(command[0] == 0x00 && command[1] == 0x02, "gaia group");
  check(command[2] == 0x00 && command[3] == 0x14, "gaia command");

  const auto framed = benshi::gaia::encode(command);
  check(framed.size() == 9, "gaia frame size");
  check(framed[0] == 0xFF && framed[1] == 0x01, "gaia frame sync");
  check(framed[3] == 1, "gaia payload length");

  benshi::gaia::Decoder decoder;
  decoder.push(framed.data(), framed.size());
  benshi::gaia::Frame frame;
  check(decoder.next(frame), "gaia decode");
  check(frame.group == benshi::gaia::kGroupBasic, "gaia decoded group");
  check(frame.command == 20, "gaia decoded command");
  check(!frame.isResponse, "gaia request bit");
  check(frame.payload.size() == 1 && frame.payload[0] == 0x42,
        "gaia payload");

  const auto reply = benshi::gaia::buildCommand(
      benshi::gaia::kGroupBasic,
      static_cast<uint16_t>(
          0x8000 | static_cast<uint16_t>(benshi::gaia::Command::GetHtStatus)),
      {0x00, 0x80, 0x00, 0x00, 0x00});
  const auto framedReply = benshi::gaia::encode(reply);
  decoder.push(framedReply.data(), framedReply.size());
  check(decoder.next(frame), "gaia reply decode");
  check(frame.isResponse, "gaia reply bit");
  check(frame.command == 20, "gaia reply command");
  check(frame.payload.size() == 5, "gaia reply payload");
}

void testAudioFraming() {
  const std::vector<uint8_t> payload = {0x01, 0x7E, 0x02, 0x7D, 0x03, 0x00};
  const auto framed = benshi::frameAudio(0x00, payload);
  check(framed.front() == 0x7E && framed.back() == 0x7E,
        "audio frame markers");

  benshi::AudioFrameParser parser;
  parser.push(framed.data(), framed.size());
  std::vector<uint8_t> parsed;
  check(parser.next(parsed), "audio frame parse");
  check(parsed.size() == payload.size() + 1, "audio frame size");
  check(parsed[0] == 0x00, "audio frame command");
  for (size_t i = 0; i < payload.size(); ++i) {
    check(parsed[i + 1] == payload[i], "audio frame escaped payload");
  }

  const auto end = benshi::endAudioFrame();
  check(end.size() == 11 && end.front() == 0x7E && end.back() == 0x7E,
        "audio end frame");
}

void testSbc() {
  benshi::SbcCodec codec;
  codec.setBitpool(40);
  std::vector<int16_t> samples(benshi::SbcCodec::kSamplesPerFrame);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<int16_t>(
        9000.0 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 32000.0));
  }

  std::vector<uint8_t> encoded;
  check(codec.encode(samples.data(), samples.size(), encoded),
        "sbc encode succeeds");
  check(!encoded.empty() && encoded[0] == 0x9C, "sbc frame header");

  std::vector<int16_t> decoded;
  check(codec.decode(encoded.data(), encoded.size(), decoded),
        "sbc decode succeeds");
  check(decoded.size() == samples.size(), "sbc decoded sample count");
  bool nonZero = false;
  for (int16_t sample : decoded) {
    if (sample != 0) {
      nonZero = true;
      break;
    }
  }
  check(nonZero, "sbc decoded audio is non-zero");
}

void testDtmf() {
  benshi::DtmfDecoder decoder;
  std::vector<int16_t> samples;
  const double twoPi = 2.0 * 3.141592653589793;
  const int count = 32000 * 200 / 1000;
  samples.reserve(count);
  for (int i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / 32000.0;
    const double value = 6000.0 * std::sin(twoPi * 770.0 * t) +
                         6000.0 * std::sin(twoPi * 1336.0 * t);
    samples.push_back(static_cast<int16_t>(value));
  }

  std::string digits;
  for (size_t offset = 0; offset < samples.size(); offset += 256) {
    const size_t n = std::min<size_t>(256, samples.size() - offset);
    decoder.process(samples.data() + offset, n,
                    [&](char digit, int) { digits.push_back(digit); });
  }
  check(digits == "5", "dtmf digit 5 decoded");

  std::vector<int16_t> silence(1024, 0);
  decoder.process(silence.data(), silence.size(), [&](char, int) {});
  check(!decoder.toneActive(), "dtmf tone ended");
}

void testResampler() {
  benshi::Downsampler down(4);
  std::vector<int16_t> input(320, 1000);
  std::vector<int16_t> output;
  down.process(input.data(), input.size(), output);
  check(output.size() == 80, "downsampler output size");
  check(output[0] == 1000, "downsampler value");

  benshi::Upsampler up(4);
  std::vector<int16_t> upsampled;
  up.process(output.data(), output.size(), upsampled);
  check(upsampled.size() == 320, "upsampler output size");

  benshi::Downsampler down16(2);
  std::vector<int16_t> output16;
  down16.process(input.data(), input.size(), output16);
  check(output16.size() == 160, "downsampler 16 kHz output size");

  benshi::Upsampler up16(2);
  std::vector<int16_t> upsampled16;
  up16.process(output16.data(), output16.size(), upsampled16);
  check(upsampled16.size() == 320, "upsampler 16 kHz output size");
}

bool sendAll(int fd, const void *data, size_t size) {
  const auto *ptr = static_cast<const uint8_t *>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written = send(fd, ptr + offset, size - offset, MSG_NOSIGNAL);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool readExactly(int fd, void *data, size_t size) {
  auto *ptr = static_cast<uint8_t *>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t count = recv(fd, ptr + offset, size - offset, 0);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  return true;
}

std::vector<uint8_t> readMessage(int fd) {
  benshi::nettrx::Header header{};
  if (!readExactly(fd, &header, sizeof(header))) {
    return {};
  }
  if (header.size < sizeof(header) || header.size > 1024 * 1024) {
    return {};
  }
  std::vector<uint8_t> message(header.size);
  std::memcpy(message.data(), &header, sizeof(header));
  if (header.size > sizeof(header) &&
      !readExactly(fd, message.data() + sizeof(header),
                   header.size - sizeof(header))) {
    return {};
  }
  return message;
}

std::vector<uint8_t> readMessageOfType(int fd, uint32_t expectedType) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    const auto message = readMessage(fd);
    if (message.size() < sizeof(benshi::nettrx::Header)) {
      return {};
    }
    benshi::nettrx::Header header{};
    std::memcpy(&header, message.data(), sizeof(header));
    if (header.type == expectedType) {
      return message;
    }
  }
  return {};
}

int connectTo(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct timeval timeout = {};
  timeout.tv_sec = 3;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

  for (int attempt = 0; attempt < 50; ++attempt) {
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&address),
                sizeof(address)) == 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  close(fd);
  return -1;
}

void writeConfig(const std::filesystem::path &path, uint16_t port,
                 const std::string &authKey) {
  std::ofstream out(path);
  out << "[RADIO]\nMAC=AA:BB:CC:DD:EE:FF\n"
      << "[SVXLINK]\nBIND=127.0.0.1\nPORT=" << port << "\n"
      << "AUTH_KEY=" << authKey << "\n"
      << "[LOG]\nLEVEL=error\n";
}

void testNetTrx() {
  const auto path = std::filesystem::temp_directory_path() /
                    "svxlink_benshi_nettrx_test.conf";
  const uint16_t port = static_cast<uint16_t>(45000 + (getpid() % 10000));
  writeConfig(path, port, "");

  benshi::Config config;
  std::string error;
  check(config.load(path.string(), error), "nettrx config loads");

  benshi::Logger logger(benshi::LogLevel::Error);
  benshi::NetTrxServer server(logger, config);

  std::atomic<size_t> txAudioSamples{0};
  std::atomic<size_t> flushCount{0};
  std::atomic<int> lastTxMode{-1};
  benshi::NetTrxServer::Callbacks callbacks;
  callbacks.tx_audio = [&](const float *, size_t count) {
    txAudioSamples += count;
  };
  callbacks.tx_mode = [&](int mode) { lastTxMode = mode; };
  callbacks.flush = [&]() { ++flushCount; };
  server.setCallbacks(std::move(callbacks));

  check(server.start(error), "nettrx server starts");

  const int fd = connectTo(port);
  check(fd >= 0, "nettrx client connects");
  if (fd < 0) {
    server.stop();
    std::filesystem::remove(path);
    return;
  }

  auto message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::ProtoVer));
  check(message.size() == sizeof(benshi::nettrx::ProtoVer),
        "nettrx proto version received");
  if (message.size() == sizeof(benshi::nettrx::ProtoVer)) {
    benshi::nettrx::ProtoVer version{};
    std::memcpy(&version, message.data(), sizeof(version));
    check(version.major == 2 && version.minor == 8, "nettrx protocol 2.8");
  }

  message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::AuthOk));
  check(!message.empty(), "nettrx auth ok received");

  benshi::nettrx::AudioCodecSelect rxCodec{};
  rxCodec.header.type = static_cast<uint32_t>(
      benshi::nettrx::MessageType::RxAudioCodecSelect);
  rxCodec.header.size = sizeof(rxCodec);
  std::strcpy(rxCodec.codec_name, "S16");
  check(sendAll(fd, &rxCodec, sizeof(rxCodec)), "send RX codec select");

  benshi::nettrx::AudioCodecSelect txCodec{};
  txCodec.header.type = static_cast<uint32_t>(
      benshi::nettrx::MessageType::TxAudioCodecSelect);
  txCodec.header.size = sizeof(txCodec);
  std::strcpy(txCodec.codec_name, "RAW");
  check(sendAll(fd, &txCodec, sizeof(txCodec)), "send TX codec select");

  benshi::nettrx::SetMuteState mute{};
  mute.header.type =
      static_cast<uint32_t>(benshi::nettrx::MessageType::SetMuteState);
  mute.header.size = sizeof(mute);
  mute.mute_state = benshi::nettrx::MuteNone;
  check(sendAll(fd, &mute, sizeof(mute)), "send mute state");

  benshi::nettrx::SetTxCtrlMode mode{};
  mode.header.type =
      static_cast<uint32_t>(benshi::nettrx::MessageType::SetTxCtrlMode);
  mode.header.size = sizeof(mode);
  mode.mode = benshi::nettrx::TxAuto;
  check(sendAll(fd, &mode, sizeof(mode)), "send TX mode");

  const float txSamples[2] = {0.25f, -0.25f};
  benshi::nettrx::Audio audio{};
  audio.header.type =
      static_cast<uint32_t>(benshi::nettrx::MessageType::Audio);
  audio.header.size =
      sizeof(benshi::nettrx::Header) + sizeof(int32_t) + sizeof(txSamples);
  audio.payload_size = sizeof(txSamples);
  std::memcpy(audio.data, txSamples, sizeof(txSamples));
  check(sendAll(fd, &audio, audio.header.size), "send TX audio");

  benshi::nettrx::Flush flush{};
  flush.header.type =
      static_cast<uint32_t>(benshi::nettrx::MessageType::Flush);
  flush.header.size = sizeof(flush);
  check(sendAll(fd, &flush, sizeof(flush)), "send flush");

  for (int attempt = 0; attempt < 100 &&
                        (txAudioSamples.load() != 2 || flushCount.load() != 1);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(txAudioSamples.load() == 2, "nettrx TX audio delivered");
  check(lastTxMode.load() == benshi::nettrx::TxAuto,
        "nettrx TX mode delivered");
  check(flushCount.load() == 1, "nettrx flush delivered");

  benshi::nettrx::AudioCodecSelect txS16{};
  txS16.header.type = static_cast<uint32_t>(
      benshi::nettrx::MessageType::TxAudioCodecSelect);
  txS16.header.size = sizeof(txS16);
  std::strcpy(txS16.codec_name, "S16");
  check(sendAll(fd, &txS16, sizeof(txS16)), "send S16 TX codec select");

  const int16_t txS16Samples[2] = {16384, -16384};
  benshi::nettrx::Audio s16Audio{};
  s16Audio.header.type =
      static_cast<uint32_t>(benshi::nettrx::MessageType::Audio);
  s16Audio.header.size = sizeof(benshi::nettrx::Header) + sizeof(int32_t) +
                         sizeof(txS16Samples);
  s16Audio.payload_size = sizeof(txS16Samples);
  std::memcpy(s16Audio.data, txS16Samples, sizeof(txS16Samples));
  check(sendAll(fd, &s16Audio, s16Audio.header.size), "send S16 TX audio");

  for (int attempt = 0; attempt < 100 && txAudioSamples.load() != 4;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(txAudioSamples.load() == 4, "nettrx S16 TX audio delivered");

  server.sendSquelch(true, -80.0f, '?', "test");
  message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::Squelch));
  check(message.size() == sizeof(benshi::nettrx::Squelch),
        "nettrx squelch received");

  server.sendSiglev(-70.0f, '?');
  message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::SiglevUpdate));
  check(message.size() == sizeof(benshi::nettrx::SiglevUpdate),
        "nettrx siglev received");

  server.sendDtmf('5', 120);
  message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::Dtmf));
  check(message.size() == sizeof(benshi::nettrx::Dtmf),
        "nettrx DTMF received");
  if (message.size() == sizeof(benshi::nettrx::Dtmf)) {
    benshi::nettrx::Dtmf dtmf{};
    std::memcpy(&dtmf, message.data(), sizeof(dtmf));
    check(dtmf.digit == '5' && dtmf.duration == 120, "nettrx DTMF payload");
  }

  const float rxSamples[4] = {0.0f, 0.5f, -0.5f, 1.0f};
  server.sendRxAudio(rxSamples, 4);
  message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::Audio));
  check(message.size() ==
            sizeof(benshi::nettrx::Header) + sizeof(int32_t) + 8,
        "nettrx S16 RX audio received");
  if (message.size() == sizeof(benshi::nettrx::Header) + sizeof(int32_t) + 8) {
    int16_t samples[4] = {0};
    std::memcpy(samples, message.data() + sizeof(benshi::nettrx::Header) +
                            sizeof(int32_t),
                sizeof(samples));
    check(samples[1] == 16383, "nettrx S16 conversion");
    check(samples[3] == 32767, "nettrx S16 clipping");
  }

  server.sendTransmitterState(true);
  message = readMessageOfType(
      fd, static_cast<uint32_t>(
              benshi::nettrx::MessageType::TransmitterStateChange));
  check(!message.empty(), "nettrx transmitter state received");

  server.sendAllSamplesFlushed();
  message = readMessageOfType(
      fd, static_cast<uint32_t>(
              benshi::nettrx::MessageType::AllSamplesFlushed));
  check(!message.empty(), "nettrx all samples flushed received");

  close(fd);
  server.stop();
  std::filesystem::remove(path);
}

void testNetTrxAuth() {
  const auto path = std::filesystem::temp_directory_path() /
                    "svxlink_benshi_nettrx_auth_test.conf";
  const uint16_t port = static_cast<uint16_t>(46000 + (getpid() % 10000));
  writeConfig(path, port, "secret-key");

  benshi::Config config;
  std::string error;
  check(config.load(path.string(), error), "auth config loads");

  benshi::Logger logger(benshi::LogLevel::Error);
  benshi::NetTrxServer server(logger, config);
  check(server.start(error), "auth server starts");

  const int fd = connectTo(port);
  check(fd >= 0, "auth client connects");
  if (fd < 0) {
    server.stop();
    std::filesystem::remove(path);
    return;
  }

  auto message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::ProtoVer));
  check(!message.empty(), "auth proto version received");

  message = readMessageOfType(
      fd, static_cast<uint32_t>(benshi::nettrx::MessageType::AuthChallenge));
  check(message.size() == sizeof(benshi::nettrx::AuthChallenge),
        "auth challenge received");
  if (message.size() == sizeof(benshi::nettrx::AuthChallenge)) {
    benshi::nettrx::AuthChallenge challenge{};
    std::memcpy(&challenge, message.data(), sizeof(challenge));

    benshi::nettrx::AuthResponse response{};
    response.header.type =
        static_cast<uint32_t>(benshi::nettrx::MessageType::AuthResponse);
    response.header.size = sizeof(response);
    unsigned int digestLength = 0;
    HMAC(EVP_sha1(), "secret-key", 10, challenge.challenge,
         benshi::nettrx::kAuthChallengeLen, response.digest, &digestLength);
    check(digestLength == benshi::nettrx::kAuthDigestLen, "auth digest size");
    check(sendAll(fd, &response, sizeof(response)), "send auth response");

    message = readMessageOfType(
        fd, static_cast<uint32_t>(benshi::nettrx::MessageType::AuthOk));
    check(!message.empty(), "auth ok received");
  }

  close(fd);
  server.stop();
  std::filesystem::remove(path);
}

} // namespace

int main() {
  testConfig();
  testGaia();
  testAudioFraming();
  testSbc();
  testDtmf();
  testResampler();
  testNetTrx();
  testNetTrxAuth();

  if (failures == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
