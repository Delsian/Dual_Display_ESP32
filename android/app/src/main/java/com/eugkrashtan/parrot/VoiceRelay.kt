package com.eugkrashtan.parrot

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.SystemClock
import android.os.Looper
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

class VoiceRelay(private val context: Context, private val listener: Listener) {
    interface Listener {
        fun relayConfig(): Config
        fun onStatus(message: String)
    }

    data class Config(val apiKey: String)

    private val generation = AtomicLong()
    private val classifier = GeminiIntent(context.assets.open("intent_topics.json").bufferedReader().use { it.readText() })

    private val adapter = BluetoothAdapter.getDefaultAdapter()
    private val scanner get() = adapter?.bluetoothLeScanner
    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var tx: BluetoothGattCharacteristic? = null
    private var pcm = ByteArrayOutputStream()
    private var expectedSamples = 0
    private var sampleRate = 16000
    private var subscribed = false
    private var voiceBusy = false
    private var writePending = false
    private var pendingPlay: String? = null

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(type: Int, result: ScanResult) {
            val name = result.device.name ?: return
            if (name.startsWith("Parrot-")) {
                stopScan()
                listener.onStatus("Connecting to $name")
                connect(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            listener.onStatus("BLE scan failed: $errorCode")
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                listener.onStatus("Connected; discovering services")
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                main.post {
                    if (this@VoiceRelay.gatt === gatt) {
                        listener.onStatus("Disconnected")
                        close()
                    }
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onStatus("Service discovery failed: $status")
                return
            }
            val service = gatt.getService(VOICE_SERVICE) ?: run {
                listener.onStatus("Voice service not found")
                return
            }
            tx = service.getCharacteristic(VOICE_TX)
            rx = service.getCharacteristic(VOICE_RX)
            if (tx == null || rx == null) {
                listener.onStatus("Voice characteristics not found")
                return
            }
            listener.onStatus("Negotiating BLE MTU")
            gatt.requestMtu(185)
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS || mtu < 168) {
                listener.onStatus("BLE MTU too small: $mtu")
                return
            }
            val characteristic = tx ?: return
            if (!gatt.setCharacteristicNotification(characteristic, true)) {
                listener.onStatus("Could not subscribe to audio")
                return
            }
            val descriptor = characteristic.getDescriptor(CCCD) ?: run {
                listener.onStatus("Notification descriptor missing")
                return
            }
            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            listener.onStatus("Subscribing to audio")
            gatt.writeDescriptor(descriptor)
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            main.post {
                if (this@VoiceRelay.gatt !== gatt) return@post
                subscribed = status == BluetoothGatt.GATT_SUCCESS
                listener.onStatus(if (subscribed) "Ready" else "Subscription failed")
            }
        }

        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            main.post {
                if (this@VoiceRelay.gatt !== gatt || characteristic.uuid != VOICE_RX) return@post
                writePending = false
                val clip = pendingPlay
                pendingPlay = null
                if (status != BluetoothGatt.GATT_SUCCESS) listener.onStatus("BLE write failed: $status")
                else if (clip != null) listener.onStatus("Play request sent: $clip")
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val packet = characteristic.value.copyOf()
            main.post { if (this@VoiceRelay.gatt === gatt) handlePacket(packet) }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            val packet = value.copyOf()
            main.post { if (this@VoiceRelay.gatt === gatt) handlePacket(packet) }
        }
    }

    @SuppressLint("MissingPermission")
    fun scanAndConnect() {
        if (!hasBluetoothPermission()) {
            listener.onStatus("Grant Bluetooth Nearby devices permission")
            return
        }
        stopScan()
        listener.onStatus("Scanning for Parrot")
        scanner?.startScan(scanCallback)
        main.postDelayed({
            stopScan()
            if (gatt == null) listener.onStatus("No Parrot found")
        }, 12_000)
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        close()
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        scanner?.stopScan(scanCallback)
    }

    @SuppressLint("MissingPermission")
    fun close() {
        generation.incrementAndGet()
        subscribed = false
        voiceBusy = false
        writePending = false
        pendingPlay = null
        stopScan()
        gatt?.close()
        gatt = null
        rx = null
        tx = null
    }

    private fun handlePacket(packet: ByteArray) {
        if (packet.isEmpty()) return
        when (packet[0].toInt() and 0xff) {
            0x01 -> {
                if (packet.size < 3) return
                generation.incrementAndGet()
                voiceBusy = true
                sampleRate = u16(packet, 1)
                pcm = ByteArrayOutputStream()
                listener.onStatus("Recording at ${sampleRate} Hz")
            }
            0x02 -> decodeAudio(packet)
            0x03 -> {
                if (packet.size < 5) return
                expectedSamples = u32(packet, 1)
                listener.onStatus("Uploading recording")
                uploadRecording()
            }
            0x04 -> {
                generation.incrementAndGet()
                pcm.reset()
                voiceBusy = false
                listener.onStatus("Recording cancelled")
            }
        }
    }

    private fun decodeAudio(packet: ByteArray) {
        if (packet.size < 6) return
        var predictor = signed16(packet, 2)
        var index = packet[4].toInt() and 0xff
        if (index > 88) return
        val bytes = packet.copyOfRange(5, packet.size)

        fun decodeNibble(nibble: Int, output: (Int) -> Unit) {
            val step = STEP_TABLE[index]
            var difference = step shr 3
            if ((nibble and 1) != 0) difference += step shr 2
            if ((nibble and 2) != 0) difference += step shr 1
            if ((nibble and 4) != 0) difference += step
            predictor = if ((nibble and 8) != 0) predictor - difference else predictor + difference
            predictor = predictor.coerceIn(-32768, 32767)
            index = (index + INDEX_TABLE[nibble]).coerceIn(0, 88)
            output(predictor)
        }

        for (value in bytes) {
            decodeNibble(value.toInt() and 0x0f) { sample -> writeSample(sample) }
            decodeNibble((value.toInt() ushr 4) and 0x0f) { sample -> writeSample(sample) }
        }
    }

    private fun writeSample(sample: Int) {
        pcm.write(sample and 0xff)
        pcm.write((sample ushr 8) and 0xff)
    }

    private fun uploadRecording() {
        val samples = expectedSamples
        val rate = sampleRate
        val captured = pcm.toByteArray()
        val job = generation.incrementAndGet()
        val deadline = SystemClock.elapsedRealtime() + 25_000
        // Snapshot the configuration on the UI thread; never read EditText from a worker.
        main.post {
            if (generation.get() != job) return@post
            if (rate != 16000 || samples !in 4000..480000 ||
                captured.size < samples * 2 || captured.size > samples * 2 + 2) {
                writeReply("error:recording")
                return@post
            }
            val config = listener.relayConfig()
            val timeout = Runnable {
                if (generation.compareAndSet(job, job + 1)) {
                    writeReply("error:timeout")
                }
            }
            // Firmware gives us 30 seconds. Leave time for the BLE write.
            main.postDelayed(timeout, 25_000)
            thread(name = "parrot-gemini") {
                val response = try {
                    classifier.classify(wav(captured.copyOf(samples * 2), rate), config.apiKey)
                } catch (_: Exception) {
                    // Never expose provider bodies or exception details containing credentials.
                    "error:gemini"
                }
                main.post {
                    main.removeCallbacks(timeout)
                    if (generation.compareAndSet(job, job + 1)) {
                        writeReply(if (SystemClock.elapsedRealtime() < deadline) response else "error:timeout")
                    }
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun playStoredClip(input: String) {
        val name = ClipSelection.normalize(input)
        if (name == null) {
            listener.onStatus("Enter a clip number 1–999 or name such as off_1 (no .wav)")
            return
        }
        val connection = gatt
        val characteristic = rx
        if (!hasBluetoothPermission() || !subscribed || connection == null || characteristic == null) {
            listener.onStatus("Connect and wait for Ready before playing a clip")
            return
        }
        if (voiceBusy || writePending) {
            listener.onStatus("Wait for the current request to finish")
            return
        }
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        characteristic.value = "play:$name".toByteArray(Charsets.UTF_8)
        writePending = true
        pendingPlay = name
        if (!connection.writeCharacteristic(characteristic)) {
            writePending = false
            pendingPlay = null
            listener.onStatus("Could not send play request; try again")
        } else listener.onStatus("Sending play request: $name")
    }

    @SuppressLint("MissingPermission")
    private fun writeReply(reply: String) {
        voiceBusy = false
        val characteristic = rx ?: return
        val connection = gatt ?: return
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        characteristic.value = reply.toByteArray(Charsets.UTF_8).let { it.copyOf(minOf(it.size, 63)) }
        writePending = true
        if (!connection.writeCharacteristic(characteristic)) {
            writePending = false
            listener.onStatus("Could not send reply")
        } else listener.onStatus("Reply sent: ${reply.substringBefore(':')}")
    }

    private fun wav(pcm: ByteArray, rate: Int): ByteArray {
        val output = ByteArray(44 + pcm.size)
        val view = ByteBuffer.wrap(output).order(ByteOrder.LITTLE_ENDIAN)
        output[0] = 'R'.code.toByte(); output[1] = 'I'.code.toByte(); output[2] = 'F'.code.toByte(); output[3] = 'F'.code.toByte()
        view.putInt(4, 36 + pcm.size)
        output[8] = 'W'.code.toByte(); output[9] = 'A'.code.toByte(); output[10] = 'V'.code.toByte(); output[11] = 'E'.code.toByte()
        output[12] = 'f'.code.toByte(); output[13] = 'm'.code.toByte(); output[14] = 't'.code.toByte(); output[15] = ' '.code.toByte()
        view.putInt(16, 16); view.putShort(20, 1); view.putShort(22, 1); view.putInt(24, rate); view.putInt(28, rate * 2); view.putShort(32, 2); view.putShort(34, 16)
        output[36] = 'd'.code.toByte(); output[37] = 'a'.code.toByte(); output[38] = 't'.code.toByte(); output[39] = 'a'.code.toByte()
        view.putInt(40, pcm.size)
        pcm.copyInto(output, 44)
        return output
    }

    private fun hasBluetoothPermission() = android.os.Build.VERSION.SDK_INT < 31 ||
        context.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
        context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun u16(data: ByteArray, offset: Int) = (data[offset].toInt() and 0xff) or ((data[offset + 1].toInt() and 0xff) shl 8)
    private fun u32(data: ByteArray, offset: Int) = u16(data, offset) or (u16(data, offset + 2) shl 16)
    private fun signed16(data: ByteArray, offset: Int) = u16(data, offset).toShort().toInt()

    companion object {
        private val VOICE_SERVICE = UUID.fromString("6b520010-7c8e-4c30-9aa8-45e626d39b01")
        private val VOICE_TX = UUID.fromString("6b520011-7c8e-4c30-9aa8-45e626d39b01")
        private val VOICE_RX = UUID.fromString("6b520012-7c8e-4c30-9aa8-45e626d39b01")
        private val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private val INDEX_TABLE = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
        private val STEP_TABLE = intArrayOf(7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)
    }
}
