#pragma once
#include <stdint.h>
#include <stddef.h>





class AudioResampler48k {
 public:
  static constexpr uint32_t outputRate = 48000;
  bool begin(uint32_t sourceRate) {
    rate_ = sourceRate; phase_ = 0; have_ = false;
    return rate_ >= 8000 && rate_ <= outputRate;
  }
  template<class Sink>
  bool push(const int16_t* data, size_t frames, int channels, bool mono, Sink&& sink) {
    if (rate_ < 8000 || rate_ > outputRate || (channels != 1 && channels != 2)) return false;
    for (size_t i = 0; i < frames; ++i) {
      int16_t left = data[i * channels];
      int16_t right = channels == 2 ? data[i * channels + 1] : left;
      if (mono) left = right = int16_t((int32_t(left) + right) / 2);
      if (!have_) { left_ = left; right_ = right; have_ = true; continue; }
      if (!interval(left, right, sink)) return false;
      left_ = left; right_ = right;
    }
    return true;
  }
  template<class Sink>
  bool finish(Sink&& sink) {
    if (!have_) return true;

    bool ok = interval(left_, right_, sink);
    have_ = false;
    return ok;
  }
 private:
  template<class Sink>
  bool interval(int16_t left, int16_t right, Sink& sink) {
    while (phase_ < outputRate) {
      int16_t l = int16_t(left_ + (int64_t(left - int32_t(left_)) * phase_) / outputRate);
      int16_t r = int16_t(right_ + (int64_t(right - int32_t(right_)) * phase_) / outputRate);
      if (!sink(l, r)) return false;
      phase_ += rate_;
    }
    phase_ -= outputRate;
    return true;
  }
  uint32_t rate_ = 0, phase_ = 0;
  int16_t left_ = 0, right_ = 0;
  bool have_ = false;
};
