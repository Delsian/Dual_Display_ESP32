import hashlib
import struct
import unittest
from unittest.mock import patch

from ota_update import CONTROL, DATA, Uploader, decode_status


class FakeDevice:
    def __init__(self, notify=True, fail_hash=False, disconnect=False):
        self.is_connected = True
        self.status = (0, 0, 0)
        self.received = bytearray()
        self.writes = []
        self.notify = notify
        self.fail_hash = fail_hash
        self.disconnect = disconnect

    async def read_gatt_char(self, _uuid):
        return struct.pack("<BIB", *self.status)

    async def write_gatt_char(self, uuid, packet, response):
        assert response is True
        self.writes.append((uuid, packet))
        if uuid == CONTROL and packet[0] == 1:
            self.size = struct.unpack_from("<I", packet, 1)[0]
            self.digest = packet[5:]
            assert len(packet) == 37
            self.status = (2, 0, 0)
        elif uuid == DATA:
            offset = struct.unpack_from("<I", packet)[0]
            assert offset == len(self.received)
            self.received.extend(packet[4:])
            self.status = (2, len(self.received), 0)
        elif packet == b"\x02":
            assert len(self.received) == self.size
            assert hashlib.sha256(self.received).digest() == self.digest
            self.status = (5, self.size, 8) if self.fail_hash else (4, self.size, 0)
            if self.disconnect:
                self.is_connected = False
                return
        elif packet == b"\x03":
            self.status = (5, len(self.received), 12)
        if self.notify:
            self.uploader.notified(None, struct.pack("<BIB", *self.status))


class UploadTests(unittest.IsolatedAsyncioTestCase):
    def uploader(self, **kwargs):
        device = FakeDevice(**kwargs)
        device.uploader = Uploader(device, timeout=2)
        return device, device.uploader

    async def test_full_upload_partial_last_chunk(self):
        device, uploader = self.uploader()
        image = b"\xe9" + bytes(range(256)) * 2
        with patch("builtins.print"):
            await uploader.run(image, 185)
        self.assertTrue(uploader.complete)
        self.assertEqual(device.received, image)
        chunks = [p for uuid, p in device.writes if uuid == DATA]
        self.assertEqual([len(p) - 4 for p in chunks], [178, 178, 157])
        self.assertEqual(device.writes[-1], (CONTROL, b"\x02"))

    async def test_missing_notifications_read_status_without_replay(self):
        device, uploader = self.uploader(notify=False)
        with patch("builtins.print"):
            await uploader.run(b"\xe9" * 24, 185)
        self.assertTrue(uploader.complete)
        self.assertEqual(len([p for uuid, p in device.writes if uuid == DATA]), 1)

    async def test_prepare_only_aborts_without_data_or_finish(self):
        device, uploader = self.uploader()
        with patch("builtins.print"):
            await uploader.run(b"\xe9" * 24, 40, prepare_only=True)
        self.assertEqual([p[0] for _, p in device.writes], [1, 3])
        self.assertFalse(uploader.complete)

    async def test_hash_failure_attempts_abort(self):
        device, uploader = self.uploader(fail_hash=True)
        with patch("builtins.print"), self.assertRaisesRegex(RuntimeError, "SHA-256"):
            await uploader.run(b"\xe9" * 24, 185)
        self.assertEqual(device.writes[-1], (CONTROL, b"\x03"))
        self.assertFalse(uploader.complete)

    async def test_disconnect_is_not_success(self):
        _, uploader = self.uploader(disconnect=True)
        with patch("builtins.print"), self.assertRaisesRegex(RuntimeError, "outcome unknown"):
            await uploader.run(b"\xe9" * 24, 185)
        self.assertFalse(uploader.complete)

    async def test_mtu_too_small_sends_nothing(self):
        device, uploader = self.uploader()
        with self.assertRaises(ValueError):
            await uploader.run(b"\xe9" * 24, 23)
        self.assertEqual(device.writes, [])

    async def test_malformed_and_unknown_status(self):
        for packet in (b"", bytes(5), bytes(7), b"\xff" + bytes(5), bytes(5) + b"\xff"):
            with self.assertRaises(RuntimeError):
                decode_status(packet)


if __name__ == "__main__":
    unittest.main()
