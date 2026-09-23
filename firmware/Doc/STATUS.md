# Current project status

Updated: 2026-09-23. Scope: voice AI workflow.

## Implemented and validated

- Microphone → `/ask` → Gemini serial reply works on hardware, per user logs.
- Reply speech: user logs confirm successful TTS and playback on hardware.
- Keep-alive success confirmed in user logs; three-minute close validation pending.
- Detailed Gemini headers/body and speech timing added; deployment and hardware
  timing validation pending. Mocked Worker timing checks passed.
- HTTPS connection reuse works: user logs showed zero connection setup time
  for reused connections, with total replies around 2.3–3.4 seconds in those
  samples. Fresh connection setup took about 2.8 seconds; timings vary.
- Voice activation is implemented; user confirmed “It works now.” Defaults:
  80 ms onset, up to 200 ms pre-roll, 800 ms silence cutoff, five-second maximum
  including pre-roll, and one-second cooldown. KEY1 remains available.
- Automatic capture pauses while offline, during requests and during playback.
- Voice-activation build and sanitized detector tests passed; WAV tests passed previously.

## Current configuration and limits

- Unrecognized speech now maps to an empty answer (no TTS); Worker redeployment
  and real-audio validation pending.
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

- Pi Docker remains installed/running; Docker service/socket and containerd boot
  startup disabled and verified (2026-09-23). Ollama qwen3:1.7b text-tested.
  Ukrainian unreliable; warm replies 1–6 s, cold 25 s, with a two-CPU quota.
  RAM limits are not enforced (missing memory cgroup); printer services active.
  See [local deployment/results](../backend/local/README.md). Whisper, Piper,
  firmware API/TLS integration and printing coexistence validation remain pending.
- Upload keep-alive firmware; verify reuse after 30–60 seconds, close after three
  minutes, no further pings until a new request, and recovery after Wi-Fi loss.
- User also requested exploring upload while recording and minimal model cost.
  Streaming and model changes remain pending; settle the approach before coding.
- Validate each next step on hardware, including no speaker-triggered capture,
  cutoff behavior, background noise, and continued animation.
