package com.eugkrashtan.parrot

import org.junit.Assert.*
import org.junit.Test

class ClipSelectionTest {
    @Test fun normalizesTopicNumbersAndPreservesNames() {
        assertEquals("001", ClipSelection.normalize(" 1 "))
        assertEquals("001", ClipSelection.normalize("001"))
        assertEquals("999", ClipSelection.normalize("999"))
        assertEquals("off_1", ClipSelection.normalize("off_1"))
        assertEquals("abcdefghijklmnop", ClipSelection.normalize("abcdefghijklmnop"))
    }

    @Test fun rejectsPathsExtensionsInvalidCharactersAndOutOfRangeIds() {
        for (name in listOf("", " ", "0", "1000", "9999999999999999", "../001", "001.wav",
            "off_1/", "OFF_1", "off 1", "abcdefghijklmnopq", "off_1\u0000", "-1")) {
            assertNull(name, ClipSelection.normalize(name))
        }
    }
}
