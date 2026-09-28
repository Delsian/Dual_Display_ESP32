package com.eugkrashtan.parrot

import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.SocketTimeoutException
import java.net.HttpURLConnection
import java.net.URL
import java.util.Base64
import kotlin.random.Random

/** Direct Gemini classifier; credentials and provider bodies are never logged. */
internal class GeminiIntent(
    catalog: String,
    private val openConnection: (URL) -> HttpURLConnection = { it.openConnection() as HttpURLConnection },
) {
    private val data = JSONObject(catalog)
    private val topics = data.getJSONArray("topics")
    private val fallbackCount = data.getInt("fallbackCount")
    private val ids = (0 until topics.length()).map { topics.getJSONObject(it).getInt("id").toString() }

    fun request(wav: ByteArray): JSONObject {
        val descriptions = (0 until topics.length()).joinToString("\n") {
            val topic = topics.getJSONObject(it)
            "${topic.getInt("id")}: ${topic.getString("topic")}"
        }
        val instruction = "Classify the user's spoken question, usually in Ukrainian, into exactly one topic by its number. " +
            "Choose the topic whose description matches the question's meaning, not just shared words. " +
            "Output offtopic for understandable speech that matches no topic. " +
            "Output ignore for silence, noise, or speech you cannot understand confidently.\n\nTopics:\n" + descriptions
        val recording = JSONObject().put("inlineData", JSONObject()
            .put("mimeType", "audio/wav").put("data", Base64.getEncoder().encodeToString(wav)))
        return JSONObject()
            .put("systemInstruction", JSONObject().put("parts", JSONArray().put(JSONObject().put("text", instruction))))
            .put("contents", JSONArray().put(JSONObject().put("parts", JSONArray()
                .put(JSONObject().put("text", "Classify this spoken question.")).put(recording))))
            .put("generationConfig", JSONObject()
                .put("responseMimeType", "text/x.enum")
                .put("responseSchema", JSONObject().put("type", "STRING")
                    .put("enum", JSONArray(ids + listOf("offtopic", "ignore"))))
                .put("maxOutputTokens", 16)
                .put("thinkingConfig", JSONObject().put("thinkingLevel", "minimal")))
    }

    fun reply(response: JSONObject): String {
        val candidate = response.optJSONArray("candidates")?.optJSONObject(0)
            ?: error("Gemini returned no candidate")
        require(candidate.optString("finishReason") == "STOP") { "Gemini reply was incomplete" }
        val parts = candidate.optJSONObject("content")?.optJSONArray("parts")
            ?: error("Gemini returned no text")
        val label = (0 until parts.length()).mapNotNull {
            val part = parts.optJSONObject(it)
            if (part == null || part.optBoolean("thought") || part.opt("text") !is String) null
            else part.getString("text")
        }.joinToString("").trim()
        return when {
            label == "ignore" -> "ignore"
            label == "offtopic" && fallbackCount > 0 -> "clip:off_${Random.nextInt(1, fallbackCount + 1)}"
            label in ids -> "clip:${label.padStart(3, '0')}"
            else -> error("Gemini returned an unknown topic")
        }
    }

    fun classify(wav: ByteArray, apiKey: String): String = reply(post(request(wav), apiKey))

    /** A small real request verifies this key can use the app's configured model. */
    fun testKey(apiKey: String): String {
        if (apiKey.isBlank()) return "Enter a Gemini API key first"
        val request = JSONObject()
            .put("contents", JSONArray().put(JSONObject().put("parts", JSONArray()
                .put(JSONObject().put("text", "Reply with ready.")))))
            .put("generationConfig", JSONObject()
                .put("responseMimeType", "text/x.enum")
                .put("responseSchema", JSONObject().put("type", "STRING").put("enum", JSONArray(listOf("ready"))))
                .put("maxOutputTokens", 16)
                .put("thinkingConfig", JSONObject().put("thinkingLevel", "minimal")))
        return try {
            val candidate = post(request, apiKey).optJSONArray("candidates")?.optJSONObject(0)
            val parts = candidate?.optJSONObject("content")?.optJSONArray("parts")
            val text = if (parts == null) "" else (0 until parts.length()).mapNotNull {
                val part = parts.optJSONObject(it)
                if (part == null || part.optBoolean("thought")) null else part.optString("text")
            }.joinToString("").trim()
            if (candidate?.optString("finishReason") == "STOP" && text == "ready") {
                "API key valid; Gemini is ready"
            } else {
                "Gemini returned an unexpected response; test again"
            }
        } catch (error: HttpFailure) {
            when (error.status) {
                400 -> "Gemini rejected the key or test request (HTTP 400)"
                401 -> "API key was not accepted (HTTP 401)"
                403 -> "Access denied; check API key restrictions and permissions (HTTP 403)"
                404 -> "Configured Gemini model is unavailable (HTTP 404)"
                429 -> "Quota or rate limit reached; key validity could not be confirmed"
                else -> "Gemini test failed (HTTP ${error.status}); try again"
            }
        } catch (_: SocketTimeoutException) {
            "Gemini test timed out; try again"
        } catch (_: IOException) {
            "Could not reach Gemini; check your connection"
        } catch (_: Exception) {
            "Gemini returned an unexpected response; test again"
        }
    }

    private class HttpFailure(val status: Int) : IOException("Gemini HTTP $status")

    private fun post(request: JSONObject, apiKey: String): JSONObject {
        require(apiKey.isNotBlank()) { "Gemini API key is required" }
        val body = request.toString().toByteArray(Charsets.UTF_8)
        val connection = openConnection(URL("https://generativelanguage.googleapis.com/v1beta/models/$MODEL:generateContent"))
        try {
            connection.apply {
                requestMethod = "POST"
                instanceFollowRedirects = false
                connectTimeout = 5_000
                readTimeout = 20_000
                doOutput = true
                setRequestProperty("x-goog-api-key", apiKey)
                setRequestProperty("Content-Type", "application/json")
                setFixedLengthStreamingMode(body.size)
            }
            connection.outputStream.use { it.write(body) }
            if (connection.responseCode !in 200..299) throw HttpFailure(connection.responseCode)
            val response = connection.inputStream.bufferedReader().use { it.readText() }
            return JSONObject(response)
        } finally {
            connection.disconnect()
        }
    }

    companion object {
        const val MODEL = "gemini-3.6-flash"
    }
}
