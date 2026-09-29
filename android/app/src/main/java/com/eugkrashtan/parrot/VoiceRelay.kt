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
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanSettings
import android.os.ParcelUuid
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

class VoiceRelay(
    private val context: Context,
    private val listener: Listener,
    private val classifyRecording: (ByteArray, String) -> String = GeminiIntent(
        context.assets.open("intent_topics.json").bufferedReader().use { it.readText() }
    )::classify,
    private val executeRequest: (() -> Unit) -> Unit = { work -> thread(name = "parrot-gemini") { work() } },
) {
    interface Listener {
        fun relayConfig(): Config
        fun onStatus(message: String)
        fun onRequestActive(active: Boolean) {}
        fun onBatteryLevel(level: Int?) {}
    }

    data class Config(val apiKey: String, val paidApiKey: String = "")

    private val generation = AtomicLong()
    private val keyFallback = GeminiKeyFallback { SystemClock.elapsedRealtime() }

    private val adapter = BluetoothAdapter.getDefaultAdapter()
    private val scanner get() = adapter?.bluetoothLeScanner
    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var tx: BluetoothGattCharacteristic? = null
    private var battery: BluetoothGattCharacteristic? = null
    private var pcm = ByteArrayOutputStream()
    private var expectedSamples = 0
    private var sampleRate = 16000
    private var subscribed = false
    private var voiceBusy = false
        set(value) {
            field = value
            listener.onRequestActive(value)
        }
    private var running = false
    private var activeScan: ScanCallback? = null
    private var activeScanner: BluetoothLeScanner? = null
    private var retryDelay = 5_000L
    private val reconnect = Runnable { scanAndConnect() }
    private val setupTimeout = Runnable { retry("Connection or pairing timed out") }
    private val writeTimeout = Runnable { retry("BLE write timed out") }
    private var writePending = false
    private var pendingPlay: String? = null

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (!running || this@VoiceRelay.gatt !== gatt) return
            if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                retry("Bluetooth permission unavailable")
                return
            }
            if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                retry("Disconnected; waiting for Parrot")
            } else if (newState == BluetoothProfile.STATE_CONNECTED) {
                listener.onStatus("Connected; discovering services")
                bleOperation { gatt.discoverServices() }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (!running || this@VoiceRelay.gatt !== gatt) return
            if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                retry("Bluetooth permission unavailable")
                return
            }
            if (status != BluetoothGatt.GATT_SUCCESS) {
                retry("Service discovery failed: $status")
                return
            }
            val service = gatt.getService(VOICE_SERVICE) ?: run {
                retry("Voice service not found")
                return
            }
            tx = service.getCharacteristic(VOICE_TX)
            rx = service.getCharacteristic(VOICE_RX)
            if (tx == null || rx == null) {
                retry("Voice characteristics not found")
                return
            }
            battery = gatt.getService(BATTERY_SERVICE)?.getCharacteristic(BATTERY_LEVEL)
            val level = battery
            bleOperation {
                if (level != null && gatt.readCharacteristic(level)) true
                else gatt.requestMtu(185)
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (!running || this@VoiceRelay.gatt !== gatt) return
            if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                retry("Bluetooth permission unavailable")
                return
            }
            if (status != BluetoothGatt.GATT_SUCCESS || mtu < 168) {
                retry("BLE MTU too small: $mtu")
                return
            }
            bleOperation {
                val characteristic = battery
                val descriptor = characteristic?.getDescriptor(CCCD)
                if (characteristic != null && descriptor != null &&
                    gatt.setCharacteristicNotification(characteristic, true)) {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    if (gatt.writeDescriptor(descriptor)) return@bleOperation true
                }
                subscribeVoice(gatt)
                true
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            batteryRead(gatt, characteristic, characteristic.value ?: byteArrayOf(), status)
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray, status: Int,
        ) {
            batteryRead(gatt, characteristic, value, status)
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (!running || this@VoiceRelay.gatt !== gatt || descriptor.uuid != CCCD) return
            if (descriptor.characteristic === battery) {
                subscribeVoice(gatt)
                return
            }
            if (descriptor.characteristic !== tx) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                retry("Subscription failed; check pairing")
                return
            }
            subscribed = true
            retryDelay = 5_000L
            main.removeCallbacks(setupTimeout)
            listener.onStatus("Ready")
        }

        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            main.post {
                if (this@VoiceRelay.gatt !== gatt || characteristic.uuid != VOICE_RX) return@post
                main.removeCallbacks(writeTimeout)
                writePending = false
                val clip = pendingPlay
                pendingPlay = null
                if (clip == null) voiceBusy = false
                if (status != BluetoothGatt.GATT_SUCCESS) retry("BLE write failed: $status")
                else if (clip != null) listener.onStatus("Play request sent: $clip")
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            routeNotification(gatt, characteristic, characteristic.value ?: byteArrayOf())
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            routeNotification(gatt, characteristic, value)
        }
    }

    @SuppressLint("MissingPermission")
    private fun batteryRead(connection: BluetoothGatt, characteristic: BluetoothGattCharacteristic,
                            value: ByteArray, status: Int) {
        if (!running || gatt !== connection || characteristic !== battery) return
        if (status == BluetoothGatt.GATT_SUCCESS) updateBattery(value)
        listener.onStatus("Negotiating BLE MTU")
        bleOperation { connection.requestMtu(185) }
    }

    @SuppressLint("MissingPermission")
    private fun subscribeVoice(connection: BluetoothGatt) {
        val characteristic = tx ?: return
        val descriptor = characteristic.getDescriptor(CCCD) ?: run {
            retry("Notification descriptor missing")
            return
        }
        listener.onStatus("Subscribing; approve pairing if requested")
        bleOperation {
            if (!connection.setCharacteristicNotification(characteristic, true)) false
            else {
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                connection.writeDescriptor(descriptor)
            }
        }
    }

    private fun updateBattery(value: ByteArray) {
        val level = value.singleOrNull()?.toInt()?.and(0xff)
        listener.onBatteryLevel(level?.takeIf { it in 0..100 })
    }

    private fun routeNotification(connection: BluetoothGatt, characteristic: BluetoothGattCharacteristic,
                                  value: ByteArray) {
        val packet = value.copyOf()
        main.post {
            if (!running || gatt !== connection) return@post
            when (characteristic) {
                battery -> updateBattery(packet)
                tx -> handlePacket(packet)
            }
        }
    }

    /** All connection state and GATT callbacks run on the main looper. */
    fun start() {
        running = true
        scanAndConnect()
    }

    fun bluetoothStateChanged() {
        if (!running) return
        main.removeCallbacks(reconnect)
        stopScan()
        clearConnection()
        scanAndConnect()
    }

    @SuppressLint("MissingPermission")
    private fun scanAndConnect() {
        if (!running || gatt != null || activeScan != null) return
        if (!hasBluetoothPermission()) {
            listener.onStatus("Grant Nearby devices permission, then start relay again")
            return
        }
        if (runCatching { adapter?.isEnabled }.getOrNull() != true) {
            listener.onStatus("Waiting for Bluetooth to be enabled")
            return
        }
        val scan = object : ScanCallback() {
            override fun onScanResult(type: Int, result: ScanResult) {
                main.post {
                    if (!running || activeScan !== this || gatt != null) return@post
                    stopScan()
                    connect(result.device)
                }
            }

            override fun onScanFailed(errorCode: Int) {
                main.post {
                    if (activeScan === this) retry("BLE scan failed: $errorCode")
                }
            }
        }
        activeScan = scan
        try {
            // Non-empty filter permits discovery while the screen is off. The
            // firmware advertises its config UUID, not its voice service UUID.
            val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(CONFIG_SERVICE)).build()
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_POWER).build()
            val scanner = scanner ?: run { retry("Bluetooth scanner unavailable"); return }
            activeScanner = scanner
            scanner.startScan(listOf(filter), settings, scan)
            listener.onStatus("Waiting for Parrot nearby")
        } catch (_: SecurityException) {
            retry("Bluetooth permission unavailable")
        } catch (_: IllegalStateException) {
            retry("Bluetooth is not ready")
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        if (!running) return
        listener.onStatus("Connecting to Parrot")
        try {
            gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE,
                BluetoothDevice.PHY_LE_1M_MASK, main)
            if (gatt == null) retry("Could not connect to Parrot")
            else main.postDelayed(setupTimeout, 60_000)
        } catch (_: SecurityException) {
            retry("Bluetooth permission unavailable")
        }
    }

    private fun bleOperation(operation: () -> Boolean) {
        try {
            if (!operation()) retry("BLE operation could not start")
        } catch (_: SecurityException) {
            retry("Bluetooth permission unavailable")
        }
    }

    private fun retry(message: String) {
        stopScan()
        clearConnection()
        listener.onStatus(message)
        main.removeCallbacks(reconnect)
        if (running) {
            main.postDelayed(reconnect, retryDelay)
            retryDelay = (retryDelay * 2).coerceAtMost(30_000L)
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        val scan = activeScan ?: return
        activeScan = null
        val scanner = activeScanner
        activeScanner = null
        try { scanner?.stopScan(scan) } catch (_: SecurityException) {
        } catch (_: IllegalStateException) { }
    }

    @SuppressLint("MissingPermission")
    private fun clearConnection() {
        generation.incrementAndGet()
        main.removeCallbacks(setupTimeout)
        main.removeCallbacks(writeTimeout)
        subscribed = false
        voiceBusy = false
        writePending = false
        pendingPlay = null
        pcm.reset()
        val oldGatt = gatt
        gatt = null
        rx = null
        tx = null
        battery = null
        listener.onBatteryLevel(null)
        try { oldGatt?.disconnect() } catch (_: SecurityException) { }
        try { oldGatt?.close() } catch (_: SecurityException) { }
    }

    fun close() {
        running = false
        main.removeCallbacks(reconnect)
        stopScan()
        clearConnection()
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
            if (config.apiKey.isBlank()) {
                writeReply("error:apikey")
                listener.onStatus("Add a Gemini API key in Settings for AI replies")
                return@post
            }
            val timeout = Runnable {
                if (generation.compareAndSet(job, job + 1)) {
                    writeReply("error:timeout")
                }
            }
            // Firmware gives us 30 seconds. Leave time for the BLE write.
            main.postDelayed(timeout, 25_000)
            executeRequest {
                val response = try {
                    val audio = wav(captured.copyOf(samples * 2), rate)
                    keyFallback.classify(config.apiKey, config.paidApiKey,
                        isActive = { generation.get() == job && SystemClock.elapsedRealtime() < deadline },
                        onFallback = {
                            main.post {
                                if (generation.get() == job) {
                                    listener.onStatus("Free Gemini key failed; using paid key for one hour")
                                }
                            }
                        },
                        request = { key -> classifyRecording(audio, key) })
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
        main.postDelayed(writeTimeout, 10_000)
        listener.onStatus("Sending play request: $name")
        bleOperation { connection.writeCharacteristic(characteristic) }
    }

    @SuppressLint("MissingPermission")
    private fun writeReply(reply: String) {
        val characteristic = rx ?: return
        val connection = gatt ?: return
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        characteristic.value = reply.toByteArray(Charsets.UTF_8).let { it.copyOf(minOf(it.size, 63)) }
        writePending = true
        main.postDelayed(writeTimeout, 5_000)
        listener.onStatus("Reply sent: ${reply.substringBefore(':')}")
        bleOperation { connection.writeCharacteristic(characteristic) }
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
        private val CONFIG_SERVICE = UUID.fromString("6b520001-7c8e-4c30-9aa8-45e626d39b01")
        private val VOICE_SERVICE = UUID.fromString("6b520010-7c8e-4c30-9aa8-45e626d39b01")
        private val VOICE_TX = UUID.fromString("6b520011-7c8e-4c30-9aa8-45e626d39b01")
        private val VOICE_RX = UUID.fromString("6b520012-7c8e-4c30-9aa8-45e626d39b01")
        private val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private val BATTERY_SERVICE = UUID.fromString("0000180f-0000-1000-8000-00805f9b34fb")
        private val BATTERY_LEVEL = UUID.fromString("00002a19-0000-1000-8000-00805f9b34fb")
        private val INDEX_TABLE = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
        private val STEP_TABLE = intArrayOf(7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)
    }
}
