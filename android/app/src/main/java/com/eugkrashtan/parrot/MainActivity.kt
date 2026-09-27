package com.eugkrashtan.parrot

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

class MainActivity : Activity(), VoiceRelay.Listener {
    private lateinit var status: TextView
    private lateinit var relay: VoiceRelay
    private lateinit var endpoint: EditText
    private lateinit var token: EditText

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        relay = VoiceRelay(this, this)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        status = TextView(this).apply { text = "Disconnected" }
        endpoint = EditText(this).apply {
            hint = "Worker /intent URL"
            setText("https://parrot.eug-krashtan.workers.dev/intent")
            singleLine = true
        }
        token = EditText(this).apply {
            hint = "Device token"
            singleLine = true
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
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
        root.addView(TextView(this).apply { text = "Parrot Relay" })
        root.addView(status)
        root.addView(endpoint, fieldParams())
        root.addView(token, fieldParams())
        root.addView(scan, buttonParams())
        root.addView(disconnect, buttonParams())
        setContentView(root)
    }

    private fun fieldParams() = ViewGroup.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    private fun buttonParams() = ViewGroup.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    override fun relayConfig(): VoiceRelay.Config = VoiceRelay.Config(
        endpoint = endpoint.text.toString().trim(),
        token = token.text.toString(),
    )

    override fun onStatus(message: String) {
        runOnUiThread { status.text = message }
    }

    private fun showMessage(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        onStatus(message)
    }

    override fun onDestroy() {
        relay.close()
        super.onDestroy()
    }
}
