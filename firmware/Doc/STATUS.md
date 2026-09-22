# Current project status

Updated: 2026-09-22. Scope: voice AI workflow.

## Implemented and validated

- Microphone phrase → Worker `/ask` → Gemini text reply in serial works on
  hardware, per user logs. Actual reply speech is now implemented locally.
- Reply speech: user logs confirm successful TTS and playback on hardware.
- Keep-alive success confirmed in user logs; three-minute close validation pending.
- Detailed Gemini headers/body and speech timing added; deployment and hardware
  timing validation pending. Mocked Worker timing checks passed.
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

- Unrecognized speech now maps to an empty answer (no TTS); Worker redeployment
  and real-audio validation pending.
- English/Ukrainian-only instructions preserved; validate after Groq deployment.
- `AUDIO_AI_REPLY_TEST=1`, `AUDIO_VOICE_ACTIVATION=1`, microphone channel 0.
- `/ask` now uses Groq Whisper Turbo → `openai/gpt-oss-20b`; Gemini TTS unchanged.
- Groq key and English/Ukrainian text calls verified (339/153 ms locally).
  Mocked routing/error tests passed; deployed audio flow remains unverified.
- Energy detection can trigger on background sounds; tune `AUDIO_VAD_MIN_RMS`
  in `include/config.h`. This is not wake-word detection.
- Upload starts after recording; no streaming capture/upload implemented.
- Wi-Fi sleep remains enabled for BLE coexistence after a confirmed abort
  when sleep was disabled. Do not reintroduce that optimization.

## Pending work

- Upload keep-alive firmware; verify reuse after 30–60 seconds, close after three
  minutes, no further pings until a new request, and recovery after Wi-Fi loss.
- Add `GROQ_API_KEY` Worker secret and deploy; test both languages and noise.
- Streaming upload remains pending; settle the approach before coding.
- Validate each next step on hardware, including no speaker-triggered capture,
  cutoff behavior, background noise, and continued animation.
- No blocker is currently reported. Read [SPEECH.md](SPEECH.md) for operating
  steps and timing definitions; update this summary after important changes.
