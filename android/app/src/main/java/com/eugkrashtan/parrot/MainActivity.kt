package com.eugkrashtan.parrot

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import kotlin.concurrent.thread

class MainActivity : Activity(), VoiceRelay.Listener {
    private lateinit var status: TextView
    private lateinit var relay: VoiceRelay
    private lateinit var keyStore: ApiKeyStore
    private var settingsDialog: AlertDialog? = null
    private var config = VoiceRelay.Config("")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        relay = VoiceRelay(this, this)
        keyStore = ApiKeyStore(this)
        val keyLoaded = try {
            config = VoiceRelay.Config(keyStore.read())
            true
        } catch (_: Exception) { false }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        status = TextView(this).apply { text = "Disconnected" }
        if (!keyLoaded) status.text = "Could not read saved API key; open Settings"
        val settings = Button(this).apply {
            text = "Settings"
            setOnClickListener { showSettings() }
        }
        val scan = Button(this).apply {
            text = "Scan and connect"
            setOnClickListener {
                if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED ||
                    checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT), 100)
                } else if (BluetoothAdapter.getDefaultAdapter()?.isEnabled != true) {
                    showMessage("Enable Bluetooth first")
                } else {
                    relay.scanAndConnect()
                }
            }
        }
        val disconnect = Button(this).apply {
            text = "Disconnect"
            setOnClickListener { relay.close() }
        }
        val clip = EditText(this).apply {
            hint = "ESP32 clip: 001 or off_1"
            isSingleLine = true
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }
        val play = Button(this).apply {
            text = "Play"
            setOnClickListener { relay.playStoredClip(clip.text.toString()) }
        }
        root.addView(TextView(this).apply { text = "Parrot Relay" })
        root.addView(status)
        root.addView(settings, buttonParams())
        root.addView(scan, buttonParams())
        root.addView(disconnect, buttonParams())
        root.addView(clip, buttonParams())
        root.addView(play, buttonParams())
        setContentView(root)
    }

    private fun showSettings() {
        if (settingsDialog?.isShowing == true) return
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 16, 32, 16)
        }
        val apiKey = EditText(this).apply {
            hint = "Gemini API key"
            setText(config.apiKey)
            isSingleLine = true
            isSaveEnabled = false
            importantForAutofill = android.view.View.IMPORTANT_FOR_AUTOFILL_NO
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        val result = TextView(this).apply {
            text = if (config.apiKey.isEmpty()) "No API key saved" else "API key saved"
        }
        val add = Button(this).apply { text = "Add" }
        val test = Button(this).apply { text = "Test" }
        content.addView(apiKey)
        content.addView(TextView(this).apply { text = "Add saves the key. Test checks the entered key with Gemini." })
        content.addView(add)
        content.addView(test)
        content.addView(result)
        val dialog = AlertDialog.Builder(this)
            .setTitle("Settings")
            .setView(content)
            .setNegativeButton("Close", null)
            .create()
        settingsDialog = dialog
        add.setOnClickListener {
            val key = apiKey.text.toString().trim()
            if (key.isEmpty()) {
                result.text = "Enter a Gemini API key first"
            } else {
                try {
                    keyStore.save(key)
                    config = VoiceRelay.Config(key)
                    result.text = "API key saved"
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
        dialog.show()
    }

    private fun buttonParams() = ViewGroup.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    override fun relayConfig(): VoiceRelay.Config = config

    override fun onStatus(message: String) {
        runOnUiThread { status.text = message }
    }

    private fun showMessage(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        onStatus(message)
    }

    override fun onDestroy() {
        settingsDialog?.dismiss()
        relay.close()
        super.onDestroy()
    }
}
