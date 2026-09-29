package com.eugkrashtan.parrot

import android.Manifest
import android.app.Application
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import java.util.UUID
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanResult
import android.content.Intent
import android.os.Looper
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.LooperMode
import java.time.Duration

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
@LooperMode(LooperMode.Mode.PAUSED)
class RelayLifecycleTest {
    private lateinit var app: Application
    private lateinit var adapter: BluetoothAdapter
    private val loop get() = shadowOf(Looper.getMainLooper())
    private val services = mutableListOf<org.robolectric.android.controller.ServiceController<RelayService>>()
    private val relays = mutableListOf<VoiceRelay>()

    @Before fun setup() {
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.POST_NOTIFICATIONS)
        adapter = BluetoothAdapter.getDefaultAdapter()
        shadowOf(adapter).setState(BluetoothAdapter.STATE_ON)
    }

    @After fun cleanup() {
        relays.forEach { it.close() }
        services.forEach { it.destroy() }
    }

    private fun service(): RelayService {
        val controller = Robolectric.buildService(RelayService::class.java).create()
        services += controller
        return controller.get()
    }

    private fun relay(): VoiceRelay = VoiceRelay(app, object : VoiceRelay.Listener {
        override fun relayConfig() = VoiceRelay.Config("")
        override fun onStatus(message: String) {}
    }).also { relays += it }

    @Test fun startedServiceSurvivesUiUnbindingAndStopDisablesRecovery() {
        val service = service()
        assertEquals(Service.START_STICKY, service.onStartCommand(Intent().setAction(RelayService.ACTION_START), 0, 1))
        assertTrue(RelayService.isEnabled(app))
        assertNotNull(shadowOf(service).lastForegroundNotification)
        val binder = service.onBind(Intent()) as RelayService.LocalBinder
        binder.observe { }
        binder.observe(null)
        service.onUnbind(Intent())
        assertEquals(1, shadowOf(adapter.bluetoothLeScanner).scanCallbacks.size)
        service.onStartCommand(Intent().setAction(RelayService.ACTION_START), 0, 2)
        assertEquals(1, shadowOf(adapter.bluetoothLeScanner).scanCallbacks.size)
        binder.stop()
        loop.idleFor(Duration.ofMinutes(2))
        assertFalse(RelayService.isEnabled(app))
        assertTrue(shadowOf(adapter.bluetoothLeScanner).scanCallbacks.isEmpty())
    }

    @Test fun stickyRestartOnlyRunsWhenPreviouslyEnabled() {
        val service = service()
        assertEquals(Service.START_NOT_STICKY, service.onStartCommand(null, 0, 1))
        service.onStartCommand(Intent().setAction(RelayService.ACTION_START), 0, 2)
        assertEquals(Service.START_STICKY, service.onStartCommand(null, 0, 3))
    }

    @Test fun bootOnlyStartsEnabledServiceWithPermissions() {
        val receiver = RelayBootReceiver()
        receiver.onReceive(app, Intent(Intent.ACTION_BOOT_COMPLETED))
        assertNull(shadowOf(app).nextStartedService)
        val service = service()
        service.onStartCommand(Intent().setAction(RelayService.ACTION_START), 0, 1)
        receiver.onReceive(app, Intent(Intent.ACTION_BOOT_COMPLETED))
        assertEquals(RelayService.ACTION_START, shadowOf(app).nextStartedService.action)
        shadowOf(app).denyPermissions(Manifest.permission.BLUETOOTH_CONNECT)
        receiver.onReceive(app, Intent(Intent.ACTION_BOOT_COMPLETED))
        assertNull(shadowOf(app).nextStartedService)
    }

    @Test fun serviceReadsUpdatedSavedKeyWithoutActivity() {
        val service = service()
        ApiKeyStore(app).save("first-test-key")
        assertEquals("first-test-key", service.relayConfig().apiKey)
        ApiKeyStore(app).save("second-test-key")
        assertEquals("second-test-key", service.relayConfig().apiKey)
        assertEquals("", service.relayConfig().paidApiKey)
        ApiKeyStore(app, paid = true).save("paid-test-key")
        assertEquals("paid-test-key", service.relayConfig().paidApiKey)
        assertEquals("second-test-key", service.relayConfig().apiKey)
        ApiKeyStore(app, paid = true).save("")
        assertEquals("", service.relayConfig().paidApiKey)
    }

    @Test fun stopCancelsPendingScanRetry() {
        val relay = relay()
        relay.start()
        val scanner = shadowOf(adapter.bluetoothLeScanner)
        val callback = scanner.scanCallbacks.single()
        callback.onScanFailed(3)
        loop.idle()
        relay.close()
        loop.idleFor(Duration.ofMinutes(2))
        assertTrue(scanner.scanCallbacks.isEmpty())
    }

    @Test fun scanFailureRetriesWithoutDuplicateScans() {
        val relay = relay()
        relay.start()
        relay.start()
        val scanner = shadowOf(adapter.bluetoothLeScanner)
        assertEquals(1, scanner.scanCallbacks.size)
        scanner.scanCallbacks.single().onScanFailed(3)
        loop.idle()
        assertTrue(scanner.scanCallbacks.isEmpty())
        loop.idleFor(Duration.ofSeconds(5))
        assertEquals(1, scanner.scanCallbacks.size)
    }

    @Test fun disconnectRetriesAndStaleCallbacksCannotCloseNewScan() {
        val relay = relay()
        relay.start()
        val scanner = shadowOf(adapter.bluetoothLeScanner)
        val device = adapter.getRemoteDevice("01:02:03:04:05:06")
        scanner.scanCallbacks.single().onScanResult(0, ScanResult(device, null, -45, 0))
        loop.idle()
        assertTrue(scanner.scanCallbacks.isEmpty())
        val gatt = shadowOf(device).bluetoothGatts.single()
        val callback = shadowOf(gatt).gattCallback
        callback.onConnectionStateChange(gatt, BluetoothGatt.GATT_SUCCESS, BluetoothProfile.STATE_DISCONNECTED)
        loop.idleFor(Duration.ofSeconds(5))
        assertTrue(shadowOf(gatt).isClosed)
        assertEquals(1, scanner.scanCallbacks.size)
        callback.onConnectionStateChange(gatt, BluetoothGatt.GATT_SUCCESS, BluetoothProfile.STATE_CONNECTED)
        assertEquals(1, scanner.scanCallbacks.size)
    }

    @Test fun stalledConnectionTimesOutAndBluetoothCanResumeScanning() {
        val relay = relay()
        relay.start()
        val scanner = shadowOf(adapter.bluetoothLeScanner)
        val device = adapter.getRemoteDevice("01:02:03:04:05:06")
        scanner.scanCallbacks.single().onScanResult(0, ScanResult(device, null, -45, 0))
        loop.idle()
        loop.idleFor(Duration.ofSeconds(65))
        assertTrue(shadowOf(shadowOf(device).bluetoothGatts.single()).isClosed)
        assertEquals(1, scanner.scanCallbacks.size)
        shadowOf(adapter).setState(BluetoothAdapter.STATE_OFF)
        relay.bluetoothStateChanged()
        assertTrue(scanner.scanCallbacks.isEmpty())
        shadowOf(adapter).setState(BluetoothAdapter.STATE_ON)
        relay.bluetoothStateChanged()
        assertEquals(1, scanner.scanCallbacks.size)
    }
    private fun ready(relay: VoiceRelay, battery: BluetoothGattCharacteristic? = null,
                      logs: BluetoothGattCharacteristic? = null): Pair<BluetoothGatt, BluetoothGattCharacteristic> {
        relay.start()
        val device = adapter.getRemoteDevice("01:02:03:04:05:06")
        shadowOf(adapter.bluetoothLeScanner).scanCallbacks.single()
            .onScanResult(0, ScanResult(device, null, -40, 0))
        loop.idle()
        val gatt = shadowOf(device).bluetoothGatts.last()
        val service = BluetoothGattService(UUID.fromString("6b520010-7c8e-4c30-9aa8-45e626d39b01"), 0)
        val tx = BluetoothGattCharacteristic(UUID.fromString("6b520011-7c8e-4c30-9aa8-45e626d39b01"),
            BluetoothGattCharacteristic.PROPERTY_NOTIFY, 0)
        val descriptor = BluetoothGattDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"),
            BluetoothGattDescriptor.PERMISSION_WRITE)
        tx.addDescriptor(descriptor)
        service.addCharacteristic(tx)
        service.addCharacteristic(BluetoothGattCharacteristic(UUID.fromString("6b520012-7c8e-4c30-9aa8-45e626d39b01"),
            BluetoothGattCharacteristic.PROPERTY_WRITE, BluetoothGattCharacteristic.PERMISSION_WRITE))
        shadowOf(gatt).addDiscoverableService(service)
        shadowOf(gatt).allowCharacteristicNotification(tx)
        if (battery != null) {
            val batteryService = BluetoothGattService(UUID.fromString("0000180f-0000-1000-8000-00805f9b34fb"), 0)
            batteryService.addCharacteristic(battery)
            shadowOf(gatt).addDiscoverableService(batteryService)
            shadowOf(gatt).allowCharacteristicNotification(battery)
        }
        if (logs != null) {
            val logService = BluetoothGattService(UUID.fromString("6b520020-7c8e-4c30-9aa8-45e626d39b01"), 0)
            logService.addCharacteristic(logs)
            shadowOf(gatt).addDiscoverableService(logService)
            shadowOf(gatt).allowCharacteristicNotification(logs)
        }
        shadowOf(gatt).gattCallback.onConnectionStateChange(gatt, BluetoothGatt.GATT_SUCCESS, BluetoothProfile.STATE_CONNECTED)
        // Complete platform callbacks explicitly; this is a simulated peripheral.
        if (battery != null) {
            shadowOf(gatt).gattCallback.onCharacteristicRead(gatt, battery, byteArrayOf(73), BluetoothGatt.GATT_SUCCESS)
            shadowOf(gatt).gattCallback.onDescriptorWrite(gatt, battery.descriptors.single(), BluetoothGatt.GATT_SUCCESS)
        }
        if (logs != null) {
            shadowOf(gatt).gattCallback.onDescriptorWrite(gatt, logs.descriptors.single(), BluetoothGatt.GATT_SUCCESS)
        }
        shadowOf(gatt).gattCallback.onDescriptorWrite(gatt, descriptor, BluetoothGatt.GATT_SUCCESS)
        return gatt to tx
    }

    private fun recording(gatt: BluetoothGatt, tx: BluetoothGattCharacteristic) {
        val callback = shadowOf(gatt).gattCallback
        callback.onCharacteristicChanged(gatt, tx, byteArrayOf(1, 0x80.toByte(), 0x3e))
        repeat(13) { sequence ->
            val packet = ByteArray(165)
            packet[0] = 2
            packet[1] = sequence.toByte()
            callback.onCharacteristicChanged(gatt, tx, packet)
        }
        // 13 packets x 320 samples = 4160 samples, little endian.
        callback.onCharacteristicChanged(gatt, tx, byteArrayOf(3, 0x40, 0x10, 0, 0))
        loop.idle()
    }

    @Test fun batteryReadUpdatesAndDisconnectClearsWithoutConsumingVoicePackets() {
        val levels = mutableListOf<Int?>()
        val relay = VoiceRelay(app, object : VoiceRelay.Listener {
            override fun relayConfig() = VoiceRelay.Config("")
            override fun onStatus(message: String) {}
            override fun onBatteryLevel(level: Int?) { levels += level }
        }).also { relays += it }
        val battery = BluetoothGattCharacteristic(UUID.fromString("00002a19-0000-1000-8000-00805f9b34fb"),
            BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
            BluetoothGattCharacteristic.PERMISSION_READ)
        battery.addDescriptor(BluetoothGattDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"),
            BluetoothGattDescriptor.PERMISSION_WRITE))
        val (gatt, tx) = ready(relay, battery)
        assertEquals(73, levels.last())
        val callback = shadowOf(gatt).gattCallback
        for (level in listOf(0, 1, 3, 100)) {
            callback.onCharacteristicChanged(gatt, battery, byteArrayOf(level.toByte()))
            loop.idle()
            assertEquals(level, levels.last())
        }
        // Legacy callback used by Android 12.
        battery.value = byteArrayOf(42)
        callback.onCharacteristicChanged(gatt, battery)
        loop.idle()
        assertEquals(42, levels.last())
        for (bad in listOf(byteArrayOf(), byteArrayOf(101), byteArrayOf(-1), byteArrayOf(1, 2))) {
            callback.onCharacteristicChanged(gatt, battery, bad)
            loop.idle()
            assertNull(levels.last())
        }
        callback.onCharacteristicChanged(gatt, battery, byteArrayOf(50))
        loop.idle()
        // Voice traffic still follows its own route.
        recording(gatt, tx)
        assertEquals("error:apikey", String(shadowOf(gatt).latestWrittenBytes))
        assertEquals(50, levels.last())
        relay.close()
        assertNull(levels.last())
        callback.onCharacteristicChanged(gatt, battery, byteArrayOf(99))
        loop.idle()
        assertNull(levels.last())
    }

    @Test fun batteryObserverReceivesCachedValueAfterRebinding() {
        val service = service()
        val binder = service.onBind(Intent()) as RelayService.LocalBinder
        var level: Int? = -1
        binder.observeBattery { level = it }
        assertNull(level)
        service.onBatteryLevel(65)
        assertEquals(65, level)
        binder.observeBattery(null)
        service.onBatteryLevel(64)
        assertEquals(65, level)
        binder.observeBattery { level = it }
        assertEquals(64, level)
        binder.stop()
        assertNull(level)
    }

    @Test fun notificationShowsBatteryUpdatesAndClearsDisconnectedValue() {
        val service = service()
        service.onStartCommand(Intent().setAction(RelayService.ACTION_START), 0, 1)
        val initial = shadowOf(service).lastForegroundNotification!!
        assertEquals(R.drawable.ic_notification_parrot, initial.smallIcon.resId)
        assertTrue(initial.extras.getCharSequence(android.app.Notification.EXTRA_TITLE).toString().contains("—"))
        val manager = shadowOf(app.getSystemService(android.app.NotificationManager::class.java))
        service.onStatus("Ready")
        for (level in listOf(73, 0, 100, null)) {
            service.onBatteryLevel(level)
            val notification = manager.allNotifications.single()
            assertEquals("Parrot relay · Battery: ${level?.let { "$it%" } ?: "—"}",
                notification.extras.getCharSequence(android.app.Notification.EXTRA_TITLE).toString())
            assertEquals("Ready", notification.extras.getCharSequence(android.app.Notification.EXTRA_TEXT).toString())
            assertEquals(R.drawable.ic_notification_parrot, notification.smallIcon.resId)
        }
    }

    @Test fun deviceLogNotificationsAreDecodedAndOldConnectionsIgnored() {
        val output = StringBuilder()
        val relay = VoiceRelay(app, object : VoiceRelay.Listener {
            override fun relayConfig() = VoiceRelay.Config("")
            override fun onStatus(message: String) {}
            override fun onDeviceLog(text: String) { output.append(text) }
        }).also { relays += it }
        val logs = BluetoothGattCharacteristic(UUID.fromString("6b520021-7c8e-4c30-9aa8-45e626d39b01"),
            BluetoothGattCharacteristic.PROPERTY_NOTIFY, 0)
        logs.addDescriptor(BluetoothGattDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"),
            BluetoothGattDescriptor.PERMISSION_WRITE))
        val (gatt, tx) = ready(relay, logs = logs)
        assertTrue(output.contains("stream connected"))
        val callback = shadowOf(gatt).gattCallback
        callback.onCharacteristicChanged(gatt, logs, byteArrayOf(0, 0, 0, 0) + "Audio: ".toByteArray())
        logs.value = byteArrayOf(1, 0, 0, 0) + "ready\n".toByteArray()
        callback.onCharacteristicChanged(gatt, logs)
        loop.idle()
        assertTrue(output.toString().endsWith("Audio: ready\n"))
        recording(gatt, tx)
        assertEquals("error:apikey", String(shadowOf(gatt).latestWrittenBytes))
        relay.close()
        val before = output.toString()
        callback.onCharacteristicChanged(gatt, logs, byteArrayOf(2, 0, 0, 0) + "stale\n".toByteArray())
        loop.idle()
        assertEquals(before, output.toString())
        assertTrue(before.contains("stream disconnected"))
    }

    @Test fun logHistoryIsBoundedRetainedAcrossRebindingAndClearable() {
        val service = service()
        val binder = service.onBind(Intent()) as RelayService.LocalBinder
        var text = ""
        binder.observeLogs { text = it.toString() }
        repeat(3000) { service.onDeviceLog("Audio: ready\n") }
        loop.idleFor(Duration.ofMillis(100))
        assertTrue(text.length <= 32768)
        assertTrue(text.endsWith("Audio: ready\n"))
        binder.observeLogs(null)
        service.onDeviceLog("new\n")
        binder.observeLogs { text = it.toString() }
        assertTrue(text.endsWith("new\n"))
        binder.clearLogs()
        assertEquals("", text)
        loop.idleFor(Duration.ofMillis(100))
        assertEquals("", text)
    }

    @Test fun commonLogsKeepSourceColorsAndCallCountersAcrossClear() {
        val service = service()
        val binder = service.onBind(Intent()) as RelayService.LocalBinder
        var snapshot: CharSequence = ""
        binder.observeLogs { snapshot = it }
        service.onDeviceLog("Device line\n")
        service.onStatus("App connection ready")
        service.onAiCallStarted(1, false, 2500)
        service.onAiCallFinished(1, false, "HTTP 429", 300, false)
        service.onAiCallStarted(2, true, 2500)
        loop.idleFor(Duration.ofMillis(100))
        val spans = snapshot as android.text.Spanned
        fun colorAt(text: String): Int {
            val offset = snapshot.indexOf(text)
            assertTrue(offset >= 0)
            return spans.getSpans(offset, offset + 1, android.text.style.ForegroundColorSpan::class.java)
                .single().foregroundColor
        }
        assertEquals(android.graphics.Color.rgb(0, 0, 139), colorAt("Device line"))
        assertEquals(android.graphics.Color.BLACK, colorAt("App connection ready"))
        assertEquals(android.graphics.Color.BLACK, colorAt("AI #1"))
        assertTrue(binder.aiCounters().contains("AI calls: 2 · Free: 1 · Paid: 1"))
        assertTrue(binder.aiCounters().contains("Succeeded: 0 · Failed: 1 · Pending: 1"))
        service.onAiCallFinished(2, true, "clip:001", 500, true)
        loop.idleFor(Duration.ofMillis(100))
        assertTrue(snapshot.contains("late/cancelled; not delivered"))
        assertTrue(binder.aiCounters().contains("Succeeded: 1 · Failed: 1 · Pending: 0"))
        binder.clearLogs()
        assertEquals("", snapshot.toString())
        assertTrue(binder.aiCounters().contains("AI calls: 2"))
    }

    @Test fun recordingReachesAiAndRepliesWithoutAnActivity() {
        var calls = 0
        val relay = VoiceRelay(app, object : VoiceRelay.Listener {
            override fun relayConfig() = VoiceRelay.Config("saved-test-key")
            override fun onStatus(message: String) {}
        }, { wav, key ->
            calls++
            assertEquals("saved-test-key", key)
            assertEquals(44 + 4160 * 2, wav.size)
            assertEquals("RIFF", String(wav.copyOfRange(0, 4)))
            "clip:001"
        }, { it() }).also { relays += it }
        val (gatt, tx) = ready(relay)
        recording(gatt, tx)
        assertEquals(1, calls)
        assertEquals("clip:001", String(shadowOf(gatt).latestWrittenBytes))
    }

    @Test fun freeFailureRetriesSameAudioWithPaidKey() {
        val calls = mutableListOf<String>()
        val events = mutableListOf<String>()
        var firstAudio: ByteArray? = null
        val relay = VoiceRelay(app, object : VoiceRelay.Listener {
            override fun relayConfig() = VoiceRelay.Config("free-test-key", "paid-test-key")
            override fun onStatus(message: String) {}
            override fun onAiCallStarted(id: Long, paid: Boolean, audioMs: Int) {
                events += "start:$id:$paid"
                assertEquals(260, audioMs)
            }
            override fun onAiCallFinished(id: Long, success: Boolean, outcome: String, elapsedMs: Long, stale: Boolean) {
                events += "finish:$id:$success"
                assertFalse(stale)
            }
        }, { audio, key ->
            calls += key
            if (key == "free-test-key") {
                firstAudio = audio
                throw java.io.IOException()
            }
            assertArrayEquals(firstAudio, audio)
            "clip:002"
        }, { it() }).also { relays += it }
        val (gatt, tx) = ready(relay)
        recording(gatt, tx)
        assertEquals(listOf("free-test-key", "paid-test-key"), calls)
        assertEquals("clip:002", String(shadowOf(gatt).latestWrittenBytes))
        assertEquals(listOf("start:1:false", "finish:1:false", "start:2:true", "finish:2:true"), events)
    }

    @Test fun stoppingDropsPendingAiReply() {
        var work: (() -> Unit)? = null
        val relay = VoiceRelay(app, object : VoiceRelay.Listener {
            override fun relayConfig() = VoiceRelay.Config("test-key")
            override fun onStatus(message: String) {}
        }, { _, _ -> "clip:001" }, { work = it }).also { relays += it }
        val (gatt, tx) = ready(relay)
        recording(gatt, tx)
        assertNotNull(work)
        val before = shadowOf(gatt).latestWrittenBytes
        relay.close()
        work!!.invoke()
        loop.idle()
        assertArrayEquals(before, shadowOf(gatt).latestWrittenBytes)
        assertTrue(shadowOf(gatt).isClosed)
    }

    @Test fun missingKeyReturnsAnErrorWithoutCallingAi() {
        val relay = VoiceRelay(app, object : VoiceRelay.Listener {
            override fun relayConfig() = VoiceRelay.Config("")
            override fun onStatus(message: String) {}
        }, { _, _ -> error("Must not contact Gemini") }, { it() }).also { relays += it }
        val (gatt, tx) = ready(relay)
        recording(gatt, tx)
        assertEquals("error:apikey", String(shadowOf(gatt).latestWrittenBytes))
    }

}
