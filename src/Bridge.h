// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Config.h"
#include "Logger.h"
#include "audio/DtmfDecoder.h"
#include "audio/Resampler.h"
#include "net/NetTrxServer.h"
#include "radio/RadioSession.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace benshi {

// Connects a Benshi/VR-N76 radio to SvxLink through SvxLink's native NetTrx
// transceiver protocol.
class Bridge {
public:
  Bridge(Logger &logger, const Config &config);
  ~Bridge();

  Bridge(const Bridge &) = delete;
  Bridge &operator=(const Bridge &) = delete;

  bool start(std::string &error);
  void stop();

private:
  enum class SquelchMode { Radio, Vox, Auto };

  void radioLoop();
  void timerLoop();
  void txLoop();

  void onRadioStatus(const RadioStatus &status);
  void onRadioAudio(const std::vector<int16_t> &pcm, bool transmit);
  void onRadioDisconnected();

  void onNetTxAudio(const float *samples, size_t count);
  void onNetTxMode(int mode);
  void onNetFlush();
  void onNetMute(int state);
  void onNetReset();

  static double rmsDb(const int16_t *samples, size_t count);
  static float rssiToDb(int rssi);
  static int16_t applyGain(int16_t sample, double gain);

  Logger &logger_;
  Config config_;
  RadioSession radio_;
  NetTrxServer server_;

  SquelchMode squelchMode_ = SquelchMode::Auto;
  bool useRadioSquelch_ = true;
  bool useVoxSquelch_ = true;
  int voxThresholdDb_ = -38;
  int voxHangMs_ = 600;
  double rxGain_ = 1.0;
  double txGain_ = 1.0;
  int connectRetryMs_ = 5000;
  int txLeadMs_ = 100;

  std::atomic<bool> running_{false};
  std::atomic<bool> radioReady_{false};
  std::atomic<bool> reconnectRequested_{false};
  std::thread radioThread_;
  std::thread timerThread_;
  std::thread txThread_;

  std::mutex carrierMutex_;
  bool carrierOpen_ = false;
  bool voxOpen_ = false;
  bool radioRx_ = false;
  RadioStatus lastStatus_;
  std::chrono::steady_clock::time_point lastVoice_;
  std::chrono::steady_clock::time_point lastSiglev_;

  DtmfDecoder dtmfDecoder_;
  std::chrono::steady_clock::time_point dtmfMuteUntil_;
  Downsampler downsampler_;
  Upsampler upsampler_;

  std::mutex txMutex_;
  std::condition_variable txCv_;
  std::deque<int16_t> txQueue_;
  int txMode_ = nettrx::TxOff;
  bool flushPending_ = false;
  bool autoEndAfterDrain_ = false;
  int leadRemaining_ = 0;
  std::atomic<bool> keyed_{false};
  std::chrono::steady_clock::time_point nextSend_;

  std::ofstream txInCapture_;
  std::ofstream txRadioCapture_;
  std::ofstream txTimingCapture_;
  uint64_t txChunkIndex_ = 0;
};

} // namespace benshi
