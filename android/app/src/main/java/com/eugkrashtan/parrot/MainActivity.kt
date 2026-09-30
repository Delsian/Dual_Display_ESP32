package com.eugkrashtan.parrot

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.IBinder
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var battery: TextView
    private lateinit var firmware: TextView
    private lateinit var otaProgress: TextView
    private var relay: RelayService.LocalBinder? = null
    private var bound = false
    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, service: IBinder) {
            relay = service as RelayService.LocalBinder
            relay?.observe { onStatus(it) }
            relay?.observeBattery { onBatteryLevel(it) }
            relay?.observeFirmware { version -> runOnUiThread { firmware.text = "Firmware: ${version ?: "—"}" } }
            relay?.observeOta { message -> runOnUiThread { otaProgress.text = message } }
            relay?.observeLogs(logViewUpdater)
        }
        override fun onServiceDisconnected(name: ComponentName) {
            relay = null
            onBatteryLevel(null)
            firmware.text = "Firmware: —"
            onStatus("Relay restarting")
        }
    }
    private lateinit var keyStore: ApiKeyStore
    private var settingsDialog: AlertDialog? = null
    private var logDialog: AlertDialog? = null
    private var logViewUpdater: ((CharSequence) -> Unit)? = null
    private var config = VoiceRelay.Config("")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        keyStore = ApiKeyStore(this)
        val keyLoaded = try {
            config = VoiceRelay.Config(keyStore.read(), ApiKeyStore(this, paid = true).read())
            true
        } catch (_: Exception) { false }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        status = TextView(this).apply { text = "Disconnected" }
        battery = TextView(this).apply { text = "Parrot battery: —" }
        firmware = TextView(this).apply { text = "Firmware: —" }
        otaProgress = TextView(this).apply { text = "No firmware update in progress" }
        if (!keyLoaded) status.text = "Could not read saved API key; open Settings"
        val settings = Button(this).apply {
            text = "Settings"
            setOnClickListener { showSettings() }
        }
        val scan = Button(this).apply {
            text = "Start background relay"
            setOnClickListener { requestRelayStart() }
        }
        val disconnect = Button(this).apply {
            text = "Stop background relay"
            setOnClickListener { relay?.stop() }
        }
        val clip = EditText(this).apply {
            hint = "ESP32 clip: 001 or off_1"
            isSingleLine = true
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }
        val play = Button(this).apply {
            text = "Play"
            setOnClickListener {
                val enteredClip = clip.text.toString()
                val clipNumber = enteredClip.toIntOrNull()
                val clipName = if (clipNumber != null && clipNumber > 50) {
                    "off_${clipNumber - 50}"
                } else {
                    enteredClip
                }
                relay?.play(clipName) ?: showMessage("Wait for relay service")
            }
        }
        root.addView(TextView(this).apply { text = "Parrot Relay" })
        root.addView(status)
        root.addView(battery)
        root.addView(firmware)
        root.addView(settings, buttonParams())
        root.addView(scan, buttonParams())
        root.addView(disconnect, buttonParams())
        root.addView(Button(this).apply {
            text = "Sleep"
            setOnClickListener { relay?.setSleeping(true) ?: showMessage("Wait for relay service") }
        }, buttonParams())
        root.addView(Button(this).apply {
            text = "WakeUp"
            setOnClickListener { relay?.setSleeping(false) ?: showMessage("Wait for relay service") }
        }, buttonParams())
        root.addView(clip, buttonParams())
        root.addView(play, buttonParams())
        root.addView(Button(this).apply {
            text = "Update firmware"
            setOnClickListener {
                if (relay == null) showMessage("Wait for relay service")
                else startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                }, 2001)
            }
        }, buttonParams())
        root.addView(otaProgress)
        root.addView(Button(this).apply {
            text = "Cancel update"
            setOnClickListener { relay?.cancelUpdate() }
        }, buttonParams())
        root.addView(Button(this).apply {
            text = "Logs"
            setOnClickListener { showDeviceLogs() }
        }, buttonParams())
        setContentView(android.widget.ScrollView(this).apply { addView(root) })
    }

    override fun onStart() {
        super.onStart()
        bound = bindService(Intent(this, RelayService::class.java), connection, Context.BIND_AUTO_CREATE)
        if (RelayService.isEnabled(this) && RelayService.hasBluetoothPermissions(this)) startRelay()
    }

    override fun onStop() {
        relay?.observe(null)
        relay?.observeBattery(null)
        relay?.observeFirmware(null)
        relay?.observeOta(null)
        firmware.text = "Firmware: —"
        relay?.observeLogs(null)
        onBatteryLevel(null)
        relay = null
        if (bound) unbindService(connection)
        bound = false
        super.onStop()
    }

    private fun requestRelayStart() {
        val permissions = mutableListOf<String>()
        if (!RelayService.hasBluetoothPermissions(this)) {
            permissions += Manifest.permission.BLUETOOTH_SCAN
            permissions += Manifest.permission.BLUETOOTH_CONNECT
        }
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            permissions += Manifest.permission.POST_NOTIFICATIONS
        }
        if (permissions.isEmpty()) startRelay()
        else requestPermissions(permissions.toTypedArray(), 100)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 100) return
        if (RelayService.hasBluetoothPermissions(this)) startRelay()
        else showMessage("Nearby devices permission is required")
    }

    private fun startRelay() {
        try { RelayService.start(this) }
        catch (_: RuntimeException) { showMessage("Could not start background relay; try again") }
    }

    private fun showDeviceLogs() {
        if (logDialog?.isShowing == true) return
        val text = TextView(this).apply {
            setTextColor(android.graphics.Color.BLACK)
            setBackgroundColor(android.graphics.Color.WHITE)
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 12f
            setTextIsSelectable(true)
            setPadding(16, 8, 16, 8)
        }
        val scroll = android.widget.ScrollView(this).apply { addView(text) }
        val counters = TextView(this).apply { setTextColor(android.graphics.Color.BLACK) }
        val follow = android.widget.CheckBox(this).apply {
            this.text = "Follow new logs"
            setTextColor(android.graphics.Color.BLACK)
            isChecked = true
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(android.graphics.Color.WHITE)
            addView(counters)
            addView(follow)
            addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                (resources.displayMetrics.heightPixels * 0.5f).toInt()))
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("Logs — device: blue · app: black")
            .setView(content)
            .setNeutralButton("Clear", null)
            .setNegativeButton("Close", null)
            .create()
        logDialog = dialog
        logViewUpdater = { history ->
            runOnUiThread {
                text.text = history.ifEmpty { "No logs yet. Start the relay and connect to Parrot." }
                counters.text = relay?.aiCounters() ?: "AI counters unavailable; waiting for relay service"
                if (follow.isChecked) scroll.post { scroll.fullScroll(android.view.View.FOCUS_DOWN) }
            }
        }
        dialog.setOnDismissListener {
            relay?.observeLogs(null)
            logViewUpdater = null
            logDialog = null
        }
        dialog.show()
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
            relay?.clearLogs()
        }
        logViewUpdater?.invoke("")
        relay?.observeLogs(logViewUpdater)
    }

    private fun showSettings() {
        if (settingsDialog?.isShowing == true) return
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 16, 32, 16)
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("Settings")
            .setView(android.widget.ScrollView(this).apply { addView(content) })
            .setNegativeButton("Close", null)
            .create()
        settingsDialog = dialog
        content.addView(TextView(this).apply {
            text = "Free key is tried first. On error, the paid key is used for one hour, then free is retried."
        })
        fun addKeyControls(paid: Boolean) {
            val label = if (paid) "Paid" else "Free"
            val store = if (paid) ApiKeyStore(this, paid = true) else keyStore
            val saved = if (paid) config.paidApiKey else config.apiKey
            content.addView(TextView(this).apply { text = "$label Gemini API key" })
            val apiKey = EditText(this).apply {
                hint = if (paid) "Optional paid fallback key" else "Free Gemini API key"
                setText(saved)
                isSingleLine = true
                isSaveEnabled = false
                importantForAutofill = android.view.View.IMPORTANT_FOR_AUTOFILL_NO
                inputType = android.text.InputType.TYPE_CLASS_TEXT or
                    android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
            }
            val result = TextView(this).apply {
                text = if (saved.isEmpty()) "No $label key saved" else "$label key saved"
            }
            val add = Button(this).apply { text = "Save $label key" }
            val test = Button(this).apply { text = "Test $label key" }
            content.addView(apiKey)
            content.addView(TextView(this).apply {
                text = if (paid) "Save an empty value to disable paid fallback. Test uses the entered key without saving."
                    else "Save applies the key. Test checks the entered key without saving."
            })
            content.addView(add)
            content.addView(test)
            content.addView(result)
            add.setOnClickListener {
                val key = apiKey.text.toString().trim()
                if (key.isEmpty() && !paid) {
                    result.text = "Enter a Gemini API key first"
                } else {
                    try {
                        store.save(key)
                        config = if (paid) config.copy(paidApiKey = key) else config.copy(apiKey = key)
                        result.text = if (key.isEmpty()) "Paid fallback disabled" else "$label key saved"
                    } catch (_: Exception) {
                        result.text = "Could not save API key; try again"
                    }
                }
            }
            test.setOnClickListener {
                val key = apiKey.text.toString().trim()
                if (key.isEmpty()) {
                    result.text = "Enter a Gemini API key first"
                    return@setOnClickListener
                }
                apiKey.isEnabled = false
                add.isEnabled = false
                test.isEnabled = false
                result.text = "Testing…"
                thread(name = "parrot-key-test") {
                    val message = try {
                        val catalog = assets.open("intent_topics.json").bufferedReader().use { it.readText() }
                        GeminiIntent(catalog).testKey(key)
                    } catch (_: Exception) {
                        "Could not test API key; try again"
                    }
                    runOnUiThread {
                        if (!isDestroyed && dialog.isShowing) {
                            result.text = message
                            apiKey.isEnabled = true
                            add.isEnabled = true
                            test.isEnabled = true
                        }
                    }
                }
            }
        }
        addKeyControls(paid = false)
        addKeyControls(paid = true)
        dialog.show()
    }

    private fun buttonParams() = ViewGroup.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    private fun onStatus(message: String) {
        runOnUiThread { status.text = message }
    }

    private fun onBatteryLevel(level: Int?) {
        runOnUiThread { battery.text = level?.let { "Parrot battery: $it%" } ?: "Parrot battery: —" }
    }

    @Deprecated("Legacy Activity result API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 2001 || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try { contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        catch (_: SecurityException) { /* Temporary document access may still be valid. */ }
        AlertDialog.Builder(this).setTitle("Update Parrot firmware?")
            .setMessage("The selected firmware.bin will replace the device firmware and restart Parrot. Keep Bluetooth connected.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Update") { _, _ -> relay?.updateFirmware(uri) ?: showMessage("Wait for relay service") }
            .show()
    }

    private fun showMessage(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        onStatus(message)
    }

    override fun onDestroy() {
        settingsDialog?.dismiss()
        logDialog?.dismiss()
        super.onDestroy()
    }
}
