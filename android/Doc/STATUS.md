# Current Android project status

Updated: 2026-09-30. Scope: step 10 Android OTA and BLE firmware-version display.

## Implemented in source

- Firmware picker validates image size/metadata/version and SHA-256 before transfer.
  Service owns serialized OTA writes, flash acknowledgments, status-read recovery,
  progress/Cancel, timeouts and a bounded wake lock. UI closure retains progress.
  Voice/play/sleep writes are suppressed during OTA; failures disconnect for cleanup.
- Successful OTA reconnects to the updated device and compares its running version.
  Firmware version is read at connection, retained across UI rebinding and cleared
  on disconnect. Missing/invalid version does not block voice; firmware enforces upgrades.
- Sleep/WakeUp send authenticated commands; Sleep cancels AI and retains BLE.
- Common Logs shows device text dark blue and app text black, with Follow/Clear.
  AI counters track total/free/paid/succeeded/failed/pending classification calls;
  request logs show IDs, durations, results/safe errors. Clear preserves counters.
  Relay service retains 32K characters across activity closure; no disk logging.
- Authenticated log subscription is serialized with battery/voice setup. Decoder
  reassembles bounded UTF-8 lines, marks packet/overflow loss and resets on reconnect.
  Missing log service leaves voice working; stale GATT callbacks are ignored.
- Main screen and notification show battery percentage; notification uses a parrot icon.
  Service caches it across UI rebinding and clears on disconnect; unknown shows a dash.
- RelayService owns BLE/AI independently of MainActivity. Start/Stop and notification
  Stop control persisted enablement; boot/update and sticky restart are supported.
- Low-power discovery and 5–30 s reconnect backoff handle disconnect/Bluetooth changes;
  setup/write timeouts and generation guards protect requests from stale callbacks.
- Free/paid key fallback, bounded voice wake lock and manual playback remain available.

## Validation and limits

- Gradle 9.6.0 unit/lifecycle tests, debug APK build and lint passed offline.
- 52 tests pass: OTA metadata/version/hash, ACK ordering, missing notifications,
  cancellation, errors/offsets, timeout/disconnect, version discovery/rebinding,
  plus existing logs, battery, AI fallback and relay lifecycle coverage.
- No live Gemini or physical phone/ESP32 tests performed; firmware/app deployment pending.
- Restart remains subject to Android/vendor policy; force-stop requires reopening.
  Multiple Parrots use the first match. Log history is lost on service destruction.
- GitButler reports setup required; no session branch/commits/history changes made.

## Pending checks

- Test real OTA success, Cancel, out-of-range/process loss, older-image rejection,
  screen-off transfer, reconnect version identity, startup confirmation and rollback.
  Initial OTA-capable firmware/matching bootloader installation requires USB.
- Verify battery updates, permissions, notification Stop, activity closure, screen-off
  recording/AI replies, service/process restart, reboot and Bluetooth toggles.
- Confirm real Gemini fallback, hourly return to free, reply latency and quota behavior.
