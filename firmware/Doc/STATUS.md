# Current project status

Updated: 2026-09-25. Scope: voice AI workflow.

## Implemented and validated

- Microphone → Worker `/ask` → Gemini serial reply works on hardware, per user logs.
- Reply speech: user logs confirm successful TTS and playback on hardware.
- Keep-alive success confirmed in user logs; three-minute close validation pending.
- Detailed Gemini and speech timings confirmed in hardware logs.
- HTTPS connection reuse works: user logs showed zero connection setup time
  for reused connections, with total replies around 2.3–3.4 seconds in those
  samples. Fresh connection setup took about 2.8 seconds; timings vary.
- Voice activation is implemented; user confirmed “It works now.” Defaults:
  80 ms onset, up to 200 ms pre-roll, 800 ms silence cutoff, five-second maximum
  including pre-roll, and one-second cooldown. KEY1 remains available.
- Automatic capture pauses while offline, during requests and during playback.
- Voice-activation build and sanitized detector tests passed; WAV tests also pass.
  Individual hardware edge cases were not separately reported by the user.

## Current configuration and limits

- Unrecognized speech now maps to an empty answer (no TTS); Worker redeployment
  and real-audio validation pending.
- `/ask` now includes English/Ukrainian-only system instructions in source;
  redeployment and language behavior validation remain pending.
- `AUDIO_AI_REPLY_TEST=1`, `AUDIO_VOICE_ACTIVATION=1`, microphone channel 0.
- Worker uses Gemini 3.6 Flash only; TTS removed 2026-09-25 (5 Worker tests pass).
- Energy detection can trigger on background sounds; tune `AUDIO_VAD_MIN_RMS`
  in `include/config.h`. This is not wake-word detection.
- Wi-Fi sleep remains enabled for BLE coexistence after a confirmed abort
  when sleep was disabled. Do not reintroduce that optimization.

## Pending work

- 2026-09-25: generation/decoding/playback now use 24 kHz mono IMA ADPCM;
  I2S restores 16 kHz after clips. Firmware and filesystem builds pass;
  sanitized decoder tests pass and all 24 regenerated clips match ffmpeg exactly.
  24 of 60 clips available; data/ uses 2.02 of 3.38 MB. Add missing phrases.
  Upload FS + firmware; hardware validation pending: pitch/speed, KEY1 interruption,
  recording after playback, and latency. Serial `speech [N|off_K]` plays local clips.
- Upload keep-alive firmware; verify reuse after 30–60 seconds, close after three
  minutes, no further pings until a new request, and recovery after Wi-Fi loss.
- Cheaper models: pending. Validate on hardware: no speaker-triggered capture,
  cutoff behavior, background noise, and continued animation.
- 2026-09-23: combined `/reply` and playback during download work on hardware.
- 2026-09-23: upload while recording (chunked L16) implemented; 7 Worker tests and
  build pass. Deploy the Worker first. Hardware pending: `upload_tail` near zero,
  Worker `upload_prepare` ≈ recording length, short KEY1 press cancels.
