# Current project status

Updated: 2026-09-22. Scope: voice AI workflow.

## Implemented and validated

- Microphone phrase → Worker `/ask` → Gemini text reply in serial works on
  hardware, per user logs. Actual reply speech is now implemented locally.
- Reply speech: firmware build and mocked Worker validation passed; deployment
  and end-to-end hardware playback of actual answers remain pending.
- Fixed-phrase `/test-speech` download and speaker playback work on hardware.
- HTTPS connection reuse works: user logs showed zero connection setup time
  for reused connections, with total replies around 2.3–3.4 seconds in those
  samples. Fresh connection setup took about 2.8 seconds; timings vary.
- Voice activation is implemented; user confirmed “It works now.” Defaults:
  80 ms onset, up to 200 ms pre-roll, 800 ms silence cutoff, five-second maximum
  including pre-roll, and one-second cooldown. KEY1 remains available.
- Automatic capture pauses while offline, during requests and during playback.
- Latest voice-activation firmware build passed. Native detector tests passed
  with address/undefined-behavior sanitizers; WAV tests passed previously.
  Individual hardware edge cases were not separately reported by the user.

## Current configuration and limits

- Local API key updated; deployed Worker secret still needs updating manually.
- `/ask` now includes English/Ukrainian-only system instructions in source;
  redeployment and language behavior validation remain pending.
- `AUDIO_AI_REPLY_TEST=1`, `AUDIO_VOICE_ACTIVATION=1`, microphone channel 0.
- Worker source selects `gemini-3.6-flash` for replies and
  `gemini-2.5-flash-preview-tts` for fixed speech; no new pricing validation.
- Energy detection can trigger on background sounds; tune `AUDIO_VAD_MIN_RMS`
  in `include/config.h`. This is not wake-word detection.
- Upload starts after recording; no streaming capture/upload implemented.
- Wi-Fi sleep remains enabled for BLE coexistence after a confirmed abort
  when sleep was disabled. Do not reintroduce that optimization.

## Pending work

- Deploy Worker `/speak` and upload firmware; validate actual English/Ukrainian
  replies, TTS failures, and automatic capture resuming after playback.
- User also requested exploring upload while recording and minimal model cost.
  Streaming and model changes remain pending; settle the approach before coding.
- Validate each next step on hardware, including no speaker-triggered capture,
  cutoff behavior, background noise, and continued animation.
- No blocker is currently reported. Read [SPEECH.md](SPEECH.md) for operating
  steps and timing definitions; update this summary after important changes.
