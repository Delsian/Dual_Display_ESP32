#!/usr/bin/env python3
"""Linux/BlueZ uploader for Parrot BLE OTA v1; see scripts/README.md."""
import argparse
import asyncio
import hashlib
from pathlib import Path
import struct
import sys
import time

SUFFIX = "-7c8e-4c30-9aa8-45e626d39b01"
SERVICE, CONTROL, DATA, STATUS = [f"6b5200{x:02x}{SUFFIX}" for x in range(0x30, 0x34)]
ERRORS = ["none", "malformed packet", "invalid state/busy", "image bounds",
          "unexpected offset", "MTU too small", "no inactive partition", "flash failure",
          "SHA-256 mismatch", "invalid image", "boot selection failed", "timeout",
          "aborted", "disconnected", "resource unavailable", "firmware version must be higher (or invalid project/version)"]


def decode_status(packet):
    if len(packet) != 6:
        raise RuntimeError(f"Invalid status length: {len(packet)}")
    state, offset, error = struct.unpack("<BIB", packet)
    if state > 5 or error >= len(ERRORS):
        raise RuntimeError(f"Unknown OTA status: {packet.hex()}")
    return state, offset, error


def read_image(path):
    # This is a local sanity check, not a replacement for ESP image validation.
    if not 24 <= path.stat().st_size <= 0x640000:
        raise ValueError("Image must be 24–6,553,600 bytes for this board")
    image = path.read_bytes()
    if image[0] != 0xE9:
        raise ValueError("Expected an ESP application firmware.bin (magic 0xE9)")
    return image


class Uploader:
    def __init__(self, client, timeout=20):
        self.client = client
        self.timeout = timeout
        self.events = asyncio.Queue(maxsize=16)
        self.started = False
        self.complete = False

    def notified(self, _characteristic, packet):
        if self.events.full():
            self.events.get_nowait()
        self.events.put_nowait(bytes(packet))

    async def wait_status(self, state, offset, allowed_error=0):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            try:
                packet = await asyncio.wait_for(self.events.get(), min(0.5, deadline - time.monotonic()))
            except asyncio.TimeoutError:
                if not self.client.is_connected:
                    raise RuntimeError("Disconnected before completion was confirmed; update outcome unknown")
                packet = await asyncio.wait_for(self.client.read_gatt_char(STATUS), 5)
            current, accepted, error = decode_status(packet)
            if current == state and accepted == offset and error == allowed_error:
                return
            if error or current == 5:
                raise RuntimeError(f"OTA {ERRORS[error]} (state={current}, accepted={accepted})")
            if accepted > offset:
                raise RuntimeError(f"Unexpected acknowledged offset {accepted}, expected {offset}")
        raise TimeoutError(f"No OTA acknowledgment for state={state}, offset={offset}")

    async def write(self, characteristic, packet):
        await asyncio.wait_for(self.client.write_gatt_char(characteristic, packet, response=True), 10)

    async def run(self, image, mtu, prepare_only=False):
        if mtu < 40:
            raise ValueError(f"Negotiated MTU {mtu}; at least 40 is required")
        chunk_size = min(mtu - 7, 508)
        initial = decode_status(await self.client.read_gatt_char(STATUS))
        if initial[0] not in (0, 5):
            raise RuntimeError("Device already has an OTA session; wait for timeout or disconnect it")
        while not self.events.empty():
            self.events.get_nowait()
        digest = hashlib.sha256(image).digest()
        print(f"Image: {len(image)} bytes; SHA-256: {digest.hex()}")
        print(f"MTU: {mtu}; payload: {chunk_size} bytes")
        self.started = True  # Even a failed write can have reached the device.
        try:
            await self.write(CONTROL, b"\x01" + struct.pack("<I", len(image)) + digest)
            await self.wait_status(2, 0)
            if prepare_only:
                await self.write(CONTROL, b"\x03")
                await self.wait_status(5, 0, 12)
                self.started = False
                print("Preparation and Abort confirmed; boot selection unchanged.")
                return
            started = time.monotonic()
            last_percent = -1
            for offset in range(0, len(image), chunk_size):
                chunk = image[offset:offset + chunk_size]
                await self.write(DATA, struct.pack("<I", offset) + chunk)
                accepted = offset + len(chunk)
                await self.wait_status(2, accepted)
                percent = accepted * 100 // len(image)
                if percent != last_percent:
                    print(f"\rUploaded {accepted}/{len(image)} bytes ({percent}%)", end="", flush=True)
                    last_percent = percent
            print(f"\nTransfer: {time.monotonic() - started:.1f}s; verifying image...")
            await self.write(CONTROL, b"\x02")
            await self.wait_status(4, len(image))
            self.complete = True
            print("Device confirmed complete; restart requested. Verify the new firmware on hardware.")
            await asyncio.sleep(1)
        finally:
            if self.started and not self.complete and self.client.is_connected:
                try:
                    await self.write(CONTROL, b"\x03")
                except Exception:
                    pass  # Disconnect/firmware timeout also cleans up the session.


async def main(args):
    image = read_image(args.file)
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise RuntimeError("Install scripts/requirements.txt in a Python virtual environment") from exc
    device = await BleakScanner.find_device_by_address(args.device, timeout=15, adapter=args.adapter)
    if device is None:
        raise RuntimeError("Device not found. Stop Android relay and wake/reset Parrot, then retry")
    print("Connecting/pairing; enter the Parrot passkey through your Linux Bluetooth agent.")
    async with BleakClient(device, pair=True, timeout=90, adapter=args.adapter) as client:
        if client.services.get_service(SERVICE) is None:
            raise RuntimeError("OTA service missing: install OTA-capable firmware over USB first")
        for uuid in (CONTROL, DATA, STATUS):
            if client.services.get_characteristic(uuid) is None:
                raise RuntimeError(f"OTA characteristic missing: {uuid}")
        # BlueZ does not expose negotiated MTU via Bleak's public API.
        # Follow Bleak's mtu_size example before subscribing to Status.
        acquire = getattr(client._backend, "_acquire_mtu", None)
        if acquire is None:
            raise RuntimeError("BlueZ MTU acquisition unavailable; use the pinned Bleak version")
        await asyncio.wait_for(acquire(), 15)
        uploader = Uploader(client)
        await client.start_notify(STATUS, uploader.notified)
        await uploader.run(image, client.mtu_size, args.prepare_only)


def cli():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="Parrot BLE address, e.g. AA:BB:CC:DD:EE:FF")
    parser.add_argument("--file", required=True, type=Path, help="Application firmware.bin")
    parser.add_argument("--adapter", default="hci0", help="Linux Bluetooth adapter (default: hci0)")
    parser.add_argument("--prepare-only", action="store_true", help="Begin then Abort; erases inactive slot but does not activate an image")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This script targets Linux/BlueZ")
    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        print("\nCancelled; disconnect releases the OTA session.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"OTA failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(cli())
