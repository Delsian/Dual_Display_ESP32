package com.eugkrashtan.parrot

import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

data class OtaImage(val bytes: ByteArray, val version: String, val digest: ByteArray) {
    companion object {
        const val MAX_SIZE = 6_553_600
        fun versionParts(version: String): List<Long>? {
            if (!Regex("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)").matches(version)) return null
            return version.split('.').map { it.toLongOrNull()?.takeIf { n -> n <= 0xffffffffL } ?: return null }
        }
        fun higher(candidate: String, current: String): Boolean {
            val a = versionParts(candidate) ?: return false
            val b = versionParts(current) ?: return false
            for (i in 0..2) if (a[i] != b[i]) return a[i] > b[i]
            return false
        }
        fun read(input: InputStream): OtaImage {
            val output = java.io.ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (output.size() <= MAX_SIZE) {
                val count = input.read(buffer, 0, minOf(buffer.size, MAX_SIZE + 1 - output.size()))
                if (count < 0) break
                if (count == 0) continue
                output.write(buffer, 0, count)
            }
            val bytes = output.toByteArray()
            require(bytes.size in 288..MAX_SIZE) { "Invalid firmware size (maximum 6,553,600 bytes)" }
            require(bytes[0].toInt() and 255 == 0xe9 &&
                ByteBuffer.wrap(bytes, 32, 4).order(ByteOrder.LITTLE_ENDIAN).int == 0xabcd5432.toInt()) {
                "Select an ESP application firmware.bin, not a filesystem or merged image"
            }
            fun text(offset: Int): String {
                val end = (offset until offset + 32).firstOrNull { bytes[it] == 0.toByte() }
                    ?: error("Unterminated image metadata")
                return String(bytes, offset, end - offset, Charsets.US_ASCII)
            }
            val version = text(48)
            require(text(80) == "Parrot" && versionParts(version) != null) { "Expected a versioned Parrot firmware image" }
            return OtaImage(bytes, version, MessageDigest.getInstance("SHA-256").digest(bytes))
        }
    }
}
