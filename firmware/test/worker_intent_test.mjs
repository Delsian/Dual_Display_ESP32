import assert from "node:assert/strict";
import test from "node:test";
import worker from "../backend/worker.mjs";

const env = { DEVICE_TOKEN: "device", GEMINI_API_KEY: "gemini" };
const request = () => new Request("https://example.test/intent", {
  method: "POST",
  headers: { Authorization: "Bearer device",
    "Content-Type": "audio/L16; rate=16000; channels=1; endianness=little-endian" },
  body: new Uint8Array(16000),
});
const gemini = label => Response.json({
  candidates: [{ finishReason: "STOP", content: { parts: [{ text: label }] } }],
});

test("intent restricts Gemini to topic ids and maps them to clips", async t => {
  let sent;
  let label = "2";
  t.mock.method(globalThis, "fetch", async (url, options) => {
    sent = { url, body: JSON.parse(options.body) };
    return gemini(label);
  });
  let response = await worker.fetch(request(), env);
  let data = await response.json();
  assert.equal(response.status, 200);
  assert.match(sent.url, /generateContent$/);
  const config = sent.body.generationConfig;
  assert.equal(config.responseMimeType, "text/x.enum");
  assert.deepEqual(config.responseSchema.enum.slice(-2), ["offtopic", "ignore"]);
  assert.ok(config.responseSchema.enum.includes("2"));
  assert.match(sent.body.systemInstruction.parts[0].text, /\n2: /);
  assert.equal(sent.body.contents[0].parts[1].inlineData.mimeType, "audio/wav");
  assert.equal(data.ok, true);
  assert.equal(data.clip, "002");
  assert.equal(data.topic, 2);
  assert.match(data.text, /Rain/); // Topic description, for serial logs.

  label = "offtopic";
  data = await (await worker.fetch(request(), env)).json();
  assert.match(data.clip, /^off_([1-9]|10)$/);
  assert.equal(data.topic, "offtopic");

  label = "ignore";
  data = await (await worker.fetch(request(), env)).json();
  assert.deepEqual([data.ok, data.clip, data.text, data.ignored], [true, "", "", true]);

  label = "9999";
  response = await worker.fetch(request(), env);
  assert.equal(response.status, 502);
});
