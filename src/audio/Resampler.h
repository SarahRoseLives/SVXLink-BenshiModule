// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace benshi {

// Integer-ratio resampler between the radio's 32 kHz PCM and SvxLink's
// internal sample rate. SvxLink's current default is 16000 Hz; older builds
// and some distributions use 8000 Hz.
class Downsampler {
public:
  explicit Downsampler(int factor = 4) : factor_(factor) {}

  void setFactor(int factor);
  int factor() const { return factor_; }

  void reset();
  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &out);

private:
  int factor_ = 4;
  int32_t sum_ = 0;
  int count_ = 0;
};

class Upsampler {
public:
  explicit Upsampler(int factor = 4) : factor_(factor) {}

  void setFactor(int factor);
  int factor() const { return factor_; }

  void reset();
  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &out);

private:
  int factor_ = 4;
  int16_t previous_ = 0;
  bool havePrevious_ = false;
};

} // namespace benshi
