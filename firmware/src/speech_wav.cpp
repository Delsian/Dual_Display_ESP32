#include "speech_wav.h"
#include <cmath>
#include <cstring>

namespace {
uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
}

bool recording_wav(const uint8_t *stereo, size_t bytes, unsigned channel,
                   uint8_t *wav, size_t wav_bytes) {
  if (!stereo || !wav || channel > 1 || bytes < 16000 || bytes > 1920000 ||
      bytes % 4 || wav_bytes != 44 + bytes / 2) return false;
  auto put16 = [&](size_t offset, uint16_t value) {
    wav[offset] = value; wav[offset + 1] = value >> 8;
  };
  auto put32 = [&](size_t offset, uint32_t value) {
    put16(offset, value); put16(offset + 2, value >> 16);
  };
  memcpy(wav, "RIFF", 4); put32(4, wav_bytes - 8);
  memcpy(wav + 8, "WAVEfmt ", 8); put32(16, 16);
  put16(20, 1); put16(22, 1); put32(24, 16000); put32(28, 32000);
  put16(32, 2); put16(34, 16);
  memcpy(wav + 36, "data", 4); put32(40, wav_bytes - 44);
  for (size_t i = 0; i < bytes / 4; ++i) {
    wav[44 + i * 2] = stereo[i * 4 + channel * 2];
    wav[45 + i * 2] = stereo[i * 4 + channel * 2 + 1];
  }
  return true;
}

size_t speech_wav_frames(const uint8_t *wav, size_t bytes) {
  if (!wav || bytes < 48 || bytes > 480044 ||
      memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVEfmt ", 8) ||
      memcmp(wav + 36, "data", 4) || le32(wav + 4) != bytes - 8 ||
      le32(wav + 16) != 16 || le16(wav + 20) != 1 || le16(wav + 22) != 1 ||
      le32(wav + 24) != 24000 || le32(wav + 28) != 48000 ||
      le16(wav + 32) != 2 || le16(wav + 34) != 16 ||
      le32(wav + 40) != bytes - 44 || (bytes - 44) % 2) return 0;
  return ((bytes - 44) / 2) * 2 / 3;
}

bool speech_wav_convert(const uint8_t *wav, size_t bytes, int16_t *stereo,
                        size_t output_frames) {
  if (!stereo || !output_frames || speech_wav_frames(wav, bytes) != output_frames) return false;
  // Two fractional phases for 24 -> 16 kHz. Low-pass before decimation to
  // suppress frequencies above the output Nyquist limit; keep codec clocks fixed.
  constexpr int RADIUS = 15;
  constexpr double PI = 3.14159265358979323846;
  double taps[2][2 * RADIUS + 1];
  for (int phase = 0; phase < 2; ++phase) {
    double sum = 0;
    for (int k = -RADIUS; k <= RADIUS; ++k) {
      const double x = k - phase * 0.5;
      const double sinc = std::abs(x) < 1e-9 ? 0.6 : std::sin(0.6 * PI * x) / (PI * x);
      taps[phase][k + RADIUS] = sinc * (0.54 + 0.46 * std::cos(PI * k / RADIUS));
      sum += taps[phase][k + RADIUS];
    }
    for (double &tap : taps[phase]) tap /= sum;
  }
  const int input_frames = (bytes - 44) / 2;
  for (size_t n = 0; n < output_frames; ++n) {
    const int center = n * 3 / 2;
    double value = 0;
    for (int k = -RADIUS; k <= RADIUS; ++k) {
      int index = center + k;
      if (index < 0) index = 0;
      if (index >= input_frames) index = input_frames - 1;
      const uint16_t raw = le16(wav + 44 + index * 2);
      const int sample = raw < 32768 ? raw : int(raw) - 65536;
      value += sample * taps[n % 2][k + RADIUS];
    }
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    stereo[n * 2] = stereo[n * 2 + 1] = static_cast<int16_t>(std::lround(value));
  }
  return true;
}
