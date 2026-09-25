#ifndef SPEECH_CLIP_H
#define SPEECH_CLIP_H

#include <stddef.h>
#include <stdint.h>

constexpr uint32_t SPEECH_CLIP_SAMPLE_RATE = 24000;

// Prerecorded replies are 24 kHz mono IMA ADPCM WAV files (format 0x11,
// 4 bits per sample), a quarter of the PCM size. Returns total frames, or zero
// for an unsupported file or one longer than max_frames.
size_t speech_clip_frames(const uint8_t *wav, size_t bytes, size_t max_frames);
// Decodes into stereo PCM; `stereo` must hold speech_clip_frames() frames.
bool speech_clip_decode(const uint8_t *wav, size_t bytes, int16_t *stereo, size_t frames);
// Worker clip names: 1-16 lowercase letters, digits, or underscores.
bool speech_clip_name_valid(const char *name);

#endif
