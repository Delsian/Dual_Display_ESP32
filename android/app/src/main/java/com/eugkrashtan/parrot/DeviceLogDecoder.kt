package com.eugkrashtan.parrot

import java.io.ByteArrayOutputStream

/** Reassembles bounded UTF-8 lines across BLE packets; damaged partial lines are discarded. */
internal class DeviceLogDecoder {
    private val line = ByteArrayOutputStream()
    private var expected = 0
    private var discardLine = false

    fun reset() {
        line.reset()
        expected = 0
        discardLine = false
    }

    fun accept(packet: ByteArray): String {
        if (packet.size !in 5..20) {
            line.reset()
            discardLine = true
            return "[Malformed log packet]\n"
        }
        fun u16(offset: Int) = (packet[offset].toInt() and 255) or
            ((packet[offset + 1].toInt() and 255) shl 8)
        val sequence = u16(0)
        val dropped = u16(2)
        val output = StringBuilder()
        if (sequence != expected || dropped != 0) {
            line.reset()
            discardLine = true
            output.append("[Log gap: sequence $sequence, expected $expected; overflow $dropped bytes]\n")
        }
        expected = (sequence + 1) and 65535
        for (index in 4 until packet.size) {
            val value = packet[index].toInt() and 255
            if (discardLine) {
                if (value == 10) discardLine = false
                continue
            }
            if (line.size() >= 4096) {
                line.reset()
                discardLine = value != 10
                output.append("[Log line exceeded 4096 bytes]\n")
            } else {
                line.write(value)
                if (value == 10) {
                    output.append(line.toString("UTF-8"))
                    line.reset()
                }
            }
        }
        return output.toString()
    }
}
