// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Config.h"
#include "Logger.h"
#include "net/NetTrxMessages.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace benshi {

// Speaks the SvxLink RemoteTrx ("NetTrx") protocol. SvxLink's built-in
// NetRx/NetTx transceiver types connect to this server, so no SVXLink patches
// and no ALSA loopback devices are needed.
class NetTrxServer {
public:
  struct Callbacks {
    std::function<void(const float *, size_t)> tx_audio;
    std::function<void(int)> tx_mode;
    std::function<void()> flush;
    std::function<void(int)> mute_state;
    std::function<void(uint32_t)> rx_frequency;
    std::function<void(uint32_t)> tx_frequency;
    std::function<void()> reset;
  };

  NetTrxServer(Logger &logger, const Config &config);
  ~NetTrxServer();

  NetTrxServer(const NetTrxServer &) = delete;
  NetTrxServer &operator=(const NetTrxServer &) = delete;

  bool start(std::string &error);
  void stop();

  void setCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

  bool hasClient() const { return state_.load() == StateReady; }

  void sendSquelch(bool is_open, float signal_strength, char rx_id,
                   const std::string &activity);
  void sendSiglev(float signal_strength, char rx_id);
  void sendDtmf(char digit, int duration_ms);
  void sendRxAudio(const float *samples, size_t count);
  void sendTransmitterState(bool transmitting);
  void sendAllSamplesFlushed();

private:
  enum State : int {
    StateDisconnected = 0,
    StateVersionSent = 1,
    StateAuthWait = 2,
    StateReady = 3
  };

  void acceptLoop();
  void serveClient(int fd);
  bool handleMessage(const uint8_t *data, size_t size);
  bool handleAuthResponse(const uint8_t *data, size_t size);
  void disconnectClient();

  bool sendRaw(const void *data, size_t size);
  template <typename T> bool sendMessage(T &message) {
    message.header.size = sizeof(T);
    return sendRaw(&message, sizeof(T));
  }
  bool sendProtoVersion();
  bool sendAuthOk();

  Logger &logger_;
  Config config_;
  std::string bindAddress_;
  uint16_t port_ = 5210;
  std::string authKey_;

  Callbacks callbacks_;
  std::atomic<bool> running_{false};
  std::atomic<int> state_{StateDisconnected};
  std::atomic<int> mute_state_{nettrx::MuteNone};
  enum AudioCodec : int { CodecNone = 0, CodecRaw = 1, CodecS16 = 2 };
  std::atomic<int> rx_codec_{CodecNone};
  std::atomic<int> tx_codec_{CodecNone};

  int listen_fd_ = -1;
  std::atomic<int> client_fd_{-1};
  mutable std::mutex write_mutex_;
  std::thread accept_thread_;
  uint8_t auth_challenge_[nettrx::kAuthChallengeLen]{};
};

} // namespace benshi
