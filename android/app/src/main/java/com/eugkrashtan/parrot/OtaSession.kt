package com.eugkrashtan.parrot

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** One serialized GATT operation plus its application acknowledgment at a time. */
class OtaSession(
    private val image: OtaImage, mtu: Int, private val now: () -> Long,
    private val send: (Operation) -> Unit, private val progress: (String) -> Unit,
    private val finished: (Result, String) -> Unit,
) {
    sealed class Operation {
        data object Subscribe : Operation()
        data object Read : Operation()
        data class Write(val control: Boolean, val bytes: ByteArray) : Operation()
    }
    enum class Result { COMPLETE, CANCELLED, FAILED }
    private enum class Phase { SUBSCRIBE, INITIAL, BEGIN, DATA, FINISH, ABORT }
    private var phase = Phase.SUBSCRIBE
    private var pending = false
    private var deadline = 0L
    private var pollAt = 0L
    private var status: Triple<Int, Long, Int>? = null
    private var expected = 0
    private var cancelled = false
    private var begun = false
    private var percent = -1
    private val startedAt = now()
    private val chunkSize = minOf(mtu - 7, 508)
    var ended = false
        private set

    init { require(mtu >= 40) { "OTA needs MTU 40 or higher" } }
    fun start() { progress("OTA: subscribing"); issue(Operation.Subscribe, true) }
    private fun issue(op: Operation, resetDeadline: Boolean) {
        if (resetDeadline) deadline = now() + 20_000
        pending = true
        send(op)
    }
    fun cancel() { cancelled = true; advance() }
    fun disconnected() {
        if (!ended) finish(Result.FAILED, "OTA disconnected before completion; outcome unknown. Retry from the beginning.")
    }
    fun fail(message: String) { if (!ended) finish(Result.FAILED, message) }
    fun operationDone(success: Boolean, value: ByteArray? = null) {
        if (ended) return
        if (!success) { fail("OTA BLE operation failed"); return }
        pending = false
        if (phase == Phase.SUBSCRIBE) {
            phase = Phase.INITIAL
            issue(Operation.Read, true)
            return
        }
        if (value != null) {
            if (!parse(value)) return
            if (phase == Phase.INITIAL) {
                if (status!!.first !in listOf(0, 5)) { fail("OTA already active on device"); return }
                if (cancelled) { finish(Result.CANCELLED, "OTA cancelled before transfer"); return }
                begun = true
                phase = Phase.BEGIN
                status = null
                progress("OTA: preparing flash for ${image.version}")
                val begin = ByteBuffer.allocate(37).order(ByteOrder.LITTLE_ENDIAN)
                    .put(1).putInt(image.bytes.size).put(image.digest).array()
                issue(Operation.Write(true, begin), true)
                return
            }
        }
        pollAt = now() + 500
        advance()
    }
    private fun parse(packet: ByteArray): Boolean {
        if (packet.size != 6) { fail("Malformed OTA status"); return false }
        val state = packet[0].toInt() and 255
        val offset = ByteBuffer.wrap(packet, 1, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xffffffffL
        val error = packet[5].toInt() and 255
        if (state > 5 || error > 15) { fail("Unknown OTA status"); return false }
        status = Triple(state, offset, error)
        return true
    }
    fun notification(packet: ByteArray) {
        if (ended || phase == Phase.SUBSCRIBE || phase == Phase.INITIAL) return
        if (parse(packet)) advance()
    }
    fun tick() {
        if (ended) return
        if (now() - startedAt >= 30 * 60_000) { fail("OTA exceeded 30 minutes; reconnect and retry"); return }
        if (now() >= deadline) { fail("OTA timed out; reconnect and retry from the beginning"); return }
        if (!pending && now() >= pollAt) issue(Operation.Read, false)
    }
    private fun advance() {
        if (ended || pending) return
        val received = status
        if (phase == Phase.FINISH && received == Triple(4, image.bytes.size.toLong(), 0)) {
            finish(Result.COMPLETE, "OTA accepted ${image.version}; waiting for reboot")
            return
        }
        if (cancelled && phase != Phase.ABORT) {
            if (!begun) { finish(Result.CANCELLED, "OTA cancelled"); return }
            phase = Phase.ABORT; status = null
            progress("OTA: cancelling")
            issue(Operation.Write(true, byteArrayOf(3)), true)
            return
        }
        val (state, offset, error) = received ?: return
        if (phase == Phase.ABORT && state == 5 && error == 12) {
            finish(Result.CANCELLED, "OTA cancelled; boot selection unchanged"); return
        }
        if (error != 0 || state == 5) {
            val reason = if (error == 15) "firmware version must be higher" else "device error 0x${error.toString(16)}"
            fail("OTA rejected: $reason (accepted $offset bytes)"); return
        }
        if (offset > expected) { fail("Unexpected OTA offset $offset"); return }
        if (phase !in listOf(Phase.BEGIN, Phase.DATA) || state != 2 || offset != expected.toLong()) return
        val newPercent = expected * 100 / image.bytes.size
        if (newPercent != percent) { percent = newPercent; progress("OTA ${image.version}: $percent% ($expected/${image.bytes.size} bytes)") }
        status = null
        if (expected == image.bytes.size) {
            phase = Phase.FINISH
            progress("OTA: verifying ${image.version}")
            issue(Operation.Write(true, byteArrayOf(2)), true)
        } else {
            val start = expected
            val count = minOf(chunkSize, image.bytes.size - start)
            expected += count
            phase = Phase.DATA
            val packet = ByteBuffer.allocate(4 + count).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(start).put(image.bytes, start, count).array()
            issue(Operation.Write(false, packet), true)
        }
    }
    private fun finish(result: Result, message: String) {
        ended = true
        finished(result, message)
    }
}
