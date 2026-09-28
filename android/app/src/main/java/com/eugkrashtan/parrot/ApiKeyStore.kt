package com.eugkrashtan.parrot

import android.content.Context
import android.util.AtomicFile
import java.io.File

/** App-private internal storage, excluded from backup and device transfer. */
internal class ApiKeyStore(context: Context) {
    private val file = AtomicFile(File(context.noBackupFilesDir, "gemini_api_key"))

    fun read(): String = if (file.baseFile.exists()) file.readFully().toString(Charsets.UTF_8) else ""

    fun save(apiKey: String) {
        require(apiKey.isNotBlank())
        val output = file.startWrite()
        try {
            output.write(apiKey.toByteArray(Charsets.UTF_8))
            file.finishWrite(output)
        } catch (error: Exception) {
            file.failWrite(output)
            throw error
        }
    }
}
