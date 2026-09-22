#ifndef AUDIO_VAD_H
#define AUDIO_VAD_H
#include <stddef.h>
#include <stdint.h>

class AudioVad {
 public:
  enum Event { None, Start, Silence };
  AudioVad(float minimum_rms, uint32_t start_samples, uint32_t silence_samples)
      : minimum(minimum_rms), start_limit(start_samples), silence_limit(silence_samples) {}
  Event process(const uint8_t *stereo, size_t bytes, unsigned channel, bool recording);
  void reset() { voiced = quiet = 0; }
 private:
  float minimum;
  float noise = 100;
  uint32_t start_limit, silence_limit;
  uint32_t voiced = 0, quiet = 0;
};
#endif
