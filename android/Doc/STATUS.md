# Current Android project status

Updated: 2026-09-28. Scope: manual playback of stored ESP32 clips.

## Implemented in source

- Settings contains a masked Gemini key field: Add saves/replaces the key in
  private internal storage (no backup/transfer); startup restores it.
- Test checks the entered key against the configured Gemini model without saving;
  access, quota, network and malformed-response failures have separate messages.
- Main screen adds a clip field and Play; numbers normalize to three digits.
  Invalid names, unready links and pending voice/write requests are rejected.
  Bluetooth connection/manual playback no longer requires a Gemini key.
- Relay scans for Parrot devices, requests MTU 185, subscribes to voice
  notifications, decodes IMA ADPCM, sends inline PCM WAV directly to Gemini,
  and writes clip/ignore replies. Worker source and tests were retired.
- Gemini intent prompt/model and topic/fallback/ignore selection are ported;
  topic descriptions are generated from the firmware catalog.
- Invalid/incomplete responses become errors. Local deadline is 25 seconds;
  cancel/disconnect/new recordings suppress stale replies. HTTP may finish later.
- Local instructions and project/status summaries define context boundaries;
  the shared BLE contract remains in the firmware project.

## Validation and limits

- Passed `:app:assembleDebug` and `:app:testDebugUnitTest` (14 tests).
- Topic generator syntax and source/Android catalog parity passed.
- No live Gemini call was made; model access and credentials remain unverified.
- Manual playback adds a BLE RX command; updated firmware is required.
  Existing audio packets/replies and clip filenames remain unchanged.
- Existing Cloudflare deployment was not modified.
- No physical-device test was performed; hardware verification remains pending.
- GitButler reports setup required on the current checkout. No session branch
  was created and no version-control setup or history changes were performed.

## Pending checks

- Verify manual playback, missing clips, busy/rapid requests and reconnects on device.
- Verify Settings save/restart/replacement and Test feedback on a physical phone.
- Validate direct Gemini success, invalid key/quota errors, silence, off-topic speech,
  cancellation and the reply deadline on a phone.
- Validate pairing, notification subscription, sustained audio transfer,
  disconnects during capture/upload, reconnect/resubscribe, clip replies,
  decoded audio quality, and end-to-end latency on a physical phone and ESP32.
- For integration tasks, compare against the shared BLE contract and record
  compatibility findings; firmware summaries may lag Android implementation.
