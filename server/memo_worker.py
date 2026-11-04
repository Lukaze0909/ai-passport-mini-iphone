#!/usr/bin/env python3
"""memo_worker — the Notes app's voice memo pipeline (no companion involved).

  memo_worker.py <wav> <clip_id>
  1) speech to text with asr.py (PASSPORT_ASR)
  2) any OpenAI-compatible chat endpoint (MEMO_LLM_*) turns it into one short English
     sentence (the device shows printable ASCII only)
  3) publish {"op":"memo","clip":<id>,"text":...} to <prefix>/<device>/command
Prints a JSON line with what happened and appends it to $PASSPORT_STATE_DIR/memos.jsonl.
Without MEMO_LLM_BASE_URL the transcript itself is sent if it is plain ASCII, else "?".
"""
import json, os, re, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import config  # noqa: E402
import asr  # noqa: E402

PROMPT = ("Rewrite the spoken memo below as ONE short, natural English sentence, the way people "
          "really write a sticky note or a to-do (not textbook English). Output only that sentence: "
          "no quotes, no explanation, at most 12 words.\n\nMemo: {text}")


def ascii_only(s: str, limit: int = 120) -> str:
    return re.sub(r"\s+", " ", "".join(ch if 32 <= ord(ch) < 127 else " " for ch in s)).strip()[:limit]


def post_json(url, headers, payload, timeout=60):
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                 headers={**headers, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def summarize(text: str) -> str:
    base = config.get("MEMO_LLM_BASE_URL").rstrip("/")
    if not base:
        clean = ascii_only(text)
        return clean if clean and clean == re.sub(r"\s+", " ", text).strip() else ""
    headers = {}
    if config.get("MEMO_LLM_API_KEY"):
        headers["Authorization"] = f"Bearer {config.get('MEMO_LLM_API_KEY')}"
    j = post_json(f"{base}/chat/completions", headers,
                  {"model": config.get("MEMO_LLM_MODEL", "gpt-4o-mini"),
                   "messages": [{"role": "user", "content": PROMPT.format(text=text)}],
                   "max_tokens": config.get_int("MEMO_LLM_MAX_TOKENS", 60)},
                  timeout=config.get_int("MEMO_LLM_TIMEOUT_S", 30))
    out = (j["choices"][0]["message"].get("content") or "").strip()
    out = re.sub(r"<think>.*?</think>", "", out, flags=re.S).strip()   # reasoning models
    out = (out.splitlines() or [""])[0]
    out = re.sub(r"^[\"'“”‘’]+|[\"'“”‘’]+$", "", out).strip()
    return ascii_only(out)


def publish(cmd):
    import passportctl  # reuses its MQTT helper
    return passportctl.send_command(cmd)


def main():
    wav, clip = sys.argv[1], int(sys.argv[2])
    t0 = time.time(); said = english = ""; err = None; info = {}
    try:
        said, info = asr.transcribe(wav)
        english = summarize(said) if said else ""
    except Exception as e:  # noqa: BLE001
        err = str(e)[:200]
    if not english:
        english = "?"
    ok = publish({"op": "memo", "clip": clip, "text": english})
    rec = {"t": time.strftime("%Y-%m-%dT%H:%M:%S"), "clip": clip, "said": said, "text": english,
           "published": ok, "secs": round(time.time() - t0, 1), "asr": info, "err": err}
    state = config.state_dir()
    os.makedirs(state, exist_ok=True)
    with open(os.path.join(state, "memos.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    print(json.dumps(rec, ensure_ascii=False))


if __name__ == "__main__":
    main()
