package com.eugkrashtan.parrot

import org.junit.Assert.*
import org.junit.Test

class DeviceLogDecoderTest {
    private fun packet(sequence: Int, bytes: ByteArray, dropped: Int = 0) =
        byteArrayOf(sequence.toByte(), (sequence shr 8).toByte(), dropped.toByte(), (dropped shr 8).toByte()) + bytes

    @Test fun linesAndUtf8SurvivePacketBoundaries() {
        val decoder = DeviceLogDecoder()
        val bytes = "Привіт\r\nNext\n".toByteArray()
        val output = StringBuilder()
        bytes.forEachIndexed { sequence, byte -> output.append(decoder.accept(packet(sequence, byteArrayOf(byte)))) }
        assertEquals("Привіт\r\nNext\n", output.toString())
    }

    @Test fun gapDiscardsPartialLineAndReportsOverflow() {
        val decoder = DeviceLogDecoder()
        assertEquals("", decoder.accept(packet(0, "part".toByteArray())))
        val output = decoder.accept(packet(2, "tail\nvalid\n".toByteArray(), 12))
        assertTrue(output.contains("Log gap"))
        assertTrue(output.contains("overflow 12"))
        assertTrue(output.endsWith("valid\n"))
        assertFalse(output.contains("tail"))
        assertFalse(output.contains("part"))
    }

    @Test fun malformedAndOversizedLinesAreBounded() {
        val decoder = DeviceLogDecoder()
        assertTrue(decoder.accept(byteArrayOf()).contains("Malformed"))
        assertEquals("ok\n", decoder.accept(packet(0, "bad\nok\n".toByteArray())))
        decoder.reset()
        val output = StringBuilder()
        repeat(260) { output.append(decoder.accept(packet(it, "a".repeat(16).toByteArray()))) }
        output.append(decoder.accept(packet(260, "end\nok\n".toByteArray())))
        assertEquals("[Log line exceeded 4096 bytes]\nok\n", output.toString())
    }

    @Test fun sequenceWrapAndReconnectReset() {
        val decoder = DeviceLogDecoder()
        repeat(65536) { assertEquals("x\n", decoder.accept(packet(it, "x\n".toByteArray()))) }
        assertEquals("x\n", decoder.accept(packet(0, "x\n".toByteArray())))
        decoder.accept(packet(1, "partial".toByteArray()))
        decoder.reset()
        assertEquals("fresh\n", decoder.accept(packet(0, "fresh\n".toByteArray())))
    }
}
