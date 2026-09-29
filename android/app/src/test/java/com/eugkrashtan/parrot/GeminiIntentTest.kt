package com.eugkrashtan.parrot

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.Base64

class GeminiIntentTest {
    private val classifier = GeminiIntent(File("src/main/assets/intent_topics.json").readText())
    private fun response(label: String, reason: String = "STOP") = JSONObject("""
        {"candidates":[{"finishReason":"$reason","content":{"parts":[
          {"text":"private reasoning","thought":true},{"text":"$label"}]}}]}
    """)

    @Test fun requestContainsAudioAndConstrainedTopics() {
        val wav = byteArrayOf(0, 1, -1, 42)
        val body = classifier.request(wav)
        val audio = body.getJSONArray("contents").getJSONObject(0).getJSONArray("parts")
            .getJSONObject(1).getJSONObject("inlineData")
        assertEquals("audio/wav", audio.getString("mimeType"))
        assertArrayEquals(wav, Base64.getDecoder().decode(audio.getString("data")))
        val config = body.getJSONObject("generationConfig")
        assertEquals("text/x.enum", config.getString("responseMimeType"))
        val choices = config.getJSONObject("responseSchema").getJSONArray("enum")
        assertEquals(52, choices.length())
        assertEquals("ignore", choices.getString(51))
        assertTrue(body.getJSONObject("systemInstruction").toString().contains("Rain"))
    }

    @Test fun everyTopicMapsToItsClip() {
        for (id in 1..50) assertEquals("clip:${id.toString().padStart(3, '0')}", classifier.reply(response(" $id ")))
    }

    @Test fun fallbackStaysInCatalogRange() {
        repeat(100) { assertTrue(classifier.reply(response("offtopic")).matches(Regex("clip:off_([1-9]|10)"))) }
    }

    @Test fun noiseIsIgnored() {
        assertEquals("ignore", classifier.reply(response("ignore")))
    }

    @Test fun invalidOrIncompleteResultsAreErrors() {
        for (value in listOf(response("9999"), response(""), response("2", "MAX_TOKENS"),
            response("2", "SAFETY"), JSONObject("{}"), response("002"))) {
            assertThrows(Exception::class.java) { classifier.reply(value) }
        }
    }

    @Test fun missingKeyFailsBeforeNetworking() {
        assertThrows(IllegalArgumentException::class.java) { classifier.classify(byteArrayOf(), " ") }
    }
    private class FakeConnection(private val status: Int, private val body: String) :
        HttpURLConnection(URL("https://example.test")) {
        val sent = ByteArrayOutputStream()
        var closed = false
        override fun connect() {}
        override fun disconnect() { closed = true }
        override fun usingProxy() = false
        override fun getOutputStream() = sent
        override fun getResponseCode() = status
        override fun getInputStream() = body.byteInputStream()
    }

    @Test fun postsDirectlyWithHeaderKeyAndClosesConnection() {
        val connection = FakeConnection(200, response("2").toString())
        val client = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) { url ->
            assertEquals("generativelanguage.googleapis.com", url.host)
            assertTrue(url.path.endsWith(":generateContent"))
            assertNull(url.query)
            connection
        }
        assertEquals("clip:002", client.classify(byteArrayOf(1, 2), "test-key"))
        assertEquals("test-key", connection.getRequestProperty("x-goog-api-key"))
        assertEquals("POST", connection.requestMethod)
        assertFalse(connection.instanceFollowRedirects)
        assertTrue(connection.sent.toString("UTF-8").contains("inlineData"))
        assertTrue(connection.closed)
    }

    @Test fun httpErrorsAndMalformedResponsesCloseConnection() {
        for (status in listOf(401, 429, 500, 200)) {
            val connection = FakeConnection(status, "not-json-secret-provider-body")
            val client = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) { connection }
            assertThrows(Exception::class.java) { client.classify(byteArrayOf(1, 2), "test-key") }
            assertTrue(connection.closed)
        }
    }

    @Test fun keyTestUsesConfiguredModelAndHeaderWithoutAudio() {
        val connection = FakeConnection(200, response("ready").toString())
        val client = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) { url ->
            assertTrue(url.path.endsWith("/${GeminiIntent.MODEL}:generateContent"))
            assertNull(url.query)
            connection
        }
        assertEquals("API key valid; Gemini is ready", client.testKey("test-key"))
        assertEquals("test-key", connection.getRequestProperty("x-goog-api-key"))
        val body = connection.sent.toString("UTF-8")
        assertFalse(body.contains("inlineData"))
        assertFalse(body.contains("test-key"))
        assertTrue(connection.closed)
    }

    @Test fun keyTestDistinguishesFailuresWithoutExposingProviderBody() {
        val expected = mapOf(400 to "rejected", 401 to "not accepted", 403 to "Access denied",
            404 to "model is unavailable", 429 to "Quota", 503 to "HTTP 503")
        for ((code, message) in expected) {
            val connection = FakeConnection(code, "secret-provider-detail")
            val client = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) { connection }
            val result = client.testKey("test-key")
            assertTrue(result.contains(message))
            assertFalse(result.contains("secret-provider-detail"))
            assertFalse(result.contains("test-key"))
            assertTrue(connection.closed)
        }
    }

    @Test fun keyTestRejectsEmptyInputAndUnexpectedResponses() {
        val offline = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) {
            error("Must not contact Gemini")
        }
        assertEquals("Enter a Gemini API key first", offline.testKey(" "))
        for (body in listOf("not json", "{}", response("ready", "MAX_TOKENS").toString(),
            response("wrong").toString())) {
            val connection = FakeConnection(200, body)
            val client = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) { connection }
            assertTrue(client.testKey("test-key").contains("unexpected response"))
            assertTrue(connection.closed)
        }
    }

    @Test fun requestLogErrorsNeverExposeExceptionMessagesOrProviderBodies() {
        assertEquals("network error", GeminiIntent.failureSummary(java.io.IOException("secret-key")))
        assertEquals("network timeout", GeminiIntent.failureSummary(java.net.SocketTimeoutException("secret-key")))
        assertEquals("invalid or incomplete response", GeminiIntent.failureSummary(IllegalStateException("secret-body")))
        val client = GeminiIntent(File("src/main/assets/intent_topics.json").readText()) {
            FakeConnection(429, "secret-provider-body")
        }
        val error = assertThrows(Exception::class.java) { client.classify(byteArrayOf(1), "secret-key") }
        assertEquals("HTTP 429", GeminiIntent.failureSummary(error))
    }

    @Test fun keyTestReportsNetworkAndTimeoutFailures() {
        val catalog = File("src/main/assets/intent_topics.json").readText()
        assertTrue(GeminiIntent(catalog) { throw java.net.SocketTimeoutException() }
            .testKey("test-key").contains("timed out"))
        assertTrue(GeminiIntent(catalog) { throw java.io.IOException("secret-detail") }
            .testKey("test-key").contains("check your connection"))
    }

}
