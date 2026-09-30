# Linux BLE OTA test uploader

Requires Linux with BlueZ, a working BLE adapter, Python 3.10+, and the firmware
implementing OTA steps 3–7 already installed on Parrot. Stop Android's background
relay so it does not reconnect during this test. Firmware must be awake/advertising;
press RESET if it entered deep sleep. No Android or firmware changes are made by
installing this script.

From the firmware directory:

```sh
python3 -m venv /tmp/parrot-ota-venv
/tmp/parrot-ota-venv/bin/pip install -r scripts/requirements.txt
```

Find/pair the device using `bluetoothctl` (keep this terminal open as the passkey
agent while running the uploader from another terminal):

```text
power on
agent KeyboardDisplay
default-agent
scan on
pair AA:BB:CC:DD:EE:FF
```

Enter the six-digit passkey displayed by Parrot. Replace the address below with
its actual BLE address. The script also requests pairing, reusing existing bonds.
It does not ask for or store the passkey itself.
After pairing, run `scan off` and `disconnect AA:BB:CC:DD:EE:FF` in bluetoothctl
so Parrot advertises again; leave that terminal open for its pairing agent.

First test preparation and Abort. This **erases the inactive app slot** but does
not change the running firmware or boot selection:

```sh
/tmp/parrot-ota-venv/bin/python scripts/ota_update.py \
  --device AA:BB:CC:DD:EE:FF \
  --file .pio/build/esp32-s3-dualeye-touch-lcd-1_28/firmware.bin --prepare-only
```

Then perform a real update (removing `--prepare-only` activates the image and
reboots Parrot):

```sh
/tmp/parrot-ota-venv/bin/python scripts/ota_update.py \
  --device AA:BB:CC:DD:EE:FF \
  --file .pio/build/esp32-s3-dualeye-touch-lcd-1_28/firmware.bin
```

Use `--adapter hci1` for another adapter. Only application firmware.bin is accepted;
do not pass bootloader, merged-flash or filesystem images. Initial size/magic
checks are only sanity checks; firmware validates the image before activation.
The board's two app slots are each 6,553,600 bytes. NVS and LittleFS are preserved.

Versions start at `1.0.0` in `include/firmware_version.h`. Before building the next
release, increase it (for example to `1.0.1`) and rebuild normally. PlatformIO
embeds the version/project into the image descriptor. Firmware rejects same/older
versions at Finish with error `0x0f`; transferring the currently running version
is therefore a rejection test, not a successful update. `--prepare-only` still
works because it does not reach Finish. Rollback to the previous working image
remains available; version enforcement does not burn anti-rollback eFuses.

The uploader waits for application acknowledgments and reads Status when a
notification is missing. It never blindly retries an unacknowledged chunk or
treats a disconnect as success. On error/Ctrl+C it attempts Abort; disconnect or
the firmware timeout also releases the session. Restart interrupted transfers
from zero. `complete` confirms validation and boot selection, not successful
boot/rollback: verify the device afterward. A lost completion notification plus
disconnect leaves the outcome unknown and returns failure.

Bleak's BlueZ backend needs its private `_acquire_mtu()` workaround, used before
Status subscription; the dependency is pinned for this reason. MTU below 40 is
rejected. See the [official Bleak MTU example](https://github.com/hbldh/bleak/blob/develop/examples/mtu_size.py)
and the authoritative [BLE OTA contract](../Doc/BLE.md#ble-ota-v1-contract-transfer-implemented).

Host tests (no Bluetooth or Bleak installation needed):

```sh
python3 -m unittest discover -s scripts -p 'test_*.py' -v
```

Hardware upload, power-loss recovery, and rollback remain unverified.
