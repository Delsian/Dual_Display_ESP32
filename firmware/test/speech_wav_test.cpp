// Run: g++ -std=c++11 -Iinclude src/speech_wav.cpp test/speech_wav_test.cpp -o /tmp/speech_wav_test && /tmp/speech_wav_test
#include "speech_wav.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

static std::vector<uint8_t> tone(double hz, int constant = 0) {
  std::vector<uint8_t> wav(44 + 24000 * 2);
  auto put16 = [&](size_t offset, uint16_t value) {
    wav[offset] = value; wav[offset + 1] = value >> 8;
  };
  auto put32 = [&](size_t offset, uint32_t value) {
    put16(offset, value); put16(offset + 2, value >> 16);
  };
  memcpy(wav.data(), "RIFF", 4); put32(4, wav.size() - 8);
  memcpy(wav.data() + 8, "WAVEfmt ", 8); put32(16, 16);
  put16(20, 1); put16(22, 1); put32(24, 24000); put32(28, 48000);
  put16(32, 2); put16(34, 16);
  memcpy(wav.data() + 36, "data", 4); put32(40, wav.size() - 44);
  for (int i = 0; i < 24000; ++i) {
    int16_t value = constant ? constant : std::lround(10000 * std::sin(2 * 3.141592653589793 * hz * i / 24000));
    put16(44 + i * 2, static_cast<uint16_t>(value));
  }
  return wav;
}

static double convert_rms(const std::vector<uint8_t> &wav) {
  const size_t frames = speech_wav_frames(wav.data(), wav.size());
  assert(frames > 200);
  std::vector<int16_t> out(frames * 2);
  assert(speech_wav_convert(wav.data(), wav.size(), out.data(), frames));
  double sum = 0;
  for (size_t i = 0; i < frames; ++i) {
    assert(out[2 * i] == out[2 * i + 1]);
    if (i >= 100 && i < frames - 100) sum += double(out[2 * i]) * out[2 * i];
  }
  return std::sqrt(sum / (frames - 200));
}

int main(int argc, char **argv) {
  std::vector<uint8_t> mic(16000);
  for (size_t i = 0; i < mic.size(); i += 4) {
    mic[i] = 0x34; mic[i + 1] = 0x12;
    mic[i + 2] = 0x00; mic[i + 3] = 0x80;
  }
  std::vector<uint8_t> upload(8044);
  for (unsigned channel = 0; channel < 2; ++channel) {
    assert(recording_wav(mic.data(), mic.size(), channel, upload.data(), upload.size()));
    assert(upload[24] == 0x80 && upload[25] == 0x3e); // 16000 Hz
    assert(upload[22] == 1 && upload[34] == 16);
    for (size_t i = 44; i < upload.size(); i += 2) {
      assert(upload[i] == mic[channel * 2] && upload[i + 1] == mic[channel * 2 + 1]);
    }
  }
  assert(!recording_wav(mic.data(), 15996, 0, upload.data(), upload.size()));
  assert(!recording_wav(mic.data(), mic.size(), 2, upload.data(), upload.size()));
  assert(!recording_wav(mic.data(), mic.size(), 0, upload.data(), upload.size() - 1));
  auto wav = tone(1000);
  assert(speech_wav_frames(wav.data(), wav.size()) == 16000);
  assert(convert_rms(wav) > 6500 && convert_rms(wav) < 7500);
  assert(convert_rms(tone(10000)) < 100); // Reject frequencies that would alias.
  assert(std::abs(convert_rms(tone(0, -12345)) - 12345) < 1);
  assert(speech_wav_frames(nullptr, 100) == 0);
  assert(speech_wav_frames(wav.data(), 43) == 0);
  assert(speech_wav_frames(wav.data(), wav.size() - 2) == 0);
  for (int offset : {0, 4, 8, 16, 20, 22, 24, 28, 32, 34, 36, 40}) {
    auto invalid = wav; invalid[offset] ^= 1;
    assert(speech_wav_frames(invalid.data(), invalid.size()) == 0);
  }
  std::vector<int16_t> out(32000);
  assert(!speech_wav_convert(wav.data(), wav.size(), out.data(), 15999));
  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary);
    assert(file.good());
    std::vector<uint8_t> live((std::istreambuf_iterator<char>(file)), {});
    assert(convert_rms(live) > 0);
  }
}
