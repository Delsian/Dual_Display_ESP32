# Parrot Relay for Android

Native Android 12+ companion app for the ESP32 Parrot BLE voice link. Open the `android/` directory in Android Studio, let Gradle sync, and run it on a physical Android device with Bluetooth LE.

## Setup

1. Install Android Studio with JDK 17 and Android SDK 35.
2. Open this directory as a project.
3. Run the app on a physical Android 12+ phone.
4. Grant Nearby devices and, on Android 13+, notification permission when prompted.
5. Open **Settings**, enter the free-account key and press **Save Free key**.
   Optionally enter a paid-account key and press **Save Paid key**.
   Test either key, close Settings, then **Start background relay**.
6. Pair with the six-digit passkey shown on the ESP32 display when Android requests it.

Both keys are saved separately in app-private internal storage, excluded from backup/device
transfer, and loaded on app startup. The existing saved key becomes the free key.
**Save** applies changes to subsequent requests. Both fields are masked; view-state
saving and autofill stay disabled. Save an empty paid key to disable fallback.
**Test Free key** and **Test Paid key** check the entered value without saving it, using a small request to the same
Gemini model as the relay. This uses API quota. Results distinguish access, quota,
network, and response failures; a failed test does not overwrite the saved key.
The app calls Gemini directly; Cloudflare and a device token are no longer required.

## Background relay

After the relay is Ready, **Sleep** turns off Parrot's eyes/audio while retaining
the BLE connection; **WakeUp** restores activity and its five-minute offline
window. These controls require matching updated firmware. Sleep cancels the
current voice request. If a BLE write is already pending, retry after it finishes.
Deep sleep after one hour of disconnected idle still requires hardware RESET.

The main screen shows **Parrot battery: N%** from the device's BLE battery service.
It reads the level on connection and listens for changes while connected. The service
keeps the latest value when you close and reopen the activity. A dash means the level
is unavailable or the device is disconnected. This is the device's voltage-based
estimate, not the phone battery or a charging-status indicator.

Start the relay once from the app. A persistent **Parrot relay** notification
shows connection/request status and has a **Stop** action. You can leave or close
the activity; the service owns BLE and AI requests, and the UI reconnects to the
same service when reopened. **Stop background relay** disables automatic recovery.

While enabled, the app scans with a low-power filter for Parrot's advertised
configuration service. It connects when a matching device appears, discovers the
voice service, negotiates MTU, and subscribes again after reconnecting. Discovery
continues with the screen off. Connection/setup failures retry with a 5–30 second
backoff; stalled setup is abandoned after 60 seconds. Bluetooth off pauses work;
turning it back on resumes discovery. If multiple Parrots are nearby, the first
matching advertisement is selected, as there is no device selection UI yet.

The enabled setting survives process restarts, phone reboot and app updates.
Android may defer service restarts; force-stopping the app or vendor battery
restrictions can prevent recovery until you reopen it. Notification denial does
not prevent the foreground service, but its status may only appear in Android's
active-apps UI. No permanent wake lock is held: each voice request gets a bounded
wake lock through reply delivery to support screen-off processing.

**Pairing is still required.** Current firmware does not retain bonds, so Android
may ask for the ESP32 passkey again after reconnecting. Automatic discovery does
not bypass that prompt. Fully unattended reconnects need firmware bonding support.

AI requests read both saved Gemini keys in the service, independent of the activity.
A missing free key returns an error to the ESP32 and reports a Settings reminder.
Free is used first. Any classification exception (HTTP, network, or invalid response)
retries the same recording once with the paid key, if configured and different.
Subsequent recordings use paid for one hour from the free failure; the next recording
after that hour tries free again. Another free failure starts another hour on paid.
Paid failures return an error without further retries or extending the hour.
Changing either key or recreating the relay service resets selection to free.
Key tests use only the selected field and do not change fallback state.
Each recording retains the existing 25-second AI deadline; stopping, disconnecting,
or starting a new recording invalidates old results. Both attempts share this deadline;
no paid retry starts after cancellation or expiry. A slow free request can leave too
little time for the paid attempt. Manual clip playback remains
available through the app while the service is running.

This uses Android's [connected-device foreground service guidance](https://developer.android.com/develop/connectivity/bluetooth/ble/background).
Verify screen-off audio, pairing, out-of-range recovery, Bluetooth toggles, reboot,
notification Stop, and battery restrictions on your phone before relying on it.

## App and device logs

Press **Logs** to open the common live, selectable log window. Device text is dark
blue and app events are black on a white background. **Follow new logs**
keeps the newest lines visible; turn it off to inspect earlier output. **Clear**
removes the app's current log history without changing the device or subscription.

AI counters show total Gemini classification calls, free/paid calls, succeeded,
failed and pending calls. A fallback retry counts as a separate call. Logs include
call ID, key tier, audio duration, result or safe failure category (including HTTP
status), and elapsed time. Late/cancelled results are marked as not delivered.
Counters measure API-call outcomes, not device playback; key tests in Settings are
excluded. Clear preserves counters; service destruction resets them. API keys,
provider response bodies and recorded audio are never added to app request logs.

App connection/status and AI events appear alongside device logs. The running relay
automatically subscribes to the authenticated firmware log service
after pairing. The service retains up to 32K characters in memory while the window
is closed; older lines are removed. History is lost when the service is destroyed.
Only complete lines are displayed, with a 4096-byte limit per line. UTF-8 characters
and lines are reassembled across notifications. Packet loss and firmware overflow
produce visible gap markers; damaged partial lines are discarded.

Install firmware with the [device log endpoint](../firmware/Doc/BLE.md#device-log-service).
Older firmware shows a logs-unavailable message while voice operation continues.
Reconnects reset decoding and resubscribe; the window marks connection boundaries.
Pairing passkeys, ROM boot output, SDK logs and crash dumps are not included.

## Play a stored ESP32 clip

Start the background relay and wait for **Ready**, enter a clip number or name (for example `1`,
`001`, or `off_1`) in the main screen's clip field, then press **Play**.
Numbers 1–999 are padded to three digits. Do not include `.wav` or a path.
The clip plays on the ESP32 speaker; no API key or internet connection is needed.
Gemini voice classification still needs a saved key.

This requires the updated firmware's manual-play command. Clips must already
be in the ESP32 filesystem. The app reports the BLE write result; it does not
receive playback completion or missing-file status. Check firmware serial logs
for those errors. Wait for a recording/request to finish before manual playback.

## Relay behavior

The service scans for Parrot's advertised configuration service, negotiates MTU 185, subscribes to the voice TX characteristic, decodes the firmware's packetized IMA ADPCM to 16-bit mono PCM, sends an inline base64 WAV to Gemini, and writes `clip:<name>` or `ignore` back to the ESP32.

Protocol and hardware behavior are documented in [the shared BLE contract](../firmware/Doc/BLE.md). BLE throughput, pairing, reconnects, and end-to-end audio still require physical-device validation.

## Intent selection and validation

`GeminiIntent.kt` preserves the former Worker's model (`gemini-3.6-flash`),
prompt, enum response schema, and topic/fallback/ignore behavior. Unknown,
blocked, empty, or incomplete results return an error instead of playing a clip.
Requests time out locally after 25 seconds; cancelled, disconnected, superseded,
and late results cannot produce a reply. Network work may finish after cancellation.

The catalog in `app/src/main/assets/intent_topics.json` contains descriptions only.
Its source is `../firmware/backend/topics.json`. After editing that source, run
`python3 backend/convert_phrases.py --topics-only` from the firmware directory,
then rebuild this app. Clip conversion also refreshes the catalog.

Run `gradle :app:testDebugUnitTest :app:assembleDebug :app:lintDebug` with the installed Gradle.
Tests do not contact Gemini. Live key/model access, quota failures, classification,
cancellation, and phone/ESP32 playback still need device validation.
The old Cloudflare deployment has not been changed or deleted.

## Project context

Start Android agent sessions in this directory. [AGENTS.md](AGENTS.md) defines
the working rules; [Doc/PROJECT.md](Doc/PROJECT.md) describes the local
architecture, and [Doc/STATUS.md](Doc/STATUS.md) tracks implementation and
validation evidence. Read firmware documentation only when a task involves
device integration; use the shared BLE contract rather than duplicating it.
