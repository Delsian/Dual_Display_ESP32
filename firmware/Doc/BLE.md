# BLE configuration, battery and voice link

Flash the firmware normally; a filesystem upload is not needed for BLE services.
Voice replies require the corresponding WAV clips in LittleFS (`/clips/`).
The device advertises as `Parrot-XXXXXX`. Install **nRF Connect for Mobile**,
scan, and connect from inside the app.

Configuration reads and writes require encrypted, authenticated BLE pairing.
When prompted, enter the six-digit code displayed on the left TFT (also printed
on serial). Pairing temporarily replaces the left eye; Wi-Fi setup can remain
visible on the right. This implementation does not request bonding; reconnecting
can require pairing again. If a phone retains an obsolete bond, forget it and
pair again. BLE and Wi-Fi use the same device name but are separate connections.

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

## Voice service (Android relay)

Updated 2026-09-25: firmware integration is implemented and build-tested; codec
host tests pass with sanitizers. The Android app is pending, and the protocol
has not been validated on a phone. This describes current source behavior,
not a finalized or hardware-verified transport.

Service UUID: `6b520010-7c8e-4c30-9aa8-45e626d39b01`

The characteristics share the suffix `-7c8e-4c30-9aa8-45e626d39b01`:

| UUID prefix | Purpose | Operation |
| --- | --- | --- |
| `6b520011` | TX: ESP32 → phone recording | Notify, binary packets |
| `6b520012` | RX: phone → ESP32 reply | Write with response, UTF-8 text |

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

The planned native Android 12+ app buffers and decodes notifications to mono
16-bit PCM. After end, it wraps the recording as PCM WAV and posts to the
existing Worker `/intent` endpoint using the device token. The app then writes
one complete UTF-8 message to RX, without a newline or NUL terminator:

| RX message | Firmware behavior |
| --- | --- |
| `clip:001` or `clip:off_1` | Decode and play `/clips/001.wav` or `/clips/off_1.wav` |
| `ignore` | Finish without playback |
| `error:<text>` | Log the reply and finish without playback |

Map the Worker's nonempty `clip` field to `clip:<name>` and its ignored result
to `ignore`. Clip names exclude the extension and accept 1–16 lowercase letters,
digits or underscores. RX retains at most 63 bytes; longer writes are truncated.
Other replies also finish without playback. Send one reply only after end.

Firmware accepts one recording/request at a time and waits up to 30 seconds
after end for a reply. Recordings shorter than 4000 samples (0.25 seconds)
produce cancel instead of end when still connected. Disconnect stops streaming
and ends reply waiting within the 100 ms polling interval; the recording buffer
stays reserved until capture finalizes. Reconnection cannot resume that job.

### Pending protocol validation

- Verify negotiated MTU, notification delivery and sustained audio throughput.
  There is no application acknowledgment, retransmission or flow control.
- Define phone handling for missing packets and sample-count mismatches.
- Replies have no request ID. Timeout currently sends no cancel packet, so the
  relay must suppress late responses; robust request correlation remains pending.
- nRF Connect can inspect the service and send a manual reply after end, but
  does not implement the Android audio decoder/Worker relay.

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

To change Wi-Fi networks, send a patch such as:

```json
{"wifi_networks":[{"ssid":"Home","password":"replace-this-password"}]}
```

The supplied array replaces the whole list; `{"wifi_networks":[]}` clears it.
Portal credentials in NVS are separate and are not changed by these commands.

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
saved during this boot, initially the validated boot configuration. They include
JSON Wi-Fi passwords and therefore require authenticated pairing.

Saving does not change currently running audio or network tasks, or restart the
device automatically. Do not upload the filesystem afterwards unless you intend
to replace the device's saved settings with your local `data/` files.

## Hardware verification

- Read Battery Level before pairing; verify a single byte in the range 0–100.
- Enable battery notifications and vary battery voltage; verify updates when the
  percentage changes. Disable notifications, then disconnect/reconnect and test
  subscribing again.
- Check that an unauthenticated read/write prompts pairing and an incorrect code
  cannot access config; pair with the displayed code.
- Save a volume patch, read it back, reboot, and check that volume persists.
- Send multiple consecutive patches and verify omitted fields remain unchanged.
- Send malformed JSON, an invalid volume, and an oversized buffer; verify the
  saved file is unchanged and `clear` allows recovery.
- Split a Wi-Fi list across multiple writes and save; verify it after reboot.
- Disconnect mid-upload and reconnect; confirm partial input was discarded.
- Test BLE editing while the Wi-Fi portal is active and while recording/replaying
  audio. Build success does not verify runtime heap or radio coexistence.
- Discover the voice service, pair, negotiate MTU 185 and subscribe to TX.
  Capture a phrase; verify start, sequential audio packets and end sample count.
- After end, write `ignore`, then test another recording with `clip:<name>` for
  an installed clip. Verify playback and subsequent recording.
- Release KEY1 before 0.25 seconds; verify cancel instead of end.
- Disconnect during capture and while awaiting a reply; reconnect, pair if
  required and resubscribe. Verify the old job is not resumed and a new one works.
