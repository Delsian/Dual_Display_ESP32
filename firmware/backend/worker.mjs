// Deploy this module through Cloudflare Workers > parrot > Edit code.
const MODEL = "gemini-3.6-flash";

// BEGIN GENERATED TOPICS: edit backend/topics.json, then run backend/convert_phrases.py.
const TOPICS = [
  [1, "Weather, appearance, color, what can be seen, visible things"],
  [2, "Rain, sea, water, swimming, being wet"],
  [3, "Heat, cold, temperature, hot or cold weather"],
  [4, "Wind, storm, hurricane, bad weather, strong wind"],
  [5, "Time, current time, what time it is, hours and minutes"],
  [6, "Date, day, calendar, today, tomorrow, yesterday"],
  [7, "Location, where we are, where something is, direction"],
  [8, "Distance, how far, how close, nearby places"],
  [9, "Navigation, route, directions, where to go, how to get somewhere"],
  [10, "Maps, coordinates, compass, orientation, finding direction"],
  [11, "Sea, ocean, waves, large bodies of water"],
  [12, "Ships, boats, vessels, sailing ships, ship construction"],
  [13, "Pirates, piracy, corsairs, buccaneers, pirate life"],
  [14, "Captain, leadership, commanding a ship, giving orders"],
  [15, "Long John Silver, John Silver, Silver, the parrot's owner"],
  [16, "Captain Flint, the parrot's name, who the parrot is, questions about the parrot itself"],
  [17, "Parrot age, birthday, how old Captain Flint is"],
  [18, "Parrot appearance, looks, feathers, color, beauty, what Captain Flint looks like"],
  [19, "Birds, parrots, flying, feathers, other bird species"],
  [20, "Animals, wild animals, pets, animal behavior"],
  [21, "Fish, marine animals, sea creatures, creatures living underwater"],
  [22, "Sharks, dangerous animals, predators, dangerous sea creatures"],
  [23, "Food, eating, meals, what to eat, hunger"],
  [24, "Taste, whether food is tasty, good or bad food, favorite food"],
  [25, "Rum, alcohol, beer, wine, drinking, alcoholic beverages"],
  [26, "Water, drinks, thirst, what to drink, non-alcoholic beverages"],
  [27, "Money, price, cost, gold, coins, wealth, treasure value"],
  [28, "Treasure, gold treasure, treasure hunting, hidden riches"],
  [29, "Where treasure is hidden, treasure maps, clues, finding treasure"],
  [30, "Weapons, sword, saber, pistol, musket, cannon, how weapons work"],
  [31, "Fight, combat, fighting, who is stronger, physical confrontation"],
  [32, "Danger, safety, risk, whether something is safe or dangerous"],
  [33, "Advice, what should I do, recommendations, asking what action to take"],
  [34, "Can something be done, possibility, permission, whether something is possible"],
  [35, "How to do something, instructions, procedure, practical tasks"],
  [36, "Why something happened, causes, reasons, explanation of an event"],
  [37, "Who is responsible, whose fault it is, blame, responsibility"],
  [38, "Yes or no questions, confirmation, whether something is true"],
  [39, "Choosing between two or more options, which one to buy, which is better"],
  [40, "Mathematics, numbers, counting, calculations, arithmetic"],
  [41, "Science, physics, chemistry, biology, scientific explanations"],
  [42, "Technology, machines, mechanisms, engines, devices, repairing things"],
  [43, "Computers, programming, software, operating systems, computer problems"],
  [44, "Phones, internet, social networks, modern communications, modern technology"],
  [45, "Medicine, health, injuries, pain, illness, feeling sick"],
  [46, "Clothes, clothing, what to wear, fashion, shoes"],
  [47, "Love, romance, relationships, dating, marriage"],
  [48, "Intelligence, stupidity, difficult questions, complicated explanations"],
  [49, "Books, history, literature, knowledge, education, learning"],
  [50, "Greetings, hello, how are you, small talk, casual conversation"],
];
const FALLBACK_CLIPS = 10;
// END GENERATED TOPICS

// /intent picks a prerecorded clip: topic N plays clips/NNN.wav, off-topic
// questions play a random clips/off_K.wav, and noise plays nothing.
function intentBody(recording) {
  const topics = TOPICS.map(([id, topic]) => `${id}: ${topic}`).join("\n");
  return {
    systemInstruction: { parts: [{ text:
      "Classify the user's spoken question, usually in Ukrainian, into exactly one topic by its number. " +
      "Choose the topic whose description matches the question's meaning, not just shared words. " +
      "Output offtopic for understandable speech that matches no topic. " +
      "Output ignore for silence, noise, or speech you cannot understand confidently.\n\nTopics:\n" + topics,
    }] },
    contents: [{ parts: [{ text: "Classify this spoken question." }, recording] }],
    generationConfig: {
      responseMimeType: "text/x.enum",
      responseSchema: { type: "STRING", enum: [...TOPICS.map(([id]) => String(id)), "offtopic", "ignore"] },
      maxOutputTokens: 16,
      thinkingConfig: { thinkingLevel: "minimal" },
    },
  };
}

// `text` is only for serial logs: the topic description, or the label.
function intentReply(label) {
  if (label === "ignore") return { clip: "", text: "", ignored: true };
  if (label === "offtopic" && FALLBACK_CLIPS > 0) {
    return { clip: `off_${1 + Math.floor(Math.random() * FALLBACK_CLIPS)}`, text: "offtopic", topic: "offtopic" };
  }
  const topic = TOPICS.find(([id]) => String(id) === label);
  return topic && { clip: String(topic[0]).padStart(3, "0"), text: topic[1], topic: topic[0] };
}

// Canonical header for 16 kHz mono 16-bit PCM.
function wavHeader(wav, pcmBytes) {
  const view = new DataView(wav.buffer, wav.byteOffset);
  const label = (offset, text) => {
    for (let i = 0; i < text.length; i++) wav[offset + i] = text.charCodeAt(i);
  };
  label(0, "RIFF");
  view.setUint32(4, 36 + pcmBytes, true);
  label(8, "WAVEfmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, 16000, true);
  view.setUint32(28, 32000, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  label(36, "data");
  view.setUint32(40, pcmBytes, true);
}

async function recordingPart(request) {
  // Streaming firmware sends raw PCM while recording, so its length is unknown
  // until the chunked body ends; the Worker adds the WAV header afterward.
  const [type, ...params] = (request.headers.get("Content-Type") ?? "").toLowerCase().split(";").map(x => x.trim());
  const raw = type === "audio/l16";
  // L16 is big-endian by default; firmware declares its little-endian samples.
  if (raw && !["rate=16000", "channels=1", "endianness=little-endian"].every(p => params.includes(p))) {
    return { error: "Expected little-endian audio/L16 at 16 kHz mono", status: 415 };
  }
  if (!raw && type !== "audio/wav") {
    return { error: "Expected audio/wav", status: 415 };
  }
  const header = raw ? 44 : 0;
  const limit = 960044 - header; // 30 seconds, 16 kHz mono PCM plus canonical WAV header.
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
  const wav = new Uint8Array(header + size);
  let offset = header;
  for (const chunk of chunks) { wav.set(chunk, offset); offset += chunk.length; }
  if (raw) {
    wavHeader(wav, size);
    size += header;
  }
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

// One provider POST with a timeout; failures never expose provider bodies or credentials.
async function callGoogle(url, key, body, provider, timeoutMs) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  const started = Date.now();
  const result = { headers_ms: 0, body_ms: 0, total_ms: 0 };
  try {
    const upstream = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json", "x-goog-api-key": key },
      body: JSON.stringify(body),
      signal: controller.signal,
    });
    result.headers_ms = result.total_ms = Date.now() - started;
    if (!upstream.ok) {
      result.failure = { ok: false, error: `${provider} request failed`, upstream_status: upstream.status };
      result.status = upstream.status === 429 ? 429 : 502;
      return result;
    }
    result.data = await upstream.json();
    result.total_ms = Date.now() - started;
    result.body_ms = result.total_ms - result.headers_ms;
    return result;
  } catch {
    result.total_ms = Date.now() - started;
    const aborted = controller.signal.aborted;
    result.failure = { ok: false, error: aborted ? `${provider} timed out` : `${provider} connection or response failed` };
    result.status = aborted ? 504 : 502;
    return result;
  } finally {
    clearTimeout(timeout);
  }
}

function json(data, status = 200) {
  return Response.json(data, {
    status,
    headers: { "Cache-Control": "no-store" },
  });
}

const PATHS = ["/health", "/test-ai", "/ask", "/intent"];
const GEMINI_URL = `https://generativelanguage.googleapis.com/v1beta/models/${MODEL}:generateContent`;

export default {
  async fetch(request, env) {
    const started = Date.now();
    const path = new URL(request.url).pathname;
    if (!PATHS.includes(path)) return json({ error: "Not found" }, 404);
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

    // /ask answers one recorded phrase; /intent picks a prerecorded clip for it.
    const intent = path === "/intent";
    const ask = path === "/ask" || intent;
    const timing = { upload_prepare_ms: 0, gemini_ms: 0, gemini_headers_ms: 0, gemini_body_ms: 0 };
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
    const body = {
      contents: [{ parts: [{ text: "Reply with exactly: Parrot is ready." }] }],
      generationConfig: {
        maxOutputTokens: 256,
        thinkingConfig: { thinkingLevel: "minimal" },
      },
    };
    if (intent) {
      Object.assign(body, intentBody(recording));
    } else if (ask) {
      body.systemInstruction = { parts: [{ text:
        "Respond in English and Ukrainian only. Use the user's language when it is English or Ukrainian. " +
        "For any other language, ask in English to speak English or Ukrainian. " +
        "If speech is absent or cannot be understood confidently, output exactly [IGNORE] and nothing else. Do not ask for repetition or answer noise. " +
        "Otherwise, answer in one short sentence of at most 20 words, using plain text for speech. " +
        "Do not invent words you cannot hear."
      }] };
      body.contents = [{ parts: [
        { text: "Listen to the user's spoken message and respond." },
        recording,
      ] }];
    }
    const gemini = await callGoogle(GEMINI_URL, env.GEMINI_API_KEY, body, "Gemini", ask ? 45000 : 20000);
    timing.gemini_headers_ms = gemini.headers_ms;
    timing.gemini_body_ms = gemini.body_ms;
    timing.gemini_ms = gemini.total_ms;
    if (gemini.failure) return reply(gemini.failure, gemini.status);
    const data = gemini.data;
    const text = (data?.candidates?.[0]?.content?.parts ?? [])
      .filter(part => typeof part.text === "string" && !part.thought)
      .map(part => part.text).join("").trim();
    if (!text) return reply({ ok: false, error: "Gemini returned no text" }, 502);
    if (ask && (data?.candidates?.[0]?.finishReason !== "STOP" || text.length > 1000)) {
      return reply({ ok: false, error: "Gemini reply was incomplete or too long" }, 502);
    }
    if (intent) {
      const selected = intentReply(text);
      if (!selected) return reply({ ok: false, error: "Gemini returned an unknown topic" }, 502);
      return reply({ ok: true, model: MODEL, ...selected });
    }
    if (ask && text === "[IGNORE]") {
      return reply({ ok: true, model: MODEL, text: "", ignored: true });
    }
    return reply({ ok: true, model: MODEL, text });
  },
};
