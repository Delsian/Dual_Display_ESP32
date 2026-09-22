#include "audio_vad.h"
#include <cassert>
#include <cstdio>
#include <vector>

static std::vector<uint8_t> block(int amplitude, unsigned channel = 0, int dc = 0) {
  std::vector<uint8_t> data(1024);
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t sample = uint16_t(dc + (i % 2 ? amplitude : -amplitude));
    data[i * 4 + channel * 2] = sample & 255;
    data[i * 4 + channel * 2 + 1] = sample >> 8;
  }
  return data;
}

int main() {
  AudioVad vad(600, 1280, 12800);
  auto quiet = block(0), speech = block(2000), dc = block(0, 0, 10000);
  auto feed = [&](const std::vector<uint8_t>& data, bool recording = false) {
    return vad.process(data.data(), data.size(), 0, recording);
  };
  for (int i = 0; i < 100; ++i) assert(feed(dc) == AudioVad::None);
  assert(feed(speech) == AudioVad::None); // A brief click cannot start capture.
  assert(feed(quiet) == AudioVad::None);
  for (int i = 0; i < 4; ++i) assert(feed(speech) == AudioVad::None);
  assert(feed(speech) == AudioVad::Start);
  vad.reset();
  for (int i = 0; i < 49; ++i) assert(feed(quiet, true) == AudioVad::None);
  assert(feed(speech, true) == AudioVad::None); // Speech restarts the silence timer.
  for (int i = 0; i < 49; ++i) assert(feed(quiet, true) == AudioVad::None);
  assert(feed(quiet, true) == AudioVad::Silence);
  vad.reset();
  auto other = block(2000, 1);
  for (int i = 0; i < 10; ++i) assert(feed(other) == AudioVad::None);
  for (int i = 0; i < 4; ++i)
    assert(vad.process(other.data(), other.size(), 1, false) == AudioVad::None);
  assert(vad.process(other.data(), other.size(), 1, false) == AudioVad::Start);
  vad.reset();
  assert(feed(speech) == AudioVad::None);
  vad.reset();
  for (int i = 0; i < 4; ++i) assert(feed(speech) == AudioVad::None);
  assert(vad.process(nullptr, 1024, 0, false) == AudioVad::None);
  assert(vad.process(speech.data(), 3, 0, false) == AudioVad::None);
  assert(vad.process(speech.data(), speech.size(), 2, false) == AudioVad::None);
  std::puts("Audio VAD tests passed");
}
