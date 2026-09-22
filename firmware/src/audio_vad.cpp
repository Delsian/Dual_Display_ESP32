#include "audio_vad.h"
#include <algorithm>
#include <cmath>

AudioVad::Event AudioVad::process(const uint8_t *stereo, size_t bytes, unsigned channel, bool recording) {
  if (!stereo || bytes < 4 || bytes % 4 || channel > 1) return None;
  const size_t frames = bytes / 4;
  double sum = 0, squares = 0;
  for (size_t i = 0; i < frames; ++i) {
    const uint8_t *p = stereo + i * 4 + channel * 2;
    const unsigned raw = unsigned(p[0]) | unsigned(p[1]) << 8;
    const int sample = raw < 32768 ? int(raw) : int(raw) - 65536;
    sum += sample;
    squares += double(sample) * sample;
  }
  const double mean = sum / frames;
  const float rms = std::sqrt(std::max(0.0, squares / frames - mean * mean));
  const float threshold = std::max(minimum * (recording ? 0.6f : 1.0f),
                                   noise * (recording ? 2.0f : 3.0f));
  if (recording) {
    voiced = 0;
    quiet = rms >= threshold ? 0 : quiet + frames;
    return quiet >= silence_limit ? Silence : None;
  }
  quiet = 0;
  if (rms >= threshold) {
    voiced += frames;
  } else {
    voiced = 0;
    // Adapt only to background frames, not the speech we want to detect.
    noise += 0.02f * (rms - noise);
  }
  return voiced >= start_limit ? Start : None;
}
