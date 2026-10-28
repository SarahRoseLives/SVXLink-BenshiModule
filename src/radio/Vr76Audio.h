// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Logger.h"
#include "bluetooth/RfcommChannel.h"
#include "radio/AudioFraming.h"
#include "radio/SbcCodec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace benshi {

class Vr76Audio {
public:
  using PcmCallback =
      std::function<void(const std::vector<int16_t> &, bool transmit)>;
  using EndCallback = std::function<void(bool transmit)>;
  using DisconnectCallback = std::function<void()>;

  explicit Vr76Audio(Logger &logger);
  ~Vr76Audio();

  bool connect(const std::string &macAddress, const std::string &localAdapter,
               std::string &error);
  void disconnect();
  bool isConnected() const { return channel_.isConnected(); }

  void setPcmCallback(PcmCallback callback) {
    pcmCallback_ = std::move(callback);
  }
  void setEndCallback(EndCallback callback) { endCallback_ = std::move(callback); }
  void setDisconnectCallback(DisconnectCallback callback) {
    disconnectCallback_ = std::move(callback);
  }

  bool beginTx();
  bool sendPcm(const int16_t *samples, size_t count);
  bool endTx();
  bool sendEndFrame();
  bool isTxActive() const { return txActive_.load(); }

  int bitpool() const { return codec_.bitpool(); }
  void setBitpool(int bitpool) { codec_.setBitpool(bitpool); }
  void setChannelOverride(int channel) { channel_.setChannelOverride(channel); }

private:
  void onData(std::vector<uint8_t> data);

  Logger &logger_;
  RfcommChannel channel_;
  AudioFrameParser parser_;
  SbcCodec codec_;
  std::mutex txMutex_;
  std::vector<int16_t> txRemainder_;
  std::atomic<bool> txActive_{false};
  PcmCallback pcmCallback_;
  EndCallback endCallback_;
  DisconnectCallback disconnectCallback_;
};

} // namespace benshi
