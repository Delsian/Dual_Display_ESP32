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
At least two phones can retain independent bonds, with one active connection at
a time. The installed SDK supports 15 bonds; a build assertion requires at least
two. Connection encryption uses ESP_BLE_SEC_ENCRYPT to preserve the configured
SC/MITM/bonding policy for new peers and reuse stored keys on reconnect.
Protected GATT permissions still require MITM authentication. Diagnostics report
stored bond counts and authentication failure reasons, without exposing keys.
After flashing, forget obsolete phone pairings and pair each phone once. Validate
by alternating phone A/B connections and notification subscriptions, then repeat
after rebooting the firmware. Neither phone should require another passkey.
These hardware checks remain pending. No Android or packet-format change is needed.

## BLE OTA v1 contract (transfer implemented)

Updated 2026-09-30. Steps 3–7 are implemented: authenticated service, bounded
worker queue, exclusive mode, inactive-partition preparation, acknowledged Data
writes and incremental SHA-256. Finish requires the declared byte count, matching
hash and successful `esp_ota_end` image validation before selecting the new boot
partition. Success reports complete and restarts after a 500 ms delivery interval.
Abort, disconnect and 30-second receiving timeout release update mode and the
OTA/hash resources. Flash operations are serialized; Abort waits for an in-flight
operation to return and is checked before final boot activation begins.
Target build and sanitized control/chunk validation tests passed; BLE/flash tests remain
pending. A [Linux BLE test uploader](../scripts/README.md) supports preparation-only
and complete uploads; simulated host tests pass, actual BLE upload remains unverified.
Step 8 fault-injection tests also execute the real OTA worker with fake platform
APIs: Abort/disconnect/timeout (including clock wrap), flash begin/write failure,
hash mismatch, image/boot-selection failures, fresh retry and stale queued data.
Cleanup invalidates a transfer epoch and clears queued errors/data before releasing
exclusive mode. Late packets from a released transfer cannot start a new session;
rejected packets do not extend the timeout. Tests do not emulate physical flash
power loss or prove rollback behavior.
Android's step 10 uploader is implemented; see [Android status](../../android/Doc/STATUS.md)
for build and device validation. This
section specifies firmware-only updates, not bootloader, partition-table, NVS,
or LittleFS updates. An initial USB installation of OTA-capable firmware is
required. Interrupted transfers restart from offset zero; persistent resume is
out of scope. Startup confirmation and rollback requests are implemented as below;
actual recovery on the device remains hardware-unverified.

### Startup confirmation and rollback

The installed ESP32-S3 `qio_opi` SDK has
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1`. The supplied QIO bootloader ELF includes
pending-image support. This is local toolchain evidence, not a readback of the
device's bootloader: install the matching bootloader with a normal USB firmware
upload before relying on rollback. App-only BLE OTA cannot replace a bootloader.

Arduino normally confirms pending images before `setup()`. Firmware overrides
its weak `verifyRollbackLater()` hook and confirms only `PENDING_VERIFY` images
after LittleFS mounted, audio initialized, the BLE OTA worker/service started,
and five seconds of successful main-loop iterations with audio task health.
Display/sensor initialization is exercised by startup/rendering, but no physical
display, microphone or speaker self-test is claimed. No phone connection is needed.

A pending-image boot gets a 60-second deadline task. Mount/audio failure, failed
health checks, expiry, or confirmation failure requests
`esp_ota_mark_app_invalid_rollback_and_reboot()`. If no valid previous image exists,
the failure is logged and the image is never confirmed; USB recovery is required.
A reset/crash before confirmation allows the rollback-enabled bootloader to reject
the unconfirmed image on the next boot. Previously confirmed and initial USB
images do not run this validation path. This does not enable anti-rollback eFuses
or signed-image enforcement, and does not protect against a later crash after
confirmation or an image that removes its own validation logic.

Separate hardware validation (pending): first install/boot known-good image A,
OTA healthy B and verify the confirmation log; then OTA a test image that fails
startup or hangs before confirmation and verify the deadline/reset returns to B.
Also reset a pending image before confirmation and verify bootloader recovery.
Check image identity from serial ELF/version output and preserved settings/clips;
do not equate the uploader's `complete` packet with successful startup validation.

### Verified flash layout

The `esp32-s3-dualeye-touch-lcd-1_28` environment in `platformio.ini` selects
the installed Arduino framework's `tools/partitions/default_16MB.csv` and 16 MB
flash. No partition change is required:

| Name | Type/subtype | Offset | Size (bytes) |
| --- | --- | --- | --- |
| nvs | data/nvs | `0x9000` | `0x5000` (20,480) |
| otadata | data/ota | `0xe000` | `0x2000` (8,192) |
| app0 | app/ota_0 | `0x10000` | `0x640000` (6,553,600) |
| app1 | app/ota_1 | `0x650000` | `0x640000` (6,553,600) |
| spiffs | data/spiffs, used for LittleFS | `0xc90000` | `0x360000` (3,538,944) |
| coredump | data/coredump | `0xff0000` | `0x10000` (65,536) |

On 2026-09-30, the existing local target `firmware.bin` measured 1,160,144
bytes, leaving 5,393,456 bytes in either app slot. CSV non-overlap and flash
bounds checks passed. This verifies local configuration/image fit, not the
partition table deployed on hardware. Future images must be checked again;
the OTA receiver must use the actual inactive partition's size at runtime.
Only the inactive app partition and OTA boot metadata may be changed by OTA;
preserve NVS/bonds/settings, LittleFS/clips, and coredump storage.

### GATT endpoints and security

Service UUID: `6b520030-7c8e-4c30-9aa8-45e626d39b01`.
Register on the shared server before advertising; discover after connection,
without adding another advertising UUID. All endpoints share the suffix
`-7c8e-4c30-9aa8-45e626d39b01`:

| UUID prefix | Name | Operations |
| --- | --- | --- |
| `6b520031` | Control | Write with response |
| `6b520032` | Data | Write with response |
| `6b520033` | Status | Read, Notify |

Require encrypted, MITM-authenticated pairing for Control/Data writes, Status
reads, and Status CCCD reads/writes. Only notify the authenticated subscribed
connection. Subscribe before Begin; reset subscription on disconnect. Require
negotiated ATT MTU at least 40; request 185. Each command/chunk is one complete
characteristic write, not a prepared/long write. No text, terminators or hex
encoding are used. All multibyte integers below are unsigned little-endian.

### Control and data packets

| Control command | Exact length | Layout |
| --- | --- | --- |
| Begin | 37 bytes | byte 0: `0x01`; bytes 1–4: image size; bytes 5–36: raw SHA-256 digest of the complete firmware.bin |
| Finish | 1 byte | byte 0: `0x02` |
| Abort | 1 byte | byte 0: `0x03` |

Data writes contain a four-byte absolute image offset followed by at least one
firmware byte. Maximum payload is `min(negotiated_MTU - 7, 508)` bytes (178 at MTU 185),
respecting the 512-byte GATT characteristic-value limit;
the final chunk may be shorter. Offset starts at zero. Reject empty payloads,
offsets other than the next expected offset, or payloads exceeding the declared
remaining size. The declared image must be nonempty and fit the inactive slot.
The payload is the application `firmware.bin`, never a merged flash image.

### Status packet and acknowledgment

Status is exactly six bytes: byte 0 is state, bytes 1–4 are the next expected
offset, byte 5 is error. The offset counts bytes successfully written and
included in the running hash, not bytes merely received or queued.

| State | Value | Meaning |
| --- | --- | --- |
| idle | `0x00` | No transfer; initial status is all zero bytes |
| preparing | `0x01` | Begin accepted; flash preparation pending, offset zero |
| receiving | `0x02` | Ready for the chunk at the reported offset |
| verifying | `0x03` | Finish accepted; hash/image checks pending |
| complete | `0x04` | Image valid and boot partition selected; reboot pending |
| error | `0x05` | Transfer aborted; old boot selection retained |

| Error | Value |
| --- | --- |
| None | `0x00` |
| Malformed packet or unknown opcode | `0x01` |
| Invalid state/busy | `0x02` |
| Empty/oversized image or chunk exceeds image bounds | `0x03` |
| Unexpected offset | `0x04` |
| MTU below 40 | `0x05` |
| No usable inactive OTA partition | `0x06` |
| Flash begin/write failure | `0x07` |
| SHA-256 mismatch | `0x08` |
| Image validation failure | `0x09` |
| Boot partition selection failure | `0x0a` |
| Transfer timeout | `0x0b` |
| Explicit abort | `0x0c` |
| Connection lost | `0x0d` |
| Queue/resource unavailable | `0x0e` |
| Version not strictly higher, invalid version, or wrong project | `0x0f` |

A GATT write response only confirms transport delivery. Publish Status after
each state transition, rejected packet, and successfully written chunk; reads
return the latest coherent snapshot. Client sends only one operation at a time
and waits for its application acknowledgment before continuing. Start Data only
after `receiving/0/none`. A chunk is acknowledged by `receiving/(offset+length)/none`.
After a missed notification, read Status: advanced offset means accepted;
unchanged offset with no error may mean still processing, so poll rather than
enqueue duplicates. Duplicate/out-of-order packets never write flash twice.
An unexpected-offset response reports the authoritative offset for recovery.

Errors `0x01`–`0x05` and queue-full `0x0e` reject the operation without changing state or
offset; the next successful operation clears the error. Preparation resource
failure (`0x0e`, including failure to quiesce audio/voice within two seconds or
initialize/update/finalize hashing) terminates the transfer in `error`. Operational failures
`0x06`–`0x0d` and `0x0f` terminate the transfer in `error`, retaining the accepted offset
for diagnostics. Begin from idle/error clears prior status; Begin during a live
transfer is busy. Finish requires receiving with offset equal to declared size;
early Finish is an invalid-state rejection. Abort terminates preparing/receiving/
verifying; it is a no-op in idle/error and is rejected after complete. Control
operations must be serialized with flash work so Abort cannot race boot selection.

### Transfer lifecycle and client handoff

Firmware versions are canonical numeric `major.minor.patch` releases, initially
`1.0.0`, defined once in `include/firmware_version.h`. Each component is an
unsigned 32-bit number; no leading zeros or prerelease/build suffixes are accepted.
The string must fit the ESP descriptor's 32-byte field including its terminator.
The target's PlatformIO hook `scripts/embed_version.py` updates the precompiled
SDK application descriptor in the ELF before binary generation, including its
project name `Parrot`. Boot logs show the same version.

After hash/image validation, Finish reads the inactive partition's descriptor
and requires project `Parrot` and a version numerically greater than the running
build. Equal, older, missing or malformed versions return `0x0f` without selecting
the new boot partition. This check is firmware-enforced, independent of uploader;
Begin and transfer still run before rejection. Increase the header version and
rebuild for each release (e.g. `1.0.1`). Older firmware lacking this check needs
the initial enforcing release installed first. Automatic bootloader rollback
and USB recovery remain possible; this is not eFuse anti-rollback or image signing.

Begin enters exclusive update mode: stop audio/capture, invalidate voice jobs,
reject new playback and Sleep/WakeUp commands, and inhibit inactivity shutdown.
Keep BLE/status available. Finish verifies byte count, SHA-256 and ESP image
validity before selecting the new boot partition. SHA-256 is corruption detection,
not publisher authentication. Notify complete and allow a short delivery interval
before reboot; a disconnect alone is not proof of success.

Disconnect before complete, Abort, or 30 seconds without accepted transfer progress
aborts the session and releases its resources without switching boot selection.
Reads, rejected packets and duplicates do not extend the timeout. Before normal
activity resumes, stale queued work must be invalidated. No transfer is resumed
after reconnect; read status, then send a new Begin from zero. Complete is the
success signal. Boot selection is a serialized commit operation: once it starts,
a later Abort/disconnect cannot revoke it, and successful selection always reboots.
An abort/disconnect observed before that operation prevents boot selection.

Pending firmware work: physical recovery/rollback verification and hardware testing.
Preparation preserves the existing
manual-sleep/active-window policy; disconnect still starts its normal five-minute
window. Update mode overrides audio activity and inhibits shutdown until released.
Android provides a file picker, metadata/size/hash validation, secured service setup,
serialized acknowledged writes, progress/errors, Cancel, and post-reboot reconnect
to the updated device. It reads the running version again after reconnect;
this proves image identity at that time, not completion of startup confirmation.
Hardware power-loss tests
and bootloader rollback verification remain required before deployment.

## Firmware version service

Device Information service `0x180A` exposes Firmware Revision String `0x2A26`
as a read-only, unencrypted standard characteristic. The value is the running
build's `FIRMWARE_VERSION`: canonical ASCII `major.minor.patch`, without a NUL
terminator. It is available through service discovery, not advertising.
Read it on each connection; there are no notifications or CCCD. Android shows
the version on its main screen, caches it across activity rebinding, and clears
it on disconnect. Missing or malformed values show a dash and do not block voice.
The endpoint reports image identity, not OTA startup-health confirmation.

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
playback handoff. Disconnect stops playback and discards queued clips.

### Disconnected idle (2026-09-29)

Android **Sleep** and **WakeUp** write `sleep` and `wakeup` to authenticated voice
RX. These commands bypass the playback/request busy gate. Sleep cancels the
current voice job and forces eyes/audio off while leaving BLE connected; WakeUp
clears that override and starts a fresh five-minute active window. BLE disconnect
also clears the override and restarts that window, just like boot. Clip playback
cannot wake a device while the manual sleep override is still set.
The one-hour deep-sleep timer runs only while disconnected, so a connected app
can always request WakeUp. Writes confirm transport delivery, not device state;
older firmware does not support these commands. Both firmware and app updates
are required; no new UUIDs or notification packets are introduced.

Boot starts a five-minute active window with eyes and audio enabled even offline.
Every spoken clip restarts that window at playback start and completion. While
disconnected and active, voice/KEY1 capture uses a random local off_*.wav fallback
after at least 0.25 seconds of captured audio; no BLE recording packets are sent.
The energy detector cannot distinguish speech from all background sounds offline.
A connected central keeps the device active; connected capture still requires
the authenticated voice subscription. Disconnect cancels the old connection's
work and starts a fresh five-minute window for new offline captures and fallback
replies, even if the device was previously put to sleep from the app.

When the window expires without a connection, backlights and eye rendering turn
off, recording/playback stop, and the amplifier is muted. Microphone DMA is
drained and discarded while idle; codecs are not powered down.
Advertising and battery updates continue for up to one continuous hour of idle.
Then the ESP32 enters deep sleep with all wake sources disabled: BLE advertising
stops and reconnecting cannot wake it. Press the hardware RESET button to restart
(KEY1 is not a wake button). Reconnecting before shutdown resets the idle timer;
the shutdown timer starts only after the active window expires.
Backlight and amplifier outputs are held off during sleep. This is MCU deep sleep,
not a board power disconnect; peripheral power consumption is hardware-unverified.
A BLE connection before shutdown restores the displays
so the pairing code remains visible; voice capture additionally requires the
authenticated voice notification subscription. A held KEY1 must be released and
pressed again. No packet changes or Android changes are required.
Build-tested; hardware disconnect/reconnect validation is pending.

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
