# BLE configuration, battery, logs and voice link

Flash the firmware normally; a filesystem upload is not needed for BLE services.
Voice replies require the corresponding WAV clips in LittleFS (`/clips/`).
The device advertises as `Parrot-XXXXXX`. Install **nRF Connect for Mobile**,
scan, and connect from inside the app.

Configuration reads and writes require encrypted, authenticated BLE pairing.
When prompted, enter the six-digit code displayed on the left TFT (also printed
on serial). Pairing temporarily replaces the left eye. Firmware requests Secure
Connections with authenticated persistent bonding. Bluedroid stores bond keys in
NVS; the phone and device can reuse them after disconnects and device restarts.
Notification subscriptions are still reset and must be enabled on each connection.
After upgrading from non-bonding firmware, forget an obsolete Parrot pairing on
the phone and pair once again. Erasing device flash/NVS removes stored bonds.
Bond reuse across reconnection/power cycles is implemented but hardware-unverified.

## Configuration service and characteristics

Service UUID: `6b520001-7c8e-4c30-9aa8-45e626d39b01`

The characteristics share the suffix `-7c8e-4c30-9aa8-45e626d39b01`:

| UUID prefix | Purpose | Operation |
| --- | --- | --- |
| `6b520002` | JSON patch buffer | Write with response, UTF-8 text |
| `6b520003` | Commands | Write with response, UTF-8 text |
| `6b520004` | Result/status | Read, interpret as UTF-8 |
| `6b520005` | Configuration page | Read, interpret as UTF-8 |

## Battery service

The standard Battery Service (`0x180F`) exposes Battery Level (`0x2A19`) as
one unsigned byte, 0–100 percent. It supports reads and notifications without
pairing. The initial value is sampled before advertising starts; the main loop
refreshes it every 10 seconds and notifies subscribers when the percentage changes.
In nRF Connect, read Battery Level or enable its notifications.

This uses the existing ADC voltage-based estimate, not an ETA6098 register or
fuel gauge. It does not report charging status, and the estimate can be affected
by charging and load.

## Device log service

Added 2026-09-29; build/host-tested, not yet hardware-verified.

Service UUID: `6b520020-7c8e-4c30-9aa8-45e626d39b01`

TX UUID: `6b520021-7c8e-4c30-9aa8-45e626d39b01` (Notify only).
The service shares the existing GATT server and is discovered after connecting;
its UUID is not included in advertising. It is available independently of audio.
Enable TX notifications through its CCCD (`0x2902`, write `01 00`) after encrypted,
authenticated pairing. Disable with `00 00`. Subscribe again after reconnecting.

Each notification is 5–20 bytes, compatible with the default ATT MTU of 23:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 2 | Unsigned little-endian sequence, starting at zero and wrapping at 65535 |
| 2 | 2 | Unsigned little-endian count of queue-overflow bytes since the previous packet, saturated at 65535 |
| 4 | 1–16 | Application log text bytes |

Strip the four-byte header and concatenate text bytes in notification order.
Use an incremental UTF-8 decoder; characters and lines can span notifications.
Text retains the original serial line endings (`LF` or `CRLF`). A sequence gap
signals missing notifications; a nonzero overflow count signals dropped source
bytes, not their exact position in the queued text. Mark a gap and discard partial
line/decoder state when either occurs. Notifications have no acknowledgement or
replay, so this is a best-effort diagnostic stream, not an audit log.

Only application output routed through `DeviceLog` is mirrored: audio, AI/voice,
configuration, display, sensor and command diagnostics. USB serial output remains.
Pairing passkeys stay serial-only. SDK/library output, ROM boot messages and crash
dumps are not captured. Logs emitted before subscription are not retained.

The stream uses a 2048-byte queue, dropping new bytes when full, and a background
task sends at most one packet per 20 ms (up to 800 text bytes/s). Producers only
enqueue; BLE sends do not run in audio/render tasks. Existing serial writes retain
their behavior. Disconnecting or writing the CCCD resets queued data and counters.
Logs from concurrent tasks may interleave, as serial output can.

Client handoff: discover this service, pair, subscribe, decode the header, and show
the reconstructed text with visible loss markers. Serialize CCCD writes with other
GATT setup operations. The Android app does not yet subscribe or display logs;
existing battery and voice clients need no changes to continue working.

Hardware checks: verify unpaired subscriptions are rejected, subscribe after
pairing, trigger a recording or serial `speech 1`, and compare received text with
serial. Confirm no passkeys appear, test MTU 23 and 185, disable/re-enable and
disconnect/reconnect, and check voice streaming/eye responsiveness with logs on.

## Voice service (Android relay)

Updated 2026-09-25: firmware integration is implemented and build-tested; codec
host tests pass with sanitizers. The Android app is implemented (see its status), and the protocol
has not been validated on a phone. This describes current source behavior,
not a finalized or hardware-verified transport.

Service UUID: `6b520010-7c8e-4c30-9aa8-45e626d39b01`

The characteristics share the suffix `-7c8e-4c30-9aa8-45e626d39b01`:

| UUID prefix | Purpose | Operation |
| --- | --- | --- |
| `6b520011` | TX: ESP32 → phone recording | Notify, binary packets |
| `6b520012` | RX: phone → ESP32 reply or manual play | Write with response, UTF-8 text |

Voice, configuration and battery use one GATT server. The voice service is
created before advertising but its UUID is not included in advertising data;
connect using the device name or configuration service, then discover services.
With `USE_AUDIO=0`, the voice service is absent.

Enabling TX notifications through its CCCD (`0x2902`) and writing RX require
an encrypted, authenticated link. Pair using the displayed passkey. Subscribe
again after every reconnect; firmware clears the subscription on disconnect.
Negotiate MTU 185 before recording: full audio notifications contain 165 bytes,
requiring MTU at least 168. Firmware currently does not check the negotiated
MTU or shrink packets for a smaller one.

### TX packet format

Each notification contains one packet. All multibyte integers are little-endian;
there is no WAV header in the BLE stream.

| Type | Layout (byte offsets) | Meaning |
| --- | --- | --- |
| `0x01` start | `0`: type; `1–2`: sample rate, unsigned 16-bit | New mono recording, currently 16000 Hz |
| `0x02` audio | `0`: type; `1`: sequence; `2–3`: signed 16-bit predictor; `4`: step index; `5…`: ADPCM nibbles | Up to 320 samples (20 ms), normally 165 bytes total |
| `0x03` end | `0`: type; `1–4`: total sample count, unsigned 32-bit | Capture completed; phone may submit the recording |
| `0x04` cancel | `0`: type only | Discard recording and cancel any associated upload |

Audio selects `AUDIO_AI_MIC_CHANNEL` from the 16 kHz stereo capture. Sequence
starts at zero per recording and increments modulo 256. Each packet carries
the decoder state **before** its first sample; the predictor header is not an
extra output sample. Reset the decoder to that packet's predictor/index, then
decode low nibble first. Valid step indices are 0–88. Mirror
[`speech_adpcm_decode`](../src/speech_clip.cpp), including its exact
`((2 * magnitude + 1) * step) >> 3` update and saturation.

The last audio packet may be shorter. An odd sample count leaves an unused
high nibble in its last byte; use the end packet's total count to trim that
padding. These packets are not standard IMA ADPCM WAV blocks. A later packet's
state permits decoding after a missing packet, but does not recover lost audio.

### Relay and reply flow

The native Android 12+ app buffers and decodes notifications to mono
16-bit PCM. After end, it wraps the recording as PCM WAV and calls Gemini
directly using a user-entered API key (updated 2026-09-28). The app then writes
one complete UTF-8 message to RX, without a newline or NUL terminator:

| RX message | Firmware behavior |
| --- | --- |
| `clip:001` or `clip:off_1` | Decode and play `/clips/001.wav` or `/clips/off_1.wav` |
| `ignore` | Finish without playback |
| `error:<text>` | Log the reply and finish without playback |

Android maps a valid topic number to `clip:NNN`, `offtopic` to a random
`clip:off_K`, and `ignore` to `ignore`; failed or invalid results become errors. Clip names exclude the extension and accept 1–16 lowercase letters,
digits or underscores. RX retains at most 63 bytes; longer writes are truncated.
Other replies also finish without playback. Send one reply only after end.

Firmware accepts one recording/request at a time and waits up to 30 seconds
after end for a reply. Recordings shorter than 4000 samples (0.25 seconds)
produce cancel instead of end when still connected. Disconnect stops streaming
and ends reply waiting within the 100 ms polling interval; the recording buffer
stays reserved until capture finalizes. Reconnection cannot resume that job.

### Manual clip playback (2026-09-28)

After pairing and subscribing, write `play:<name>` to voice RX to play a clip
without recording or calling Gemini. Names are 1–16 lowercase ASCII letters,
digits or underscores, with no extension or path; `play:001` loads `/clips/001.wav`.
Android normalizes numeric input `1` to `001`; `off_1` is sent unchanged.
This command requires the updated firmware; existing reply messages are unchanged.

The BLE callback validates the full name (including rejecting embedded NUL),
reserves the shared busy flag, and queues decoding to the voice task. Pending
recording/reply/clip requests reject manual commands; they never enter the reply
queue. A queued command is dropped if its connection changes before processing.
An already playing clip can have one subsequent clip pending, using the existing
playback handoff. Playback already handed to the audio task survives disconnect.

The GATT write acknowledgment confirms delivery, not successful playback. Busy,
missing-file and decoding failures are reported on firmware serial; no new
playback-status notification or clip-list endpoint is provided.

### Pending protocol validation

- Verify manual play while idle, during playback and a recording/request, missing
  clips, invalid names, rapid presses and disconnect/reconnect.
- Verify negotiated MTU, notification delivery and sustained audio throughput.
  There is no application acknowledgment, retransmission or flow control.
- Define phone handling for missing packets and sample-count mismatches.
- Replies have no request ID. Timeout currently sends no cancel packet, so the
  relay suppresses stale responses with a 25-second deadline and local generation
  checks; robust on-wire request correlation remains pending.
- nRF Connect can inspect the service and send a manual reply after end, but
  does not implement the Android audio decoder/Gemini client.

## Change a setting

For example, set volume to 60:

1. Write `clear` to Commands. A protected operation may prompt pairing;
   complete pairing and retry the write if the app reports insufficient authentication.
2. Write `{"audio_volume":60}` to JSON patch buffer, selecting UTF-8/text encoding.
3. Write `save` to Commands.
4. Read Result/status. Expect `saved; reboot needed`.
5. Restart the device to apply the settings.

Omitted fields retain their last successfully saved values. Successive saves
accumulate changes, even before reboot. Unknown top-level keys, invalid values,
unsupported versions, and malformed JSON are rejected without modifying the
saved configuration. On errors, write `clear` and resend the corrected patch.

Configuration supports only `version` and `audio_volume`. Wi-Fi fields were
removed on 2026-09-27; clients must omit them from patches. Voice packets and
service UUIDs are unchanged.

For longer JSON, write consecutive chunks to the buffer, then send `save` once.
Use at most 20 bytes per write at the default MTU, or negotiate a larger MTU in
the app and use at most MTU minus 3 bytes (the firmware requests MTU 185).
Do not insert extra newlines or split UTF-8 characters. Each write appends bytes.
The total buffer limit is 4096 bytes. Overflow requires `clear` before retrying.
Disconnecting discards unfinished input; it does not undo a successful save.

## Read the configuration

1. Write `read:0` to Commands.
2. Read Configuration page using the app's long-read capability to retrieve
   the entire page (up to 400 bytes).
3. Read Result/status. `more` means another page exists; `end` means complete.
4. For subsequent pages, repeat with `read:400`, `read:800`, etc.

Pages are byte slices; join their bytes before decoding the entire JSON if a
UTF-8 character crosses a page boundary. Reads show the last configuration
saved during this boot, initially the validated boot configuration. Reads require
authenticated pairing.

Saving does not change currently running audio tasks, or restart the
device automatically. Do not upload the filesystem afterwards unless you intend
to replace the device's saved settings with your local `data/` files.

## Hardware verification

- Read Battery Level before pairing; verify a single byte in the range 0–100.
- Enable battery notifications and vary battery voltage; verify updates when the
  percentage changes. Disable notifications, then disconnect/reconnect and test
  subscribing again.
- Check that an unauthenticated read/write prompts pairing and an incorrect code
  cannot access config; pair with the displayed code.
- After pairing, disconnect/reconnect, reboot the device, and power-cycle it;
  verify protected config/voice/log access without a new passkey prompt after
  restoring subscriptions. Also verify forgetting the phone bond permits pairing again.
- Save a volume patch, read it back, reboot, and check that volume persists.
- Send multiple consecutive patches and verify omitted fields remain unchanged.
- Send malformed JSON, an invalid volume, and an oversized buffer; verify the
  saved file is unchanged and `clear` allows recovery.
- Split a volume patch across multiple writes and save; verify it after reboot.
- Disconnect mid-upload and reconnect; confirm partial input was discarded.
- Test BLE editing while recording/replaying audio. Build success does not
  verify runtime heap or radio behavior.
- Discover the voice service, pair, negotiate MTU 185 and subscribe to TX.
  Capture a phrase; verify start, sequential audio packets and end sample count.
- After end, write `ignore`, then test another recording with `clip:<name>` for
  an installed clip. Verify playback and subsequent recording.
- Release KEY1 before 0.25 seconds; verify cancel instead of end.
- Disconnect during capture and while awaiting a reply; reconnect, pair if
  required and resubscribe. Verify the old job is not resumed and a new one works.
