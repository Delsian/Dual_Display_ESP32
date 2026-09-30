package com.eugkrashtan.parrot

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

/** Owns BLE and AI work independently of the activity. */
class RelayService : Service(), VoiceRelay.Listener {
    private lateinit var relay: VoiceRelay
    private lateinit var keyStore: ApiKeyStore
    private lateinit var paidKeyStore: ApiKeyStore
    private lateinit var requestWakeLock: PowerManager.WakeLock
    private var foreground = false
    private var status = "Background relay stopped"
    private var observer: ((String) -> Unit)? = null
    private var batteryLevel: Int? = null
    private var firmwareVersion: String? = null
    private var firmwareObserver: ((String?) -> Unit)? = null
    private var otaObserver: ((String) -> Unit)? = null
    private var otaMessage = "No firmware update in progress"
    private var otaBusy = false
    private var otaLoadGeneration = 0
    private lateinit var otaWakeLock: PowerManager.WakeLock
    private var batteryObserver: ((Int?) -> Unit)? = null
    private val logHistory = android.text.SpannableStringBuilder()
    private var logObserver: ((CharSequence) -> Unit)? = null
    private var freeCalls = 0L
    private var paidCalls = 0L
    private var successfulCalls = 0L
    private var failedCalls = 0L
    private val logHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private val publishLogs = Runnable { logObserver?.invoke(android.text.SpannedString(logHistory)) }

    inner class LocalBinder : Binder() {
        fun aiCounters(): String = "AI calls: ${freeCalls + paidCalls} · Free: $freeCalls · Paid: $paidCalls\n" +
            "Succeeded: $successfulCalls · Failed: $failedCalls · Pending: ${freeCalls + paidCalls - successfulCalls - failedCalls}"
        fun observeLogs(listener: ((CharSequence) -> Unit)?) {
            logObserver = listener
            listener?.invoke(android.text.SpannedString(logHistory))
        }
        fun clearLogs() {
            logHistory.clear()
            logObserver?.invoke("")
        }
        fun observeBattery(listener: ((Int?) -> Unit)?) {
            batteryObserver = listener
            listener?.invoke(batteryLevel)
        }
        fun observeFirmware(listener: ((String?) -> Unit)?) {
            firmwareObserver = listener
            listener?.invoke(firmwareVersion)
        }
        fun observeOta(listener: ((String) -> Unit)?) {
            otaObserver = listener
            listener?.invoke(otaMessage)
        }
        fun updateFirmware(uri: android.net.Uri) {
            if (!foreground || otaBusy) { onOtaStatus(otaBusy, "Start relay and wait for any update to finish"); return }
            val job = ++otaLoadGeneration
            onOtaStatus(true, "Reading firmware file")
            kotlin.concurrent.thread(name = "parrot-ota-file") {
                val image = runCatching {
                    contentResolver.openInputStream(uri)?.use(OtaImage::read) ?: error("Cannot open firmware file")
                }
                logHandler.post {
                    if (job != otaLoadGeneration || !foreground) return@post
                    image.fold({ relay.updateFirmware(it) }, { onOtaStatus(false, "Invalid firmware: ${it.message}") })
                }
            }
        }
        fun cancelUpdate() {
            ++otaLoadGeneration
            val pending = relay.cancelFirmwareUpdate()
            onOtaStatus(pending, if (pending) "OTA: cancelling" else "OTA cancelled or no transfer active")
        }
        fun observe(listener: ((String) -> Unit)?) {
            observer = listener
            listener?.invoke(status)
        }
        fun play(clip: String) {
            if (foreground) relay.playStoredClip(clip)
            else onStatus("Start background relay first")
        }
        fun stop() = stopRelay()
        fun setSleeping(sleeping: Boolean) {
            if (foreground) relay.setSleeping(sleeping)
            else onStatus("Start background relay first")
        }
    }
    private val binder = LocalBinder()
    private val bluetoothState = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == BluetoothAdapter.ACTION_STATE_CHANGED && foreground) {
                relay.bluetoothStateChanged()
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        keyStore = ApiKeyStore(this)
        paidKeyStore = ApiKeyStore(this, paid = true)
        requestWakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Parrot:VoiceRequest").apply {
                setReferenceCounted(false)
            }
        relay = VoiceRelay(applicationContext, this)
        otaWakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Parrot:FirmwareUpdate").apply { setReferenceCounted(false) }
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Parrot background relay", NotificationManager.IMPORTANCE_LOW))
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(bluetoothState, filter, RECEIVER_EXPORTED)
        else registerReceiver(bluetoothState, filter)
    }

    override fun onBind(intent: Intent): IBinder = binder

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopRelay()
            return START_NOT_STICKY
        }
        if (intent?.action != ACTION_START && !isEnabled(this)) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (!hasBluetoothPermissions(this)) {
            stopRelay()
            onStatus("Grant Nearby devices permission, then start background relay")
            return START_NOT_STICKY
        }
        try {
            startForeground(NOTIFICATION_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } catch (_: RuntimeException) {
            stopRelay()
            onStatus("Open the app to start background relay")
            return START_NOT_STICKY
        }
        foreground = true
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(ENABLED, true).apply()
        relay.start()
        return START_STICKY
    }

    private fun stopRelay() {
        ++otaLoadGeneration
        onOtaStatus(false, "Firmware update stopped with relay")
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(ENABLED, false).apply()
        foreground = false
        relay.close()
        onStatus("Background relay stopped")
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun relayConfig(): VoiceRelay.Config = VoiceRelay.Config(
        try { keyStore.read() } catch (_: Exception) { "" },
        try { paidKeyStore.read() } catch (_: Exception) { "" },
    )

    override fun onStatus(message: String) {
        if (message != status) appendLog("[App] $message\n", android.graphics.Color.BLACK)
        status = message
        observer?.invoke(message)
        if (foreground) getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification())
    }

    override fun onRequestActive(active: Boolean) {
        if (active) requestWakeLock.acquire(40_000)
        else if (requestWakeLock.isHeld) requestWakeLock.release()
    }

    override fun onBatteryLevel(level: Int?) {
        batteryLevel = level
        batteryObserver?.invoke(level)
        if (foreground) getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification())
    }
    override fun onFirmwareVersion(version: String?) {
        firmwareVersion = version
        firmwareObserver?.invoke(version)
    }
    override fun onOtaStatus(active: Boolean, message: String) {
        otaBusy = active
        otaMessage = message
        if (active) otaWakeLock.acquire(30 * 60_000L)
        else if (otaWakeLock.isHeld) otaWakeLock.release()
        otaObserver?.invoke(message)
        onStatus(message)
    }

    override fun onDeviceLog(text: String) {
        appendLog(text, android.graphics.Color.rgb(0, 0, 139))
    }

    override fun onAiCallStarted(id: Long, paid: Boolean, audioMs: Int) {
        if (paid) paidCalls++ else freeCalls++
        appendLog("[App] AI #$id request: ${if (paid) "paid" else "free"} key, ${audioMs} ms audio\n",
            android.graphics.Color.BLACK)
    }

    override fun onAiCallFinished(id: Long, success: Boolean, outcome: String, elapsedMs: Long, stale: Boolean) {
        if (success) successfulCalls++ else failedCalls++
        appendLog("[App] AI #$id ${if (success) "succeeded" else "failed"}: $outcome; ${elapsedMs} ms" +
            (if (stale) " (late/cancelled; not delivered)" else "") + "\n", android.graphics.Color.BLACK)
    }

    private fun appendLog(text: String, color: Int) {
        val start = logHistory.length
        logHistory.append(text)
        logHistory.setSpan(android.text.style.ForegroundColorSpan(color), start, logHistory.length,
            android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        if (logHistory.length > 32768) {
            val cutoff = logHistory.indexOf("\n", logHistory.length - 32768)
            logHistory.delete(0, if (cutoff >= 0) cutoff + 1 else logHistory.length - 32768)
        }
        if (logObserver != null && !logHandler.hasCallbacks(publishLogs)) logHandler.postDelayed(publishLogs, 100)
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, RelayService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification_parrot)
            .setContentTitle("Parrot relay · ${batteryLevel?.let { "$it%" } ?: "—"}")
            .setContentText(status)
            .setStyle(Notification.BigTextStyle().bigText(status))
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .build()
    }

    override fun onDestroy() {
        foreground = false
        observer = null
        batteryObserver = null
        firmwareObserver = null
        otaObserver = null
        ++otaLoadGeneration
        if (otaWakeLock.isHeld) otaWakeLock.release()
        logObserver = null
        logHandler.removeCallbacks(publishLogs)
        relay.close()
        unregisterReceiver(bluetoothState)
        super.onDestroy()
    }

    companion object {
        const val ACTION_START = "com.eugkrashtan.parrot.START_RELAY"
        const val ACTION_STOP = "com.eugkrashtan.parrot.STOP_RELAY"
        private const val CHANNEL = "parrot_relay"
        private const val NOTIFICATION_ID = 1
        private const val PREFS = "relay_service"
        private const val ENABLED = "enabled"

        fun isEnabled(context: Context): Boolean =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(ENABLED, false)

        fun hasBluetoothPermissions(context: Context): Boolean =
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
                context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

        fun start(context: Context) {
            context.startForegroundService(Intent(context, RelayService::class.java).setAction(ACTION_START))
        }
    }
}
