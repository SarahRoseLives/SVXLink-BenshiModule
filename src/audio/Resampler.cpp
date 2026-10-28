// SPDX-License-Identifier: GPL-2.0-or-later
#include "Resampler.h"

#include <algorithm>

namespace benshi {

void Downsampler::setFactor(int factor) {
  factor_ = std::max(1, factor);
  reset();
}

void Downsampler::reset() {
  sum_ = 0;
  count_ = 0;
}

void Downsampler::process(const int16_t *samples, size_t count,
                          std::vector<int16_t> &out) {
  if (samples == nullptr) {
    return;
  }
  out.reserve(out.size() + count / static_cast<size_t>(factor_) + 1);
  for (size_t i = 0; i < count; ++i) {
    sum_ += samples[i];
    if (++count_ == factor_) {
      out.push_back(static_cast<int16_t>(sum_ / factor_));
      sum_ = 0;
      count_ = 0;
    }
  }
}

void Upsampler::setFactor(int factor) {
  factor_ = std::max(1, factor);
  reset();
}

void Upsampler::reset() {
  previous_ = 0;
  havePrevious_ = false;
}

void Upsampler::process(const int16_t *samples, size_t count,
                        std::vector<int16_t> &out) {
  if (samples == nullptr) {
    return;
  }
  out.reserve(out.size() + count * static_cast<size_t>(factor_));
  for (size_t i = 0; i < count; ++i) {
    const int16_t current = samples[i];
    const int16_t start = havePrevious_ ? previous_ : current;
    for (int k = 0; k < factor_; ++k) {
      const int32_t value =
          start + (static_cast<int32_t>(current) - start) * k / factor_;
      out.push_back(static_cast<int16_t>(value));
    }
    previous_ = current;
    havePrevious_ = true;
  }
}

} // namespace benshi
