// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace benshi {

// Goertzel DTMF decoder for the radio's 32 kHz PCM stream. This is the same
// algorithm used by BenshiNode.
class DtmfDecoder {
public:
  using DigitCallback = std::function<void(char digit, int durationMs)>;

  static constexpr int kSampleRate = 32000;
  static constexpr int kBlockSize = 512;

  void reset();
  void process(const int16_t *samples, size_t count,
               const DigitCallback &onDigit);

  // True when the most recently processed block contained a valid tone.
  bool toneActive() const { return toneActive_; }

  static char digitForFrequencies(int lowIndex, int highIndex);

private:
  std::vector<int16_t> buffer_;
  char activeDigit_ = '\0';
  int activeBlocks_ = 0;
  int silenceBlocks_ = 0;
  bool reported_ = false;
  bool toneActive_ = false;
};

} // namespace benshi
