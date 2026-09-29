package com.eugkrashtan.parrot

/** One retry per recording; paid failures never extend the free-key cooldown. */
internal class GeminiKeyFallback(private val now: () -> Long) {
    private var paidUntil = 0L
    private var savedKeys: Pair<String, String>? = null

    fun classify(
        freeKey: String,
        paidKey: String,
        isActive: () -> Boolean,
        onFallback: () -> Unit,
        request: (String) -> String,
    ): String {
        val keys = freeKey to paidKey
        val usePaid = synchronized(this) {
            if (savedKeys != keys) {
                savedKeys = keys
                paidUntil = 0L
            }
            paidKey.isNotBlank() && now() < paidUntil
        }
        check(isActive())
        if (usePaid) return request(paidKey)
        try {
            return request(freeKey)
        } catch (error: Exception) {
            if (paidKey.isBlank() || paidKey == freeKey || !isActive()) throw error
            synchronized(this) {
                // A superseded request must not change a newer configuration's state.
                if (savedKeys == keys) paidUntil = now() + 60 * 60 * 1000L
            }
            onFallback()
            check(isActive())
            return request(paidKey)
        }
    }
}
