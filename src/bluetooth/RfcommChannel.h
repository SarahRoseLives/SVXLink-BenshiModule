// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Logger.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace benshi {

struct BtUuid {
  bool is16 = true;
  uint16_t uuid16 = 0;
  std::array<uint8_t, 16> uuid128{};

  static BtUuid from16(uint16_t value);
  static BtUuid from128(const std::array<uint8_t, 16> &bytes);
  std::string toString() const;
};

struct BluetoothDeviceInfo {
  std::string address;
  std::string name;
  bool paired = false;
};

class RfcommChannel {
public:
  using DataCallback = std::function<void(std::vector<uint8_t>)>;
  using DisconnectCallback = std::function<void()>;

  explicit RfcommChannel(Logger &logger);
  ~RfcommChannel();

  RfcommChannel(const RfcommChannel &) = delete;
  RfcommChannel &operator=(const RfcommChannel &) = delete;

  void setLocalAdapter(const std::string &address) { localAdapter_ = address; }

  // Optional manual RFCOMM channel override. 0 means discover via SDP.
  void setChannelOverride(int channel) { channelOverride_ = channel; }

  bool connect(const std::string &macAddress,
               const std::vector<BtUuid> &serviceUuids, bool allowFallback,
               std::string &error);
  void close();
  bool write(const uint8_t *data, size_t size);

  void setDataCallback(DataCallback callback) {
    dataCallback_ = std::move(callback);
  }
  void setDisconnectCallback(DisconnectCallback callback) {
    disconnectCallback_ = std::move(callback);
  }

  bool isConnected() const { return connected_.load(); }
  const std::string &serviceUuid() const { return serviceUuid_; }

private:
  void readLoop();
  bool openSocket(const std::string &macAddress, const std::vector<BtUuid> &uuids,
                  bool allowFallback, std::string &error);

  Logger &logger_;
  int fd_ = -1;
  std::string localAdapter_;
  int channelOverride_ = 0;
  std::thread readThread_;
  std::mutex writeMutex_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  DataCallback dataCallback_;
  DisconnectCallback disconnectCallback_;
  std::string serviceUuid_;
};

// Scan for Classic Bluetooth devices in range. Requires an HCI adapter.
std::vector<BluetoothDeviceInfo> discoverBluetoothDevices(Logger &logger,
                                                          std::string &error);

} // namespace benshi
