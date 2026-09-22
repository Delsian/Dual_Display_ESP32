// Deploy this module through Cloudflare Workers > parrot > Edit code.
const MODEL = "gemini-3.6-flash";
const TTS_MODEL = "gemini-2.5-flash-preview-tts";

async function speechText(request) {
  if (request.headers.get("Content-Type")?.split(";")[0] !== "application/json") {
    return { error: "Expected application/json", status: 415 };
  }
  if (!request.body) return { error: "Missing text", status: 400 };
  const reader = request.body.getReader();
  let size = 0;
  const chunks = [];
  let expired = false;
  const timer = setTimeout(() => { expired = true; reader.cancel().catch(() => {}); }, 15000);
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > 8192) {
        await reader.cancel();
        return { error: "Text request too large", status: 413 };
      }
      chunks.push(value);
    }
    if (expired) return { error: "Text upload timed out", status: 408 };
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
    const data = JSON.parse(new TextDecoder().decode(bytes));
    if (typeof data?.text !== "string" || !data.text.trim() || data.text.length > 1000) {
      return { error: "Expected 1-1000 characters of text", status: 400 };
    }
    return { text: data.text.trim() };
  } catch {
    return { error: "Invalid text request", status: 400 };
  } finally {
    clearTimeout(timer);
    reader.releaseLock();
  }
}

async function recordingPart(request) {
  const limit = 960044; // 30 seconds, 16 kHz mono PCM plus canonical WAV header.
  if (request.headers.get("Content-Type")?.split(";")[0] !== "audio/wav") {
    return { error: "Expected audio/wav", status: 415 };
  }
  if (Number(request.headers.get("Content-Length")) > limit) {
    return { error: "Recording exceeds 30 seconds", status: 413 };
  }
  if (!request.body) return { error: "Missing recording", status: 400 };
  const reader = request.body.getReader();
  const chunks = [];
  let size = 0;
  const timer = setTimeout(() => reader.cancel(), 15000);
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > limit) {
        await reader.cancel();
        return { error: "Recording exceeds 30 seconds", status: 413 };
      }
      chunks.push(value);
    }
  } catch {
    return { error: "Recording upload interrupted", status: 400 };
  } finally {
    clearTimeout(timer);
    reader.releaseLock();
  }
  const wav = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) { wav.set(chunk, offset); offset += chunk.length; }
  const view = new DataView(wav.buffer);
  const label = (pos, text) => [...text].every((c, i) => wav[pos + i] === c.charCodeAt(0));
  if (size < 8044 || !label(0, "RIFF") || !label(8, "WAVEfmt ") || !label(36, "data") ||
      view.getUint32(4, true) !== size - 8 || view.getUint32(16, true) !== 16 ||
      view.getUint16(20, true) !== 1 || view.getUint16(22, true) !== 1 ||
      view.getUint32(24, true) !== 16000 || view.getUint32(28, true) !== 32000 ||
      view.getUint16(32, true) !== 2 || view.getUint16(34, true) !== 16 ||
      view.getUint32(40, true) !== size - 44 || (size - 44) % 2) {
    return { error: "Expected 0.25-30 seconds of 16 kHz mono PCM WAV", status: 400 };
  }
  const binary = [];
  for (let i = 0; i < wav.length; i += 16384) {
    binary.push(String.fromCharCode(...wav.subarray(i, i + 16384)));
  }
  return { inlineData: { mimeType: "audio/wav", data: btoa(binary.join("")) } };
}

function speechResponse(data) {
  const candidate = data.candidates?.[0];
  const audioParts = (candidate?.content?.parts ?? []).filter(part => part.inlineData);
  const audio = audioParts[0]?.inlineData;
  if (candidate?.finishReason !== "STOP" || audioParts.length !== 1 ||
      audio?.mimeType !== "audio/L16;codec=pcm;rate=24000" ||
      typeof audio.data !== "string" || !audio.data.length || audio.data.length > 640000) {
    return json({ ok: false, error: "Gemini returned missing, incomplete, or unsupported audio" }, 502);
  }
  const pcm = atob(audio.data);
  if (!pcm.length || pcm.length % 2 !== 0 || pcm.length > 480000) {
    return json({ ok: false, error: "Invalid audio length" }, 502);
  }
  // Wrap at most 10 seconds of 24 kHz mono PCM in a WAV header; no resampling.
  const wav = new Uint8Array(44 + pcm.length);
  const view = new DataView(wav.buffer);
  const label = (offset, text) => {
    for (let i = 0; i < text.length; i++) wav[offset + i] = text.charCodeAt(i);
  };
  label(0, "RIFF");
  view.setUint32(4, 36 + pcm.length, true);
  label(8, "WAVEfmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, 24000, true);
  view.setUint32(28, 48000, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  label(36, "data");
  view.setUint32(40, pcm.length, true);
  for (let i = 0; i < pcm.length; i++) wav[44 + i] = pcm.charCodeAt(i);
  return new Response(wav, {
    headers: {
      "Content-Type": "audio/wav",
      "Content-Disposition": 'attachment; filename="parrot-test.wav"',
      "Cache-Control": "no-store",
    },
  });
}

function json(data, status = 200) {
  return Response.json(data, {
    status,
    headers: { "Cache-Control": "no-store" },
  });
}

export default {
  async fetch(request, env) {
    const started = Date.now();
    const path = new URL(request.url).pathname;
    if (path !== "/health" && path !== "/test-ai" && path !== "/test-speech" && path !== "/ask" && path !== "/speak") {
      return json({ error: "Not found" }, 404);
    }
    const method = path === "/health" ? "GET" : "POST";
    if (request.method !== method) {
      return new Response("Method not allowed", {
        status: 405,
        headers: { Allow: method },
      });
    }
    const configured = Boolean(env.GEMINI_API_KEY && env.DEVICE_TOKEN);
    if (path === "/health") {
      return json({ ok: configured, service: "parrot-voice" }, configured ? 200 : 503);
    }
    if (!configured) return json({ error: "Missing Worker secrets" }, 503);
    if (request.headers.get("Authorization") !== `Bearer ${env.DEVICE_TOKEN}`) {
      return json({ error: "Unauthorized" }, 401);
    }

    const ask = path === "/ask";
    const timing = { upload_prepare_ms: 0, gemini_ms: 0 };
    const reply = (data, status = 200) => json(ask ? {
      ...data, timing_ms: { ...timing, worker_total_ms: Date.now() - started },
    } : data, status);
    let recording;
    if (ask) {
      const uploadStarted = Date.now();
      recording = await recordingPart(request);
      timing.upload_prepare_ms = Date.now() - uploadStarted;
      if (recording.error) return reply({ ok: false, error: recording.error }, recording.status);
    }
    // Test endpoints use fixed prompts; /ask answers one recorded phrase.
    const speech = path === "/test-speech" || path === "/speak";
    let spokenText = "Parrot is ready.";
    if (path === "/speak") {
      const input = await speechText(request);
      if (input.error) return json({ ok: false, error: input.error }, input.status);
      spokenText = input.text;
    }
    const model = speech ? TTS_MODEL : MODEL;
    const body = speech ? {
      contents: [{ parts: [{ text: `Read the following text aloud exactly as written, without translating or adding words:\n${spokenText}` }] }],
      generationConfig: {
        responseModalities: ["AUDIO"],
        speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: "Kore" } } },
      },
    } : {
      contents: [{ parts: [{ text: "Reply with exactly: Parrot is ready." }] }],
      generationConfig: {
        maxOutputTokens: 256,
        thinkingConfig: { thinkingLevel: "minimal" },
      },
    };
    if (ask) {
      body.systemInstruction = { parts: [{ text:
        "Respond in English and Ukrainian only. Use the user's language when it is English or Ukrainian. " +
        "For any other language, ask in English to speak English or Ukrainian. " +
        "Answer in one short sentence of at most 20 words, using plain text for speech. If speech is unclear or absent, ask in English to repeat it. " +
        "Do not invent words you cannot hear."
      }] };
      body.contents = [{ parts: [
        { text: "Listen to the user's spoken message and respond." },
        recording,
      ] }];
    }
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), speech || ask ? 45000 : 20000);
    const geminiStarted = Date.now();
    try {
      const upstream = await fetch(
        `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,
        {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "x-goog-api-key": env.GEMINI_API_KEY,
          },
          body: JSON.stringify(body),
          signal: controller.signal,
        }
      );
      timing.gemini_ms = Date.now() - geminiStarted;
      if (!upstream.ok) {
        // Do not expose provider error bodies or credentials.
        return reply(
          { ok: false, error: "Gemini request failed", upstream_status: upstream.status },
          upstream.status === 429 ? 429 : 502
        );
      }
      const data = await upstream.json();
      timing.gemini_ms = Date.now() - geminiStarted;
      if (speech) return speechResponse(data);
      const text = (data.candidates?.[0]?.content?.parts ?? [])
        .filter(part => typeof part.text === "string" && !part.thought)
        .map(part => part.text).join("").trim();
      if (!text) return reply({ ok: false, error: "Gemini returned no text" }, 502);
      if (ask && (data.candidates?.[0]?.finishReason !== "STOP" || text.length > 1000)) {
        return reply({ ok: false, error: "Gemini reply was incomplete or too long" }, 502);
      }
      return reply({ ok: true, model: MODEL, text });
    } catch {
      timing.gemini_ms = Date.now() - geminiStarted;
      return reply(
        { ok: false, error: controller.signal.aborted ? "Gemini timed out" : "Gemini connection or response failed" },
        controller.signal.aborted ? 504 : 502
      );
    } finally {
      clearTimeout(timeout);
    }
  },
};
