# Current Android project status

Updated: 2026-09-27. Scope: Android context setup and source inspection.

## Implemented in source

- Native activity provides endpoint/token input, Bluetooth permission handling,
  connection controls, and status display.
- Relay scans for Parrot devices, requests MTU 185, subscribes to voice
  notifications, decodes IMA ADPCM, uploads PCM WAV to `/intent`, and writes
  clip/ignore replies. These are source findings, not hardware verification.
- Local instructions and project/status summaries define context boundaries;
  the shared BLE contract remains in the firmware project.

## Validation and limits

- Configuration and relevant Android source were inspected for these summaries.
- No Android build or physical-device test was performed for this documentation
  change; no successful Android build evidence is recorded here.
- The checkout contains Gradle wrapper properties but no `gradlew` launcher or
  wrapper JAR. Follow README setup; command-line wrapper builds require the
  missing wrapper tooling.
- GitButler reports setup required on the current checkout. No session branch
  was created and no version-control setup or history changes were performed.

## Pending checks

- Confirm Gradle sync and a debug build in the Android development environment.
- Validate pairing, notification subscription, sustained audio transfer,
  disconnects during capture/upload, reconnect/resubscribe, clip replies,
  decoded audio quality, and end-to-end latency on a physical phone and ESP32.
- For integration tasks, compare against the shared BLE contract and record
  compatibility findings; firmware summaries may lag Android implementation.
