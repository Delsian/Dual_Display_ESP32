# Persistent configuration

The firmware loads `/config.json` from LittleFS before starting audio and BLE.
The example shipped in `data/config.json` contains:

```json
{
  "version": 1,
  "audio_volume": 75
}
```

| Setting | Allowed values |
| --- | --- |
| `version` | Required integer, currently `1` |
| `audio_volume` | Integer 0–100 |
Missing settings use defaults from `include/config.h`. A missing file is
created with those defaults. Malformed JSON, files larger than 8192 bytes,
unsupported versions, and invalid settings cause the entire configuration to
fall back to defaults; the original file remains available for correction.
Unknown fields are ignored on boot, including settings left by older firmware.
Saved configurations contain only version and volume; BLE patches reject unknown
fields. Serial reports whether loading succeeded.

To edit from a phone, use the [BLE configuration service](BLE.md) with nRF Connect.
Alternatively, edit `data/config.json`, upload the filesystem image,
then restart:

```sh
pio run -e esp32-s3-dualeye-touch-lcd-1_28 -t uploadfs
```

Uploading the filesystem replaces its contents, including device-specific
settings, with the local `data/` directory. A normal firmware upload preserves
LittleFS. On an existing filesystem without this file, the new firmware creates
it automatically; no filesystem upload is needed just to enable defaults.

Firmware code can call `save_device_config(config)` to persist validated settings
for the next boot. It returns false if validation or writing fails. Saves use a
temporary file followed by a LittleFS rename, without deleting the old file
first. Callers must serialize saves; the BLE service handles its writes sequentially.
The running configuration stays unchanged until restart.

Hardware pins remain compile-time settings. LittleFS mount failures now halt with a serial message
instead of formatting and erasing configuration and assets automatically.

## On-device checks

- Boot with the example file and check `Config: loaded /config.json.`.
- Change volume, upload the filesystem, and verify the setting after reboot and after a power cycle.
- Try an invalid value or malformed JSON: check the fallback message and verify
  the file has not been replaced.
- Remove `/config.json` from the filesystem image: check that boot creates it
  with defaults.
