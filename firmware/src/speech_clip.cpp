#include "speech_clip.h"
#include <cstring>

namespace {
uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }

const int16_t STEPS[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
  253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t INDEX_CHANGE[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct ClipLayout {
  const uint8_t *data = nullptr;
  size_t data_bytes = 0;
  size_t block = 0;
  size_t frames = 0;
};

// Walks RIFF chunks; accepts optional chunks such as "fact" and "LIST".
bool parse(const uint8_t *wav, size_t bytes, ClipLayout &clip) {
  if (!wav || bytes < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4) ||
      le32(wav + 4) != bytes - 8) return false;
  size_t declared = 0, per_block = 0;
  bool format = false, fact = false;
  for (size_t pos = 12; pos < bytes;) {
    if (bytes - pos < 8) return false;
    const size_t size = le32(wav + pos + 4);
    const uint8_t *body = wav + pos + 8;
    if (size > bytes - pos - 8) return false;
    if (!memcmp(wav + pos, "fmt ", 4)) {
      if (format || size < 20 || le16(body) != 0x11 || le16(body + 2) != 1 ||
          le32(body + 4) != SPEECH_CLIP_SAMPLE_RATE || le16(body + 14) != 4 || le16(body + 16) < 2) return false;
      clip.block = le16(body + 12);
      per_block = le16(body + 18);
      if (clip.block < 5 || per_block != (clip.block - 4) * 2 + 1) return false;
      format = true;
    } else if (!memcmp(wav + pos, "fact", 4)) {
      if (fact || size < 4) return false;
      declared = le32(body);
      fact = true;
    } else if (!memcmp(wav + pos, "data", 4)) {
      if (clip.data || !format) return false;
      clip.data = body;
      clip.data_bytes = size;
    }
    pos += 8 + size + (size % 2);
    if (pos > bytes) return false;
  }
  if (!format || !clip.data || clip.data_bytes < 4) return false;
  const size_t tail = clip.data_bytes % clip.block;
  if (tail && tail < 4) return false;
  const size_t capacity = clip.data_bytes / clip.block * per_block + (tail ? (tail - 4) * 2 + 1 : 0);
  if (fact && !declared) return false;
  // ffmpeg counts samples of a dropped final partial block; trust the data.
  clip.frames = fact && declared < capacity ? declared : capacity;
  return true;
}
}

size_t speech_clip_frames(const uint8_t *wav, size_t bytes, size_t max_frames) {
  ClipLayout clip;
  return parse(wav, bytes, clip) && clip.frames <= max_frames ? clip.frames : 0;
}

bool speech_clip_decode(const uint8_t *wav, size_t bytes, int16_t *stereo, size_t frames) {
  ClipLayout clip;
  if (!stereo || !parse(wav, bytes, clip) || clip.frames != frames) return false;
  size_t out = 0;
  for (size_t offset = 0; out < frames; offset += clip.block) {
    // Each block restarts from a stored predictor and step index.
    const uint8_t *block = clip.data + offset;
    const size_t length = clip.data_bytes - offset < clip.block ? clip.data_bytes - offset : clip.block;
    int predictor = static_cast<int16_t>(le16(block));
    int index = block[2];
    if (index > 88) return false;
    stereo[out * 2] = stereo[out * 2 + 1] = static_cast<int16_t>(predictor);
    ++out;
    for (size_t i = 4; i < length && out < frames; ++i) {
      for (int shift = 0; shift <= 4 && out < frames; shift += 4) {
        const int nibble = (block[i] >> shift) & 0x0f;
        const int step = STEPS[index];
        // Exact (2n+1)*step/8 form, matching ffmpeg's encoder and decoder.
        const int diff = ((nibble & 7) * 2 + 1) * step >> 3;
        predictor += nibble & 8 ? -diff : diff;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += INDEX_CHANGE[nibble & 7];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        stereo[out * 2] = stereo[out * 2 + 1] = static_cast<int16_t>(predictor);
        ++out;
      }
    }
  }
  return true;
}

bool speech_clip_name_valid(const char *name) {
  if (!name || !*name) return false;
  size_t length = 0;
  for (; name[length]; ++length) {
    const char c = name[length];
    if (length == 16 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}
