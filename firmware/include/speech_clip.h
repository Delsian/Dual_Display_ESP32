#ifndef SPEECH_CLIP_H
#define SPEECH_CLIP_H

#include <stddef.h>
#include <stdint.h>

constexpr uint32_t SPEECH_CLIP_SAMPLE_RATE = 16000;

// Prerecorded replies are 16 kHz mono IMA ADPCM WAV files (format 0x11,
// 4 bits per sample), a quarter of the PCM size. Returns total frames, or zero
// for an unsupported file or one longer than max_frames.
size_t speech_clip_frames(const uint8_t *wav, size_t bytes, size_t max_frames);
// Decodes into stereo PCM; `stereo` must hold speech_clip_frames() frames.
bool speech_clip_decode(const uint8_t *wav, size_t bytes, int16_t *stereo, size_t frames);
// Continuous IMA ADPCM for the BLE upload (see Doc/BLE.md). Each packet carries
// the state before its first sample, so a lost packet does not corrupt the next.
struct SpeechAdpcmState {
  int16_t predictor = 0;
  uint8_t index = 0;
};
// Encodes `count` samples read every `stride` values (2 selects one stereo
// channel) into (count + 1) / 2 bytes, low nibble first.
void speech_adpcm_encode(SpeechAdpcmState &state, const int16_t *samples, size_t stride,
                         size_t count, uint8_t *out);
// Inverse of speech_adpcm_encode; false for an invalid step index.
bool speech_adpcm_decode(SpeechAdpcmState &state, const uint8_t *in, size_t count, int16_t *samples);
// Worker clip names: 1-16 lowercase letters, digits, or underscores.
bool speech_clip_name_valid(const char *name);

#endif
