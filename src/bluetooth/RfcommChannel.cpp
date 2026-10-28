// SPDX-License-Identifier: GPL-2.0-or-later
#include "RfcommChannel.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

namespace benshi {

namespace {

constexpr uint16_t kSppUuid16 = 0x1101;
constexpr uint16_t kGenericAudioUuid16 = 0x1203;
constexpr uint16_t kPublicBrowseGroupUuid16 = 0x1002;

bool parseAddress(const std::string &text, bdaddr_t &address) {
  return str2ba(text.c_str(), &address) == 0;
}

uuid_t makeSdpUuid(const BtUuid &uuid) {
  uuid_t result{};
  if (uuid.is16) {
    sdp_uuid16_create(&result, uuid.uuid16);
  } else {
    sdp_uuid128_create(&result, uuid.uuid128.data());
  }
  return result;
}

struct SdpSearchResult {
  uint8_t channel = 0;
  std::string serviceName;
};

// Search the radio's SDP database for a service and return its RFCOMM channel.
bool searchRfcommChannel(sdp_session_t *session, const BtUuid &service,
                         SdpSearchResult &result) {
  uuid_t uuid = makeSdpUuid(service);
  sdp_list_t *search = sdp_list_append(nullptr, &uuid);
  uint32_t range = 0x0000ffff;
  sdp_list_t *attrs = sdp_list_append(nullptr, &range);
  sdp_list_t *records = nullptr;

  const int status = sdp_service_search_attr_req(
      session, search, SDP_ATTR_REQ_RANGE, attrs, &records);
  sdp_list_free(attrs, nullptr);
  sdp_list_free(search, nullptr);
  if (status != 0) {
    return false;
  }

  bool found = false;
  for (sdp_list_t *node = records; node != nullptr;) {
    sdp_list_t *next = node->next;
    auto *record = static_cast<sdp_record_t *>(node->data);
    if (!found) {
      sdp_list_t *protos = nullptr;
      if (sdp_get_access_protos(record, &protos) == 0) {
        const int channel = sdp_get_proto_port(protos, RFCOMM_UUID);
        if (channel > 0 && channel <= 30) {
          result.channel = static_cast<uint8_t>(channel);
          char name[128] = {0};
          if (sdp_get_service_name(record, name, sizeof(name)) == 0) {
            result.serviceName = name;
          }
          found = true;
        }
        for (sdp_list_t *proto = protos; proto != nullptr; proto = proto->next) {
          sdp_list_free(static_cast<sdp_list_t *>(proto->data), nullptr);
        }
        sdp_list_free(protos, nullptr);
      }
    }
    free(node);
    sdp_record_free(record);
    node = next;
  }
  return found;
}

// Enumerate every service and collect RFCOMM channels. Used as a final
// fallback on firmware that does not advertise the vendor UUID.
bool searchAllRfcommChannels(sdp_session_t *session,
                             std::vector<uint8_t> &channels) {
  uuid_t browse_group;
  sdp_uuid16_create(&browse_group, kPublicBrowseGroupUuid16);
  sdp_list_t *search = sdp_list_append(nullptr, &browse_group);
  uint32_t range = 0x0000ffff;
  sdp_list_t *attrs = sdp_list_append(nullptr, &range);
  sdp_list_t *records = nullptr;

  const int status = sdp_service_search_attr_req(
      session, search, SDP_ATTR_REQ_RANGE, attrs, &records);
  sdp_list_free(attrs, nullptr);
  sdp_list_free(search, nullptr);
  if (status != 0) {
    return false;
  }

  for (sdp_list_t *node = records; node != nullptr;) {
    sdp_list_t *next = node->next;
    auto *record = static_cast<sdp_record_t *>(node->data);
    bool isSpp = false;
    sdp_list_t *classes = nullptr;
    if (sdp_get_service_classes(record, &classes) == 0) {
      for (sdp_list_t *entry = classes; entry != nullptr; entry = entry->next) {
        auto *uuid = static_cast<uuid_t *>(entry->data);
        if (uuid->type == SDP_UUID16 && uuid->value.uuid16 == kSppUuid16) {
          isSpp = true;
        }
      }
      sdp_list_free(classes, free);
    }

    sdp_list_t *protos = nullptr;
    if (sdp_get_access_protos(record, &protos) == 0) {
      const int channel = sdp_get_proto_port(protos, RFCOMM_UUID);
      if (channel > 0 && channel <= 30 && !isSpp) {
        const uint8_t value = static_cast<uint8_t>(channel);
        if (std::find(channels.begin(), channels.end(), value) ==
            channels.end()) {
          channels.push_back(value);
        }
      }
      for (sdp_list_t *proto = protos; proto != nullptr; proto = proto->next) {
        sdp_list_free(static_cast<sdp_list_t *>(proto->data), nullptr);
      }
      sdp_list_free(protos, nullptr);
    }
    free(node);
    sdp_record_free(record);
    node = next;
  }
  return !channels.empty();
}

bool resolveChannels(const std::string &macAddress,
                     const std::string &localAdapter,
                     const std::vector<BtUuid> &uuids, bool allowFallback,
                     uint8_t &channel, std::string &serviceName,
                     std::string &error) {
  bdaddr_t target{};
  if (!parseAddress(macAddress, target)) {
    error = "Invalid Bluetooth MAC address: " + macAddress;
    return false;
  }

  bdaddr_t source{};
  if (!localAdapter.empty()) {
    if (!parseAddress(localAdapter, source)) {
      error = "Invalid local Bluetooth adapter address: " + localAdapter;
      return false;
    }
  } else {
    std::memset(&source, 0, sizeof(source));
  }

  sdp_session_t *session = sdp_connect(&source, &target, SDP_RETRY_IF_BUSY);
  if (session == nullptr) {
    error = "SDP connection failed: " + std::string(std::strerror(errno));
    return false;
  }

  bool found = false;
  for (const auto &uuid : uuids) {
    SdpSearchResult result;
    if (searchRfcommChannel(session, uuid, result)) {
      channel = result.channel;
      serviceName = result.serviceName;
      found = true;
      break;
    }
  }

  if (!found && allowFallback) {
    std::vector<uint8_t> channels;
    if (searchAllRfcommChannels(session, channels)) {
      channel = channels.front();
      serviceName = "first non-SPP RFCOMM service";
      found = true;
    }
  }

  sdp_close(session);
  if (!found) {
    error = "No matching RFCOMM service found. Pair the radio first and make "
            "sure it is powered on.";
  }
  return found;
}

} // namespace

BtUuid BtUuid::from16(uint16_t value) {
  BtUuid uuid;
  uuid.is16 = true;
  uuid.uuid16 = value;
  return uuid;
}

BtUuid BtUuid::from128(const std::array<uint8_t, 16> &bytes) {
  BtUuid uuid;
  uuid.is16 = false;
  uuid.uuid128 = bytes;
  return uuid;
}

std::string BtUuid::toString() const {
  if (is16) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", uuid16);
    return buffer;
  }
  char buffer[40];
  std::snprintf(
      buffer, sizeof(buffer),
      "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
      uuid128[0], uuid128[1], uuid128[2], uuid128[3], uuid128[4], uuid128[5],
      uuid128[6], uuid128[7], uuid128[8], uuid128[9], uuid128[10],
      uuid128[11], uuid128[12], uuid128[13], uuid128[14], uuid128[15]);
  return buffer;
}

RfcommChannel::RfcommChannel(Logger &logger) : logger_(logger) {}

RfcommChannel::~RfcommChannel() { close(); }

bool RfcommChannel::connect(const std::string &macAddress,
                            const std::vector<BtUuid> &serviceUuids,
                            bool allowFallback, std::string &error) {
  close();
  serviceUuid_.clear();

  if (!openSocket(macAddress, serviceUuids, allowFallback, error)) {
    return false;
  }

  running_.store(true);
  connected_.store(true);
  readThread_ = std::thread([this]() { readLoop(); });
  return true;
}

bool RfcommChannel::openSocket(const std::string &macAddress,
                               const std::vector<BtUuid> &uuids,
                               bool allowFallback, std::string &error) {
  uint8_t channel = 0;
  std::string serviceName;
  if (channelOverride_ > 0) {
    channel = static_cast<uint8_t>(channelOverride_);
    serviceName = "configured RFCOMM channel";
  } else if (!resolveChannels(macAddress, localAdapter_, uuids, allowFallback,
                              channel, serviceName, error)) {
    return false;
  }

  bdaddr_t target{};
  if (!parseAddress(macAddress, target)) {
    error = "Invalid Bluetooth MAC address: " + macAddress;
    return false;
  }

  fd_ = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (fd_ < 0) {
    error = "Cannot create RFCOMM socket: " + std::string(std::strerror(errno));
    return false;
  }

  if (!localAdapter_.empty()) {
    bdaddr_t source{};
    if (parseAddress(localAdapter_, source)) {
      struct sockaddr_rc local = {};
      local.rc_family = AF_BLUETOOTH;
      bacpy(&local.rc_bdaddr, &source);
      local.rc_channel = 0;
      if (bind(fd_, reinterpret_cast<struct sockaddr *>(&local),
               sizeof(local)) != 0) {
        logger_.warning("Could not bind RFCOMM socket to adapter " +
                        localAdapter_ + ": " + std::strerror(errno));
      }
    }
  }

  struct sockaddr_rc address = {};
  address.rc_family = AF_BLUETOOTH;
  bacpy(&address.rc_bdaddr, &target);
  address.rc_channel = channel;

  logger_.info("Connecting RFCOMM channel " + std::to_string(channel) + " (" +
               serviceName + ") on " + macAddress);
  if (::connect(fd_, reinterpret_cast<struct sockaddr *>(&address),
                sizeof(address)) != 0) {
    error = "RFCOMM connection failed: " + std::string(std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  struct timeval sendTimeout = {};
  sendTimeout.tv_sec = 2;
  setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
  int sendBufferSize = 256 * 1024;
  setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sendBufferSize,
             sizeof(sendBufferSize));

  serviceUuid_ = serviceName + " channel " + std::to_string(channel);
  return true;
}

void RfcommChannel::readLoop() {
  std::vector<uint8_t> buffer(4096);
  while (running_.load()) {
    struct pollfd pfd = {};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 500);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      break;
    }
    if ((pfd.revents & POLLIN) == 0) {
      continue;
    }

    const ssize_t count = recv(fd_, buffer.data(), buffer.size(), 0);
    if (count <= 0) {
      break;
    }
    if (dataCallback_) {
      dataCallback_(std::vector<uint8_t>(buffer.begin(),
                                         buffer.begin() + count));
    }
  }

  const bool wasConnected = connected_.exchange(false);
  running_.store(false);
  if (wasConnected) {
    logger_.info("RFCOMM channel disconnected");
    if (disconnectCallback_) {
      disconnectCallback_();
    }
  }
}

bool RfcommChannel::write(const uint8_t *data, size_t size) {
  if (!connected_.load() || data == nullptr || size == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(writeMutex_);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written =
        send(fd_, data + offset, size - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      logger_.warning("RFCOMM write failed: " +
                      std::string(std::strerror(errno)));
      connected_.store(false);
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

void RfcommChannel::close() {
  running_.store(false);
  connected_.store(false);

  if (fd_ >= 0) {
    shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }

  if (readThread_.joinable()) {
    readThread_.join();
  }
}

std::vector<BluetoothDeviceInfo> discoverBluetoothDevices(Logger &logger,
                                                          std::string &error) {
  std::vector<BluetoothDeviceInfo> result;
  const int deviceId = hci_get_route(nullptr);
  if (deviceId < 0) {
    error = "No Bluetooth adapter found";
    return result;
  }

  const int socket = hci_open_dev(deviceId);
  if (socket < 0) {
    error = "Cannot open Bluetooth adapter: " + std::string(std::strerror(errno));
    return result;
  }

  inquiry_info *info = nullptr;
  const int maxResponses = 255;
  const int count =
      hci_inquiry(deviceId, 8, maxResponses, nullptr, &info, IREQ_CACHE_FLUSH);
  if (count < 0) {
    error = "Bluetooth inquiry failed: " + std::string(std::strerror(errno));
    hci_close_dev(socket);
    return result;
  }

  for (int i = 0; i < count; ++i) {
    char address[19] = {0};
    ba2str(&info[i].bdaddr, address);

    char name[248] = {0};
    if (hci_read_remote_name(socket, &info[i].bdaddr, sizeof(name), name, 10000) <
        0) {
      name[0] = '\0';
    }

    BluetoothDeviceInfo device;
    device.address = address;
    device.name = name;
    result.push_back(std::move(device));
  }

  if (info != nullptr) {
    free(info);
  }
  hci_close_dev(socket);

  if (result.empty()) {
    logger.info("No Bluetooth devices found");
  }
  return result;
}

} // namespace benshi
