# Current Android project status

Updated: 2026-09-29. Scope: Parrot battery indicator and Gemini key fallback.

## Implemented in source

- Main screen shows Parrot battery percentage from an initial BLE read and live
  notifications; service caches it across UI rebinding and clears on disconnect.
  Invalid/missing levels show a dash; battery setup is serialized with voice setup.
- RelayService owns BLE and AI independently of MainActivity. Start/Stop and
  notification Stop control persisted enablement; UI binds for status/manual play.
- connectedDevice foreground service, notification permission request, boot/update
  receiver and sticky restart support were added. No microphone permission needed.
- Filtered low-power discovery finds Parrot while the screen is off; disconnects
  and GATT errors retry with 5–30 s backoff. Setup and write callbacks have timeouts.
- Bluetooth off pauses connection work; Bluetooth on resumes. Stop clears scans,
  retry timers, GATT state and in-flight reply generation. Old callbacks are ignored.
- Separate free/paid keys are read for each request; the previous key remains free.
  Settings provides masked Save/Test controls for each; empty paid disables fallback.
- A free-key exception retries the same audio once with paid within the 25 s deadline.
  Paid is selected for one hour, then free is retried. Paid failures do not extend it.
  Key changes/service recreation reset to free; cancelled/expired work cannot retry.
- Missing free keys return a Settings reminder. A bounded wake lock covers requests.
- Existing BLE packets/replies, Gemini classifier and 25-second deadline retained.

## Validation and limits

- Gradle 9.6.0 debug build, unit/lifecycle tests and lint passed offline.
  Battery tests cover reads, modern/legacy notifications, invalid values, voice
  routing, disconnect/stale callbacks, and cached values after rebinding.
  Added eight fallback tests (hour boundary, failures, key changes, cancellation),
  same-audio paid retry simulation, and independent key storage/reload checks.
- No live Gemini call or physical phone/ESP32 test was performed for this change.
- Firmware does not retain bonds: reconnect can still require passkey approval.
  Fully unattended pairing needs a firmware change; this task changes Android only.
- Reboot/process recovery remains subject to Android/vendor background policy.
  Force-stop requires reopening the app; multiple Parrots use the first match.
- BLE contract unchanged; no counterpart firmware change required for the service.
- GitButler reports setup required; no session branch/commits/history changes made.

## Pending checks

- Verify initial battery percentage, live changes, disconnect/reconnect and UI reopening on hardware.
- Verify permission grants/denials, notification Stop, app closure, screen-off
  recording/AI replies, service/process restart, reboot and Bluetooth toggles.
- Check out-of-range/reconnect pairing, subscription restoration, stalled GATT,
  late AI replies, key changes, manual play and vendor battery restrictions.
- Confirm both Settings fields, real Gemini fallback, hourly return to free,
  reply latency and key/model/quota behavior on phone/ESP32.
