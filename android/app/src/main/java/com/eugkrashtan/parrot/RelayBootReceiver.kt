package com.eugkrashtan.parrot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

class RelayBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        if (!RelayService.isEnabled(context) || !RelayService.hasBluetoothPermissions(context)) return
        try {
            RelayService.start(context)
        } catch (_: RuntimeException) {
            // Background launch can be restricted by the system/vendor. Opening
            // the activity retries while visible; never schedule restart loops.
        }
    }
}
