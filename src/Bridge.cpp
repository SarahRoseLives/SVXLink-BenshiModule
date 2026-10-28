// SPDX-License-Identifier: GPL-2.0-or-later
#include "Bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>

namespace benshi {

namespace {

std::string lowerAscii(const std::string &text) {
  std::string value;
  for (char c : text) {
    value.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return value;
}

} // namespace

Bridge::Bridge(Logger &logger, const Config &config)
    : logger_(logger), config_(config), radio_(logger, config),
      server_(logger, config) {
  const std::string mode =
      lowerAscii(config_.getString("BRIDGE", "SQUELCH_MODE", "auto"));
  if (mode == "radio") {
    squelchMode_ = SquelchMode::Radio;
    useRadioSquelch_ = true;
    useVoxSquelch_ = false;
  } else if (mode == "vox") {
    squelchMode_ = SquelchMode::Vox;
    useRadioSquelch_ = false;
    useVoxSquelch_ = true;
  } else {
    squelchMode_ = SquelchMode::Auto;
    useRadioSquelch_ = true;
    useVoxSquelch_ = true;
  }

  voxThresholdDb_ = config_.getInt("BRIDGE", "VOX_THRESHOLD_DB", -38);
  voxHangMs_ = std::max(0, config_.getInt("BRIDGE", "VOX_HANG_MS", 600));
  rxGain_ = std::pow(
      10.0, config_.getDouble("BRIDGE", "RX_GAIN_DB", 0.0) / 20.0);
  txGain_ = std::pow(
      10.0, config_.getDouble("BRIDGE", "TX_GAIN_DB", 0.0) / 20.0);
  connectRetryMs_ =
      std::max(500, config_.getInt("RADIO", "CONNECT_RETRY_MS", 5000));
  txLeadMs_ = std::clamp(config_.getInt("RADIO", "TX_LEAD_MS", 100), 0, 500);

  int linkSampleRate = config_.getInt("SVXLINK", "SAMPLE_RATE", 16000);
  if (linkSampleRate != 8000 && linkSampleRate != 16000 &&
      linkSampleRate != 32000) {
    logger_.warning("Unsupported SVXLINK/SAMPLE_RATE " +
                    std::to_string(linkSampleRate) +
                    "; using 16000 Hz");
    linkSampleRate = 16000;
  }
  const int factor = 32000 / linkSampleRate;
  downsampler_.setFactor(factor);
  upsampler_.setFactor(factor);
  logger_.info("SVXLink audio sample rate: " +
               std::to_string(linkSampleRate) + " Hz");

  const std::string txInCapture = config_.getString("LOG", "TX_CAPTURE", "");
  if (!txInCapture.empty()) {
    txInCapture_.open(txInCapture, std::ios::binary | std::ios::trunc);
    logger_.info("Capturing SVXLink TX audio to " + txInCapture);
  }
  const std::string txRadioCapture =
      config_.getString("LOG", "TX_RADIO_CAPTURE", "");
  if (!txRadioCapture.empty()) {
    txRadioCapture_.open(txRadioCapture, std::ios::binary | std::ios::trunc);
    logger_.info("Capturing radio TX audio to " + txRadioCapture);
  }
  const std::string txTimingCapture =
      config_.getString("LOG", "TX_TIMING", "");
  if (!txTimingCapture.empty()) {
    txTimingCapture_.open(txTimingCapture, std::ios::trunc);
    txTimingCapture_ << "time_us,index,count,queue,lead,keyed,mode\n";
    logger_.info("Capturing TX timing to " + txTimingCapture);
  }

  lastVoice_ = std::chrono::steady_clock::now();
  lastSiglev_ = lastVoice_;
}

Bridge::~Bridge() { stop(); }

bool Bridge::start(std::string &error) {
  if (running_.load()) {
    return true;
  }

  radio_.setStatusCallback(
      [this](const RadioStatus &status) { onRadioStatus(status); });
  radio_.setAudioPcmCallback(
      [this](const std::vector<int16_t> &pcm, bool transmit) {
        onRadioAudio(pcm, transmit);
      });
  radio_.setDisconnectCallback([this]() { onRadioDisconnected(); });

  NetTrxServer::Callbacks callbacks;
  callbacks.tx_audio = [this](const float *samples, size_t count) {
    onNetTxAudio(samples, count);
  };
  callbacks.tx_mode = [this](int mode) { onNetTxMode(mode); };
  callbacks.flush = [this]() { onNetFlush(); };
  callbacks.mute_state = [this](int state) { onNetMute(state); };
  callbacks.reset = [this]() { onNetReset(); };
  callbacks.rx_frequency = [this](uint32_t frequency) {
    logger_.debug("SVXLink requested RX frequency " +
                  std::to_string(frequency) + " Hz (ignored)");
  };
  callbacks.tx_frequency = [this](uint32_t frequency) {
    logger_.debug("SVXLink requested TX frequency " +
                  std::to_string(frequency) + " Hz (ignored)");
  };
  server_.setCallbacks(std::move(callbacks));

  if (!server_.start(error)) {
    return false;
  }

  running_.store(true);
  txThread_ = std::thread([this]() { txLoop(); });
  timerThread_ = std::thread([this]() { timerLoop(); });
  radioThread_ = std::thread([this]() { radioLoop(); });
  return true;
}

void Bridge::stop() {
  if (!running_.exchange(false)) {
    server_.stop();
    return;
  }

  txCv_.notify_all();
  if (txThread_.joinable()) {
    txThread_.join();
  }
  if (timerThread_.joinable()) {
    timerThread_.join();
  }
  if (radioThread_.joinable()) {
    radioThread_.join();
  }

  radio_.disconnect();
  server_.stop();
}

void Bridge::radioLoop() {
  while (running_.load()) {
    reconnectRequested_.store(false);
    if (radio_.isConnected() && radioReady_.load()) {
      for (int waited = 0; running_.load() && radioReady_.load() &&
                         !reconnectRequested_.load() && waited < 500;
           waited += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }

    std::string error;
    if (radio_.connect(error)) {
      radioReady_.store(true);
      txCv_.notify_all();
      logger_.info("Radio link established");
    } else {
      radioReady_.store(false);
      logger_.warning("Radio connect failed: " + error);
      for (int waited = 0; running_.load() && waited < connectRetryMs_;
           waited += 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  }
}

void Bridge::timerLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::lock_guard<std::mutex> lock(carrierMutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto hang = std::chrono::milliseconds(voxHangMs_);
    const float siglev = rssiToDb(lastStatus_.rssi);

    if (voxOpen_ && now - lastVoice_ > hang) {
      voxOpen_ = false;
    }

    if (carrierOpen_ && !radioRx_ && !voxOpen_ && now - lastVoice_ > hang) {
      carrierOpen_ = false;
      server_.sendSquelch(false, siglev, '?', "");
    }

    if (server_.hasClient() && now - lastSiglev_ >= std::chrono::seconds(1)) {
      // Re-sending the squelch state also restores NetRx's state after
      // SvxLink has temporarily muted the receiver.
      server_.sendSquelch(carrierOpen_, siglev, '?', "");
      server_.sendSiglev(siglev, '?');
      lastSiglev_ = now;
    }
  }
}

void Bridge::txLoop() {
  constexpr size_t kChunkSamples = 640;
  std::unique_lock<std::mutex> lock(txMutex_);

  while (running_.load()) {
    txCv_.wait(lock, [this]() {
      return !running_.load() ||
             (txMode_ == nettrx::TxOff && (keyed_.load() || flushPending_)) ||
             autoEndAfterDrain_ || flushPending_ ||
             (radioReady_.load() &&
              (keyed_.load() || txMode_ == nettrx::TxOn ||
               !txQueue_.empty()));
    });

    if (!running_.load()) {
      break;
    }

    if (txMode_ == nettrx::TxOff) {
      const bool wasKeyed = keyed_.exchange(false);
      const bool doFlush = flushPending_;
      flushPending_ = false;
      autoEndAfterDrain_ = false;
      leadRemaining_ = 0;
      lock.unlock();
      if (wasKeyed) {
        radio_.endTx();
        server_.sendTransmitterState(false);
      }
      if (doFlush) {
        server_.sendAllSamplesFlushed();
      }
      lock.lock();
      continue;
    }

    // RepeaterLogic switches from TX_ON to TX_AUTO when a QSO ends. If no
    // flush follows, release the transmitter once the queued audio is gone.
    if (autoEndAfterDrain_ && txQueue_.empty() && !flushPending_) {
      const bool wasKeyed = keyed_.exchange(false);
      autoEndAfterDrain_ = false;
      leadRemaining_ = 0;
      lock.unlock();
      if (wasKeyed) {
        radio_.endTx();
        server_.sendTransmitterState(false);
      }
      lock.lock();
      continue;
    }

    if (txQueue_.empty() && flushPending_ && txMode_ != nettrx::TxOn) {
      const bool wasKeyed = keyed_.exchange(false);
      flushPending_ = false;
      autoEndAfterDrain_ = false;
      leadRemaining_ = 0;
      lock.unlock();
      if (wasKeyed) {
        radio_.endTx();
        server_.sendTransmitterState(false);
      }
      server_.sendAllSamplesFlushed();
      lock.lock();
      continue;
    }

    std::array<int16_t, kChunkSamples> chunk{};
    size_t count = 0;
    if (!keyed_.load() && leadRemaining_ <= 0) {
      // Key the radio with a short burst of silence first. This lets the
      // radio's playout buffer settle without sending audio ahead of time,
      // which can overrun small firmware buffers and sound like stutter.
      leadRemaining_ = static_cast<int>(32000L * txLeadMs_ / 1000);
    }
    if (leadRemaining_ > 0) {
      count = std::min<size_t>(kChunkSamples, leadRemaining_);
      chunk.fill(0);
      leadRemaining_ -= static_cast<int>(count);
    } else if (!txQueue_.empty()) {
      count = std::min(kChunkSamples, txQueue_.size());
      for (size_t i = 0; i < count; ++i) {
        chunk[i] = txQueue_.front();
        txQueue_.pop_front();
      }
    } else if (keyed_.load() || txMode_ == nettrx::TxOn) {
      // Keep the radio's playout buffer fed with silence between network
      // audio chunks so a brief queue underrun does not sound like a stutter.
      chunk.fill(0);
      count = kChunkSamples;
    } else {
      continue;
    }
    if (txRadioCapture_.is_open()) {
      txRadioCapture_.write(reinterpret_cast<const char *>(chunk.data()),
                            static_cast<std::streamsize>(count *
                                                         sizeof(int16_t)));
    }
    if (txTimingCapture_.is_open()) {
      const auto timestamp = std::chrono::duration_cast<
          std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
      txTimingCapture_ << timestamp << ',' << txChunkIndex_ << ',' << count
                       << ',' << txQueue_.size() << ',' << leadRemaining_ << ','
                       << (keyed_.load() ? 1 : 0) << ',' << txMode_ << '\n';
    }
    ++txChunkIndex_;
    lock.unlock();

    bool ok = true;
    if (!keyed_.load()) {
      if (radio_.beginTx()) {
        keyed_.store(true);
        server_.sendTransmitterState(true);
        nextSend_ = std::chrono::steady_clock::now();
      } else {
        ok = false;
      }
    }

    if (ok && keyed_.load()) {
      ok = radio_.sendPcm(chunk.data(), count);
    }

    if (!ok) {
      if (keyed_.exchange(false)) {
        radio_.endTx();
      }
      server_.sendTransmitterState(false);
      lock.lock();
      txQueue_.clear();
      flushPending_ = false;
      autoEndAfterDrain_ = false;
      leadRemaining_ = 0;
      lock.unlock();
      radioReady_.store(false);
      reconnectRequested_.store(true);
      server_.sendAllSamplesFlushed();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      lock.lock();
      continue;
    }

    nextSend_ += std::chrono::milliseconds(20);
    const auto now = std::chrono::steady_clock::now();
    if (now > nextSend_ + std::chrono::milliseconds(200)) {
      nextSend_ = now;
    } else if (nextSend_ > now) {
      std::this_thread::sleep_until(nextSend_);
    }

    lock.lock();
    if (txQueue_.empty() && (flushPending_ || autoEndAfterDrain_)) {
      const bool doFlush = flushPending_;
      flushPending_ = false;
      autoEndAfterDrain_ = false;
      const bool shouldEnd = (txMode_ != nettrx::TxOn);
      if (shouldEnd) {
        keyed_.store(false);
        leadRemaining_ = 0;
      }
      lock.unlock();
      if (shouldEnd) {
        radio_.endTx();
        server_.sendTransmitterState(false);
      }
      if (doFlush) {
        server_.sendAllSamplesFlushed();
      }
      lock.lock();
    }
  }

  const bool wasKeyed = keyed_.exchange(false);
  leadRemaining_ = 0;
  lock.unlock();
  if (wasKeyed) {
    radio_.endTx();
    server_.sendTransmitterState(false);
  }
}

void Bridge::onRadioStatus(const RadioStatus &status) {
  std::lock_guard<std::mutex> lock(carrierMutex_);
  lastStatus_ = status;
  radioRx_ = useRadioSquelch_ && (status.inRx || status.squelchOpen);

  if (!radioRx_) {
    return;
  }

  lastVoice_ = std::chrono::steady_clock::now();
  if (!carrierOpen_) {
    carrierOpen_ = true;
    server_.sendSquelch(true, rssiToDb(status.rssi), '?', "");
  }
}

void Bridge::onRadioAudio(const std::vector<int16_t> &pcm, bool transmit) {
  if (pcm.empty() || transmit) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const double db = rmsDb(pcm.data(), pcm.size());
  bool sendAudio = false;
  {
    std::lock_guard<std::mutex> lock(carrierMutex_);
    if (useVoxSquelch_ && db >= static_cast<double>(voxThresholdDb_)) {
      voxOpen_ = true;
      lastVoice_ = now;
    }

    if (carrierOpen_ || radioRx_ || voxOpen_) {
      if (!carrierOpen_) {
        carrierOpen_ = true;
        server_.sendSquelch(true, rssiToDb(lastStatus_.rssi), '?', "");
      }
      if (radioRx_ || voxOpen_) {
        lastVoice_ = now;
      }
      sendAudio = true;
    }
  }

  dtmfDecoder_.process(pcm.data(), pcm.size(),
                       [this](char digit, int durationMs) {
                         logger_.info(std::string("DTMF digit ") + digit +
                                      " (" + std::to_string(durationMs) +
                                      " ms)");
                         server_.sendDtmf(digit, durationMs);
                       });
  if (dtmfDecoder_.toneActive()) {
    dtmfMuteUntil_ = now + std::chrono::milliseconds(200);
  }
  const bool muteDtmf = dtmfDecoder_.toneActive() || now < dtmfMuteUntil_;

  if (!sendAudio || muteDtmf) {
    return;
  }

  std::vector<int16_t> gained;
  gained.reserve(pcm.size());
  for (int16_t sample : pcm) {
    gained.push_back(applyGain(sample, rxGain_));
  }

  std::vector<int16_t> downsampled;
  downsampler_.process(gained.data(), gained.size(), downsampled);
  if (downsampled.empty()) {
    return;
  }

  std::vector<float> floats;
  floats.reserve(downsampled.size());
  for (int16_t sample : downsampled) {
    floats.push_back(static_cast<float>(sample) / 32768.0f);
  }
  server_.sendRxAudio(floats.data(), floats.size());
}

void Bridge::onRadioDisconnected() {
  logger_.warning("Radio link lost; waiting for it to come back");

  {
    std::lock_guard<std::mutex> lock(carrierMutex_);
    radioRx_ = false;
    voxOpen_ = false;
    if (carrierOpen_) {
      carrierOpen_ = false;
      server_.sendSquelch(false, rssiToDb(lastStatus_.rssi), '?', "");
    }
    downsampler_.reset();
    dtmfDecoder_.reset();
  }

  radioReady_.store(false);

  {
    std::lock_guard<std::mutex> lock(txMutex_);
    txQueue_.clear();
    flushPending_ = false;
    autoEndAfterDrain_ = false;
    leadRemaining_ = 0;
  }
  txCv_.notify_all();

  if (keyed_.exchange(false)) {
    radio_.endTx();
  }
  server_.sendTransmitterState(false);
  server_.sendAllSamplesFlushed();
}

void Bridge::onNetTxAudio(const float *samples, size_t count) {
  if (samples == nullptr || count == 0) {
    return;
  }

  std::vector<int16_t> pcm(count);
  for (size_t i = 0; i < count; ++i) {
    double value = static_cast<double>(samples[i]) * txGain_;
    value = std::clamp(value, -1.0, 1.0);
    pcm[i] = static_cast<int16_t>(std::lround(value * 32767.0));
  }

  if (txInCapture_.is_open()) {
    txInCapture_.write(reinterpret_cast<const char *>(pcm.data()),
                       static_cast<std::streamsize>(pcm.size() *
                                                    sizeof(int16_t)));
  }

  std::vector<int16_t> upsampled;
  upsampler_.process(pcm.data(), pcm.size(), upsampled);

  if (!radioReady_.load()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(txMutex_);
    if (txMode_ == nettrx::TxOff) {
      return;
    }
    const size_t maxSamples = static_cast<size_t>(32000) * 10;
    if (txQueue_.size() + upsampled.size() > maxSamples) {
      const size_t excess =
          txQueue_.size() + upsampled.size() - maxSamples;
      const size_t drop = std::min(excess, txQueue_.size());
      txQueue_.erase(txQueue_.begin(),
                     txQueue_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    txQueue_.insert(txQueue_.end(), upsampled.begin(), upsampled.end());
  }
  txCv_.notify_one();
}

void Bridge::onNetTxMode(int mode) {
  if (mode < nettrx::TxOff || mode > nettrx::TxAuto) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(txMutex_);
    const int previous = txMode_;
    txMode_ = mode;
    if (mode == nettrx::TxOff) {
      txQueue_.clear();
      autoEndAfterDrain_ = false;
    } else if (mode == nettrx::TxAuto) {
      autoEndAfterDrain_ = (previous == nettrx::TxOn);
    } else {
      autoEndAfterDrain_ = false;
    }
  }
  txCv_.notify_all();
}

void Bridge::onNetFlush() {
  if (!radioReady_.load()) {
    server_.sendAllSamplesFlushed();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(txMutex_);
    flushPending_ = true;
  }
  txCv_.notify_all();
}

void Bridge::onNetMute(int state) {
  logger_.debug("SVXLink RX mute state " + std::to_string(state));
}

void Bridge::onNetReset() {
  onNetTxMode(nettrx::TxOff);
  {
    std::lock_guard<std::mutex> lock(carrierMutex_);
    radioRx_ = false;
    voxOpen_ = false;
    if (carrierOpen_) {
      carrierOpen_ = false;
      server_.sendSquelch(false, rssiToDb(lastStatus_.rssi), '?', "");
    }
  }
}

double Bridge::rmsDb(const int16_t *samples, size_t count) {
  if (samples == nullptr || count == 0) {
    return -200.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double value = static_cast<double>(samples[i]) / 32768.0;
    sum += value * value;
  }
  const double rms = std::sqrt(sum / static_cast<double>(count));
  if (rms <= 1e-9) {
    return -200.0;
  }
  return 20.0 * std::log10(rms);
}

float Bridge::rssiToDb(int rssi) {
  const int clamped = std::clamp(rssi, 0, 15);
  return static_cast<float>(-100 + clamped * 3);
}

int16_t Bridge::applyGain(int16_t sample, double gain) {
  const double value = static_cast<double>(sample) * gain;
  return static_cast<int16_t>(
      std::clamp(std::lround(value), -32768L, 32767L));
}

} // namespace benshi
