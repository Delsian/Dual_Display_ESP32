# Current Android project status

Updated: 2026-09-29. Scope: notification battery percentage and parrot icon.

## Implemented in source

- Sleep/WakeUp buttons send authenticated voice commands via the relay service.
  Sleep cancels AI results and retains BLE; matching firmware/hardware checks required.
- Reference-inspired pirate-parrot adaptive launcher icon added; phone launcher check pending.
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
- Separate free/paid keys have masked Save/Test controls. Free failures retry once
  with paid within the shared 25 s deadline; paid is used for one hour before free
  is retried. Key changes/service recreation reset selection; empty paid disables it.
- Bounded wake lock covers voice requests. Manual stored-clip playback remains available.

## Validation and limits

- Gradle 9.6.0 unit/lifecycle tests, debug APK build and lint passed offline.
- Log tests cover UTF-8/line fragmentation, gaps/overflow, malformed/oversized data,
  sequence wrap/reset, modern/legacy callbacks, voice routing, stale connections,
  history limits, rebinding and Clear. Battery, fallback and prior lifecycle tests pass.
  Common-log tests cover source colors, counters, retry events and safe error summaries.
- No live Gemini call or physical phone/ESP32 test was performed for this change.
- Sleep/WakeUp and logs require matching firmware; see the shared BLE contract.
  Firmware/app deployment and hardware Sleep/WakeUp checks are pending.
- Restart remains subject to Android/vendor policy; force-stop requires reopening.
  Multiple Parrots use the first match. Log history is lost on service destruction.
- GitButler reports setup required; no session branch/commits/history changes made.

## Pending checks

- Install firmware/app and verify colors/counters, pairing, log subscription, Follow/Clear,
  reconnect boundaries, unavailable firmware, and concurrent logs/audio/animation.
- Verify battery updates, permissions, notification Stop, activity closure, screen-off
  recording/AI replies, service/process restart, reboot and Bluetooth toggles.
- Check stalled GATT, late replies, key changes, manual play and battery restrictions.
- Confirm real Gemini fallback, hourly return to free, reply latency and quota behavior.
