package com.eugkrashtan.parrot

import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class GeminiKeyFallbackTest {
    private var now = 10L
    private val fallback = GeminiKeyFallback { now }
    private val calls = mutableListOf<String>()
    private var active = true
    private var switches = 0
    private var freeFails = true
    private var paidFails = false

    private fun classify(free: String = "free", paid: String = "paid"): String =
        fallback.classify(free, paid, { active }, { switches++ }) { key ->
            calls += key
            if ((key == free && freeFails) || (key == paid && paidFails)) throw IOException()
            "clip:001"
        }

    @Test fun successUsesOnlyFreeKey() {
        freeFails = false
        assertEquals("clip:001", classify())
        assertEquals(listOf("free"), calls)
        assertEquals(0, switches)
    }

    @Test fun retriesThenUsesPaidUntilExactlyOneHour() {
        assertEquals("clip:001", classify())
        now += 3_599_999
        classify()
        now++
        freeFails = false
        classify()
        assertEquals(listOf("free", "paid", "paid", "free"), calls)
        assertEquals(1, switches)
    }

    @Test fun failedHourlyProbeStartsAnotherHour() {
        classify()
        now += 3_600_000
        classify()
        classify()
        assertEquals(listOf("free", "paid", "free", "paid", "paid"), calls)
    }

    @Test fun paidFailureDoesNotLoopOrExtendCooldown() {
        paidFails = true
        assertThrows(IOException::class.java) { classify() }
        now += 3_599_999
        assertThrows(IOException::class.java) { classify() }
        now++
        freeFails = false
        classify()
        assertEquals(listOf("free", "paid", "paid", "free"), calls)
    }

    @Test fun missingOrDuplicatePaidKeyDoesNotRetry() {
        assertThrows(IOException::class.java) { classify(paid = "") }
        assertThrows(IOException::class.java) { classify(paid = "free") }
        assertEquals(listOf("free", "free"), calls)
    }

    @Test fun changedKeysResetToFree() {
        classify()
        classify(free = "new-free")
        assertEquals(listOf("free", "paid", "new-free", "paid"), calls)
    }

    @Test fun cancellationOrDeadlinePreventsPaidRetry() {
        assertThrows(IOException::class.java) {
            fallback.classify("free", "paid", { active }, { switches++ }) { key ->
                calls += key
                active = false
                throw IOException()
            }
        }
        assertEquals(listOf("free"), calls)
        assertEquals(0, switches)
        active = true
        freeFails = false
        classify()
        assertEquals(listOf("free", "free"), calls)
    }

    @Test fun alreadyCancelledWorkDoesNotCallProvider() {
        active = false
        assertThrows(IllegalStateException::class.java) { classify() }
        assertTrue(calls.isEmpty())
    }
}
