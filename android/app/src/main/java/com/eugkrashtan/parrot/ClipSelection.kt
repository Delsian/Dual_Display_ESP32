package com.eugkrashtan.parrot

internal object ClipSelection {
    fun normalize(input: String): String? {
        val name = input.trim()
        if (!name.matches(Regex("[a-z0-9_]{1,16}"))) return null
        if (name.all { it in '0'..'9' }) {
            val id = name.toIntOrNull() ?: return null
            return if (id in 1..999) id.toString().padStart(3, '0') else null
        }
        return name
    }
}
