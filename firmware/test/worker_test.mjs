import assert from "node:assert/strict";
import test from "node:test";
import worker from "../backend/worker.mjs";

const env = { DEVICE_TOKEN: "device", GEMINI_API_KEY: "gemini" };
const L16 = "audio/L16; rate=16000; channels=1; endianness=little-endian";
function wav(length) {
  const data = Buffer.alloc(44 + length);
  data.write("RIFF"); data.writeUInt32LE(data.length - 8, 4);
  data.write("WAVEfmt ", 8); data.writeUInt32LE(16, 16);
  data.writeUInt16LE(1, 20); data.writeUInt16LE(1, 22);
  data.writeUInt32LE(16000, 24); data.writeUInt32LE(32000, 28);
  data.writeUInt16LE(2, 32); data.writeUInt16LE(16, 34);
  data.write("data", 36); data.writeUInt32LE(length, 40);
  return data;
}
const post = (path, body, type = L16, token = "device") => new Request(`https://example.test${path}`, {
  method: "POST", headers: { Authorization: `Bearer ${token}`, "Content-Type": type }, body, duplex: "half",
});
const gemini = text => Response.json({ candidates: [{ finishReason: "STOP", content: { parts: [{ text }] } }] });

test("auth, config and routing gates; provider errors stay private", async t => {
  let calls = 0;
  let status = 429;
  t.mock.method(globalThis, "fetch", async () => { calls++; return new Response("private detail", { status }); });
  assert.equal((await worker.fetch(post("/test-ai", null, L16, "wrong"), env)).status, 401);
  assert.equal((await worker.fetch(post("/test-ai"), { DEVICE_TOKEN: "device" })).status, 503);
  for (const removed of ["/speak", "/reply", "/test-speech"]) {
    assert.equal((await worker.fetch(post(removed), env)).status, 404);
  }
  assert.equal(calls, 0);
  for (status of [429, 403, 500]) {
    const response = await worker.fetch(post("/test-ai"), env);
    assert.equal(response.status, status === 429 ? 429 : 502);
    assert.deepEqual(await response.json(), { ok: false, error: "Gemini request failed", upstream_status: status });
  }
  assert.equal(calls, 3); // No automatic retries.
});

test("health and Gemini text endpoint need only Gemini and device secrets", async t => {
  t.mock.method(globalThis, "fetch", async (url, options) => {
    assert.match(url, /gemini-3\.6-flash:generateContent$/);
    assert.equal(options.headers["x-goog-api-key"], "gemini");
    return gemini("Parrot is ready.");
  });
  const response = await worker.fetch(post("/test-ai"), env);
  assert.equal((await response.json()).text, "Parrot is ready.");
  assert.equal((await worker.fetch(new Request("https://example.test/health"), env)).status, 200);
  assert.equal((await worker.fetch(new Request("https://example.test/health"), { DEVICE_TOKEN: "device" })).status, 503);
});

test("/ask ignores unclear speech", async t => {
  t.mock.method(globalThis, "fetch", async () => gemini("[IGNORE]"));
  const body = await (await worker.fetch(post("/ask", wav(8000), "audio/wav"), env)).json();
  assert.deepEqual([body.ok, body.text, body.ignored], [true, "", true]);
});

test("chunked raw PCM upload is wrapped as WAV; bad uploads never reach Gemini", async t => {
  let sentAudio;
  t.mock.method(globalThis, "fetch", async (url, options) => {
    sentAudio = Buffer.from(JSON.parse(options.body).contents[0].parts[1].inlineData.data, "base64");
    return gemini("1");
  });
  const pcm = wav(8000).subarray(44);
  const stream = (fail = false) => new ReadableStream({
    start(controller) {
      controller.enqueue(pcm.subarray(0, 3001));
      if (fail) return controller.error(new Error("socket closed"));
      controller.enqueue(pcm.subarray(3001));
      controller.close();
    },
  });
  assert.equal((await worker.fetch(post("/intent", stream()), env)).status, 200);
  assert.deepEqual(sentAudio, wav(8000)); // Worker-built header matches firmware's.
  sentAudio = undefined;
  assert.equal((await worker.fetch(post("/intent", stream(true)), env)).status, 400);
  assert.equal((await worker.fetch(post("/intent", pcm.subarray(0, 7998)), env)).status, 400); // Under 0.25 s.
  assert.equal((await worker.fetch(post("/intent", pcm, "audio/L16; rate=16000; channels=1"), env)).status, 415);
  assert.equal(sentAudio, undefined);
});
