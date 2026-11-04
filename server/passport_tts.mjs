// passport_tts.mjs — text to speech for `passportctl say`.
//
//   node passport_tts.mjs "text to speak" out.mp3
//
// Writes an audio file (mp3 by default; passportctl converts it with ffmpeg).
// Settings come from the environment (passportctl passes its .env along):
//
//   TTS_PROVIDER   openai  (default) any OpenAI-compatible POST {TTS_BASE_URL}/audio/speech
//                  minimax MiniMax t2a_v2 (an example of a non-OpenAI API)
//   TTS_BASE_URL   openai: https://api.openai.com/v1   minimax: https://api.minimax.io
//   TTS_API_KEY    bearer token
//   TTS_MODEL      openai: e.g. gpt-4o-mini-tts / tts-1   minimax: e.g. speech-2.6-hd
//   TTS_VOICE      a voice id of your provider
//   TTS_SPEED      1.0
//   MINIMAX_GROUP_ID  optional, older MiniMax accounts need it in the URL
//
// To use something else entirely, set TTS_CMD in .env to any command that takes
// the same two arguments ("text" out-file) and writes an audio file ffmpeg can read.
import { writeFileSync } from "node:fs";

const env = process.env;
const text = process.argv[2];
const out = process.argv[3];
if (!text || !out) {
  console.error('usage: node passport_tts.mjs "text to speak" out.mp3');
  process.exit(2);
}

const provider = (env.TTS_PROVIDER || "openai").toLowerCase();
const speed = Number(env.TTS_SPEED || "1.0");

function need(name) {
  if (!env[name]) {
    console.error(`FAIL: ${name} is not set (TTS_PROVIDER=${provider})`);
    process.exit(1);
  }
  return env[name];
}

async function openaiTTS() {
  const base = (env.TTS_BASE_URL || "https://api.openai.com/v1").replace(/\/+$/, "");
  const headers = { "Content-Type": "application/json" };
  if (env.TTS_API_KEY) headers.Authorization = `Bearer ${env.TTS_API_KEY}`;
  const r = await fetch(`${base}/audio/speech`, {
    method: "POST",
    headers,
    body: JSON.stringify({
      model: env.TTS_MODEL || "tts-1",
      voice: env.TTS_VOICE || "alloy",
      input: text,
      response_format: "mp3",
      speed,
    }),
  });
  if (!r.ok) throw new Error(`HTTP ${r.status}: ${(await r.text()).slice(0, 300)}`);
  return Buffer.from(await r.arrayBuffer());
}

async function minimaxTTS() {
  const base = (env.TTS_BASE_URL || "https://api.minimax.io").replace(/\/+$/, "");
  const group = env.MINIMAX_GROUP_ID ? `?GroupId=${encodeURIComponent(env.MINIMAX_GROUP_ID)}` : "";
  const r = await fetch(`${base}/v1/t2a_v2${group}`, {
    method: "POST",
    headers: { Authorization: `Bearer ${need("TTS_API_KEY")}`, "Content-Type": "application/json" },
    body: JSON.stringify({
      model: env.TTS_MODEL || "speech-2.6-hd",
      text,
      stream: false,
      voice_setting: { voice_id: need("TTS_VOICE"), speed, vol: 1.0, pitch: 0 },
      audio_setting: { sample_rate: 32000, bitrate: 128000, format: "mp3" },
    }),
  });
  const j = await r.json();
  if (!j.data?.audio) throw new Error(JSON.stringify(j).slice(0, 300));
  return Buffer.from(j.data.audio, "hex");
}

const providers = { openai: openaiTTS, minimax: minimaxTTS };
if (!providers[provider]) {
  console.error(`FAIL: unknown TTS_PROVIDER=${provider} (known: ${Object.keys(providers).join(", ")})`);
  process.exit(1);
}
try {
  const audio = await providers[provider]();
  writeFileSync(out, audio);
  console.log("OK", out, audio.length, "bytes");
} catch (e) {
  console.error("FAIL", e.message || e);
  process.exit(1);
}
