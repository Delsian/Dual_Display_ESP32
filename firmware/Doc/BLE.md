# BLE configuration with nRF Connect

Flash the firmware normally; a filesystem upload is not needed for BLE support.
The device advertises as `Parrot-XXXXXX`. Install **nRF Connect for Mobile**,
scan, and connect from inside the app.

Configuration reads and writes require encrypted, authenticated BLE pairing.
When prompted, enter the six-digit code displayed on the left TFT (also printed
on serial). Pairing temporarily replaces the left eye; Wi-Fi setup can remain
visible on the right. This implementation does not request bonding; reconnecting
can require pairing again. If a phone retains an obsolete bond, forget it and
pair again. BLE and Wi-Fi use the same device name but are separate connections.

## Service and characteristics

Service UUID: `6b520001-7c8e-4c30-9aa8-45e626d39b01`

The characteristics share the suffix `-7c8e-4c30-9aa8-45e626d39b01`:

| UUID prefix | Purpose | Operation |
| --- | --- | --- |
| `6b520002` | JSON patch buffer | Write with response, UTF-8 text |
| `6b520003` | Commands | Write with response, UTF-8 text |
| `6b520004` | Result/status | Read, interpret as UTF-8 |
| `6b520005` | Configuration page | Read, interpret as UTF-8 |

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
