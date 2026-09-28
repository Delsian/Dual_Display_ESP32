// Run: g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/speech_clip.cpp test/speech_clip_test.cpp -o /tmp/speech_clip_test && /tmp/speech_clip_test
// Optional reference check: /tmp/speech_clip_test clip.wav clip.s16le, where the
// second file is `ffmpeg -i clip.wav -f s16le -acodec pcm_s16le clip.s16le`.
#include "speech_clip.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

static std::vector<uint8_t> clip(const std::vector<uint8_t> &data, uint16_t block, uint32_t fact) {
  std::vector<uint8_t> wav(12 + 28 + 12 + 8 + data.size() + data.size() % 2);
  auto put16 = [&](size_t offset, uint16_t value) { wav[offset] = value; wav[offset + 1] = value >> 8; };
  auto put32 = [&](size_t offset, uint32_t value) { put16(offset, value); put16(offset + 2, value >> 16); };
  memcpy(wav.data(), "RIFF", 4); put32(4, wav.size() - 8); memcpy(wav.data() + 8, "WAVE", 4);
  memcpy(wav.data() + 12, "fmt ", 4); put32(16, 20);
  put16(20, 0x11); put16(22, 1); put32(24, 16000); put32(28, 16000 * block / ((block - 4) * 2 + 1));
  put16(32, block); put16(34, 4); put16(36, 2); put16(38, (block - 4) * 2 + 1);
  memcpy(wav.data() + 40, "fact", 4); put32(44, 4); put32(48, fact);
  memcpy(wav.data() + 52, "data", 4); put32(56, data.size());
  memcpy(wav.data() + 60, data.data(), data.size());
  return wav;
}

static std::vector<int16_t> decode(const std::vector<uint8_t> &wav) {
  const size_t frames = speech_clip_frames(wav.data(), wav.size(), 160000);
  assert(frames);
  std::vector<int16_t> stereo(frames * 2);
  assert(speech_clip_decode(wav.data(), wav.size(), stereo.data(), frames));
  std::vector<int16_t> mono(frames);
  for (size_t i = 0; i < frames; ++i) {
    assert(stereo[i * 2] == stereo[i * 2 + 1]);
    mono[i] = stereo[i * 2];
  }
  return mono;
}

int main(int argc, char **argv) {
  // Header sample, then low nibble before high nibble.
  auto wav = clip({0, 0, 0, 0, 0x07, 0x00}, 8, 3);
  assert((decode(wav) == std::vector<int16_t>{0, 13, 15}));
  // Second block restarts from its own predictor; the short last block is valid.
  wav = clip({0, 0, 0, 0, 0, 0, 0, 0, 0xe8, 0x03, 88, 0, 0x08}, 8, 11);
  auto samples = decode(wav);
  assert(samples.size() == 11 && samples[9] == 1000);
  assert(samples[10] == 1000 - 32767 / 8); // Largest step, negative.
  // Predictor clamps to the 16-bit range.
  wav = clip({0x00, 0x80, 88, 0, 0x88}, 5, 3);
  assert((decode(wav) == std::vector<int16_t>{-32768, -32768, -32768}));

  auto base = clip({0, 0, 0, 0, 0x07, 0x00}, 8, 3);
  auto old_rate = base;
  old_rate[24] = 0xc0; old_rate[25] = 0x5d; // Old 24 kHz clips must be regenerated.
  assert(speech_clip_frames(old_rate.data(), old_rate.size(), 160000) == 0);
  // Ten seconds at 16 kHz fits; one extra frame exceeds the playback limit.
  auto long_clip = clip(std::vector<uint8_t>(79 * 1024), 1024, 160000);
  assert(decode(long_clip).size() == 160000);
  long_clip[48] += 1;
  assert(speech_clip_frames(long_clip.data(), long_clip.size(), 160000) == 0);
  assert(speech_clip_frames(base.data(), base.size(), 2) == 0);
  assert(speech_clip_frames(base.data(), base.size() - 1, 160000) == 0);
  assert(speech_clip_frames(nullptr, 0, 160000) == 0);
  for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(20), size_t(22), size_t(24), size_t(34), size_t(38), size_t(52)}) {
    auto invalid = base; invalid[offset] ^= 1;
    assert(speech_clip_frames(invalid.data(), invalid.size(), 160000) == 0);
  }
  auto too_many = clip({0, 0, 0, 0, 0x07, 0x00}, 8, 6); // Capacity is 5.
  assert(speech_clip_frames(too_many.data(), too_many.size(), 160000) == 5);
  auto zero = clip({0, 0, 0, 0, 0x07, 0x00}, 8, 0);
  assert(speech_clip_frames(zero.data(), zero.size(), 160000) == 0);
  auto bad_index = clip({0, 0, 89, 0, 0x07, 0x00}, 8, 3);
  std::vector<int16_t> out(6);
  assert(!speech_clip_decode(bad_index.data(), bad_index.size(), out.data(), 3));
  assert(!speech_clip_decode(base.data(), base.size(), out.data(), 2));

  // BLE upload codec: encode one channel of stereo capture in packets, decode
  // each packet from its own header state, and check the reconstruction.
  {
    std::vector<int16_t> stereo(2 * 16000);
    for (size_t i = 0; i < 16000; ++i) {
      stereo[i * 2] = 30000; // Other channel must be ignored.
      stereo[i * 2 + 1] = static_cast<int16_t>(12000 * std::sin(2 * 3.141592653589793 * 440 * i / 16000.0) +
                                               (i % 3000 < 50 ? 18000 : 0)); // Tone plus clicks.
    }
    SpeechAdpcmState encoder, decoder;
    std::vector<uint8_t> packet(160);
    std::vector<int16_t> decoded(16000);
    double error = 0, signal = 0;
    for (size_t at = 0; at < 16000; at += 319) { // Odd packet sizes exercise nibble packing.
      const size_t count = std::min<size_t>(319, 16000 - at);
      const SpeechAdpcmState header = encoder;
      speech_adpcm_encode(encoder, stereo.data() + at * 2 + 1, 2, count, packet.data());
      decoder = header;
      assert(speech_adpcm_decode(decoder, packet.data(), count, decoded.data() + at));
      assert(decoder.predictor == encoder.predictor && decoder.index == encoder.index);
    }
    for (size_t i = 0; i < 16000; ++i) {
      const double diff = decoded[i] - stereo[i * 2 + 1];
      error += diff * diff;
      signal += double(stereo[i * 2 + 1]) * stereo[i * 2 + 1];
    }
    assert(10 * std::log10(signal / error) > 20); // At least 20 dB SNR.
    SpeechAdpcmState invalid;
    invalid.index = 89;
    assert(!speech_adpcm_decode(invalid, packet.data(), 2, decoded.data()));
  }

  assert(speech_clip_name_valid("042") && speech_clip_name_valid("off_3"));
  for (const char *name : {"", "../x", "A1", "a.wav", "a/b", "abcdefghijklmnopq"}) {
    assert(!speech_clip_name_valid(name));
  }
  assert(!speech_clip_name_valid(nullptr));

  if (argc == 3) {
    std::ifstream file(argv[1], std::ios::binary), raw(argv[2], std::ios::binary);
    assert(file.good() && raw.good());
    std::vector<uint8_t> live((std::istreambuf_iterator<char>(file)), {});
    std::vector<uint8_t> pcm((std::istreambuf_iterator<char>(raw)), {});
    const auto decoded = decode(live);
    // ffmpeg ignores "fact" and also decodes the final block's padding.
    assert(decoded.size() * 2 <= pcm.size() && decoded.size() * 2 + 4096 > pcm.size());
    for (size_t i = 0; i < decoded.size(); ++i) {
      assert(decoded[i] == int16_t(pcm[i * 2] | pcm[i * 2 + 1] << 8));
    }
  }
}
