package com.eugkrashtan.parrot

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

class OtaSessionTest {
    private fun binary(version: String = "1.2.3") = ByteArray(288).also {
        it[0] = 0xe9.toByte()
        ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putInt(32, 0xabcd5432.toInt())
        version.toByteArray().copyInto(it, 48)
        "Parrot".toByteArray().copyInto(it, 80)
    }
    private fun status(state: Int, offset: Int = 0, error: Int = 0) =
        ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN).put(state.toByte()).putInt(offset).put(error.toByte()).array()

    private class Rig(val image: OtaImage) {
        var time = 0L
        val operations = mutableListOf<OtaSession.Operation>()
        var result: OtaSession.Result? = null
        val session = OtaSession(image, 185, { time }, { operations.add(it) }, {}, { r, _ -> result = r })
        fun begin() {
            session.start()
            assertEquals(OtaSession.Operation.Subscribe, operations.last())
            session.operationDone(true)
            assertEquals(OtaSession.Operation.Read, operations.last())
            session.operationDone(true, ByteArray(6))
        }
    }

    @Test fun validatesMetadataHashAndNumericVersions() {
        val bytes = binary()
        val image = OtaImage.read(bytes.inputStream())
        assertEquals("1.2.3", image.version)
        assertArrayEquals(MessageDigest.getInstance("SHA-256").digest(bytes), image.digest)
        assertTrue(OtaImage.higher("1.10.0", "1.9.9"))
        assertFalse(OtaImage.higher("1.2.3", "1.2.3"))
        assertFalse(OtaImage.higher("1.2.2", "1.2.3"))
        for (v in listOf("01.2.3", "1.2", "-1.2.3", "4294967296.0.0", "1.2.3-beta")) {
            assertNull(OtaImage.versionParts(v))
        }
        for (bad in listOf(ByteArray(287), binary().also { it[0] = 0 }, binary().also { it[80] = 0 }, binary("01.2.3"))) {
            assertThrows(IllegalArgumentException::class.java) { OtaImage.read(bad.inputStream()) }
        }
    }

    @Test fun requiresGattCompletionAndFlashAcknowledgmentBeforeNextChunk() {
        val r = Rig(OtaImage.read(binary().inputStream()))
        r.begin()
        val begin = (r.operations.last() as OtaSession.Operation.Write).bytes
        assertEquals(37, begin.size)
        assertEquals(288, ByteBuffer.wrap(begin, 1, 4).order(ByteOrder.LITTLE_ENDIAN).int)
        assertArrayEquals(r.image.digest, begin.copyOfRange(5, 37))
        r.session.notification(status(2))
        assertEquals(3, r.operations.size) // notification arrived before write callback
        r.session.operationDone(true)
        val first = r.operations.last() as OtaSession.Operation.Write
        assertFalse(first.control)
        assertEquals(182, first.bytes.size)
        r.session.operationDone(true)
        assertEquals(4, r.operations.size) // GATT ACK alone does not advance
        r.session.notification(status(2, 178))
        assertEquals(114, (r.operations.last() as OtaSession.Operation.Write).bytes.size)
        r.session.operationDone(true)
        r.session.notification(status(2, 288))
        assertArrayEquals(byteArrayOf(2), (r.operations.last() as OtaSession.Operation.Write).bytes)
        r.session.notification(status(4, 288))
        assertNull(r.result)
        r.session.operationDone(true)
        assertEquals(OtaSession.Result.COMPLETE, r.result)
    }

    @Test fun readsStatusForMissingNotificationWithoutResendingData() {
        val r = Rig(OtaImage.read(binary().inputStream()))
        r.begin(); r.session.operationDone(true)
        r.time = 500; r.session.tick()
        assertEquals(OtaSession.Operation.Read, r.operations.last())
        r.session.operationDone(true, status(2))
        assertFalse((r.operations.last() as OtaSession.Operation.Write).control)
        r.session.operationDone(true)
        r.time = 1000; r.session.tick()
        r.session.operationDone(true, status(2, 178))
        assertEquals(2, r.operations.filterIsInstance<OtaSession.Operation.Write>().count { !it.control })
    }

    @Test fun cancellationWaitsForOutstandingWriteThenAbortAcknowledgment() {
        val r = Rig(OtaImage.read(binary().inputStream()))
        r.begin(); r.session.cancel()
        assertEquals(3, r.operations.size)
        r.session.operationDone(true)
        assertArrayEquals(byteArrayOf(3), (r.operations.last() as OtaSession.Operation.Write).bytes)
        r.session.operationDone(true); r.session.notification(status(5, 0, 12))
        assertEquals(OtaSession.Result.CANCELLED, r.result)
    }

    @Test fun rejectsDeviceErrorsImpossibleOffsetsDisconnectAndTimeout() {
        for (packet in listOf(status(5, 0, 15), status(2, 1), byteArrayOf(2))) {
            val r = Rig(OtaImage.read(binary().inputStream()))
            r.begin(); r.session.operationDone(true); r.session.notification(packet)
            assertEquals(OtaSession.Result.FAILED, r.result)
        }
        val timed = Rig(OtaImage.read(binary().inputStream()))
        timed.begin(); timed.time = 20_000; timed.session.tick()
        assertEquals(OtaSession.Result.FAILED, timed.result)
        val lost = Rig(OtaImage.read(binary().inputStream()))
        lost.begin(); lost.session.disconnected()
        assertEquals(OtaSession.Result.FAILED, lost.result)
    }
}
