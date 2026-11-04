"""asr.py — pluggable speech-to-text for voice clips (16 kHz mono WAV).

    PASSPORT_ASR=sensevoice   local SenseVoice through sherpa-onnx (default)
    PASSPORT_ASR=openai       any OpenAI-compatible POST {ASR_BASE_URL}/audio/transcriptions
    PASSPORT_ASR=none         no transcription (the companion gets only the audio path)

transcribe(wav) returns (text, info). `text` is "" when nothing usable came back;
`info` says why (engine, error, or "dropped: repetition").

Whisper-family models (and sometimes others) hallucinate on silence or noise by
repeating one short phrase over and over. looks_pathological() catches that and
the transcript is dropped instead of being passed on.

    python3 asr.py some.wav      # try the configured engine on a file
"""
import json, os, re, sys, uuid, wave
import urllib.request

import config

_recognizer = None


# ------------------------------------------------------------------ repetition guard
_PUNCT = re.compile(r"[\s\.,!?;:'\"()\[\]{}<>…~\-—_/\\|，。！？；：、“”‘’（）《》【】「」·]+")


def _units(text: str) -> list:
    """Words for space-separated text, characters for CJK and the like."""
    words = [w for w in _PUNCT.split(text.lower()) if w]
    if len(words) >= 3 and sum(len(w) for w in words) / len(words) < 15:
        return words
    return list(_PUNCT.sub("", text.lower()))


def looks_pathological(text: str, min_repeats: int | None = None, max_period: int = 15,
                       min_coverage: float = 0.5) -> bool:
    """True when one short phrase (1..max_period units) repeats back to back at
    least `min_repeats` times and that run covers at least `min_coverage` of the
    whole transcript, e.g. "thank you thank you thank you thank you ..." or
    "谢谢观看谢谢观看谢谢观看谢谢观看". Short real repetitions ("no no no") pass."""
    if min_repeats is None:
        min_repeats = config.get_int("ASR_REPEAT_MIN", 4)
    u = _units(text)
    n = len(u)
    if n < min_repeats:
        return False
    for p in range(1, min(max_period, n // min_repeats) + 1):
        for i in range(n - p + 1):
            reps = 1
            while i + (reps + 1) * p <= n and u[i + reps * p:i + (reps + 1) * p] == u[i:i + p]:
                reps += 1
            run = reps * p
            if reps >= min_repeats and run >= max(min_repeats, 6) and run / n >= min_coverage:
                return True
    return False


# ------------------------------------------------------------------ engines
def _read_wav_float(wav: str):
    """Samples as float32 in [-1, 1] (numpy array) and the sample rate."""
    import numpy as np   # sherpa-onnx depends on numpy anyway
    with wave.open(wav, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("expected 16-bit PCM WAV")
        sr, ch, raw = w.getframerate(), w.getnchannels(), w.readframes(w.getnframes())
    a = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1)
    return a, sr


def _sensevoice(wav: str) -> str:
    """Local SenseVoice (sherpa-onnx). Model files: a sherpa-onnx SenseVoice
    release, e.g. sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17
    (model.int8.onnx + tokens.txt).

    Signature checked against sherpa-onnx 2.0.1: from_sense_voice(model, tokens,
    num_threads, ..., language, use_itn). Not yet run end to end with a real model.
    API reference:
    https://github.com/k2-fsa/sherpa-onnx/blob/master/python-api-examples/offline-sense-voice-ctc-decode-files.py
    https://k2-fsa.github.io/sherpa/onnx/sense-voice/index.html
    """
    global _recognizer
    import sherpa_onnx
    if _recognizer is None:
        model = config.path("SENSEVOICE_MODEL", "models/sensevoice/model.int8.onnx")
        tokens = config.path("SENSEVOICE_TOKENS", os.path.join(os.path.dirname(model), "tokens.txt"))
        if not os.path.exists(model):
            raise FileNotFoundError(f"SenseVoice model not found: {model} (set SENSEVOICE_MODEL)")
        _recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=model,
            tokens=tokens,
            num_threads=config.get_int("SENSEVOICE_THREADS", 2),
            language=config.get("SENSEVOICE_LANGUAGE", "auto"),   # auto / zh / en / ja / ko / yue
            use_itn=True,                                         # punctuation + inverse text normalisation
            debug=False,
        )
    samples, sr = _read_wav_float(wav)
    stream = _recognizer.create_stream()
    stream.accept_waveform(sr, samples)
    _recognizer.decode_stream(stream)
    text = stream.result.text or ""
    return re.sub(r"<\|[^|>]*\|>", "", text).strip()   # in case language/emotion tags leak through


def _multipart(fields: dict, file_field: str, filename: str, data: bytes, ctype: str):
    boundary = uuid.uuid4().hex
    out = bytearray()
    for k, v in fields.items():
        out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n").encode()
    out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{file_field}\"; "
            f"filename=\"{filename}\"\r\nContent-Type: {ctype}\r\n\r\n").encode()
    out += data + f"\r\n--{boundary}--\r\n".encode()
    return bytes(out), f"multipart/form-data; boundary={boundary}"


def _openai(wav: str) -> str:
    base = config.get("ASR_BASE_URL", "https://api.openai.com/v1").rstrip("/")
    fields = {"model": config.get("ASR_MODEL", "whisper-1"), "response_format": "json"}
    if config.get("ASR_LANGUAGE"):
        fields["language"] = config.get("ASR_LANGUAGE")
    with open(wav, "rb") as f:
        body, ctype = _multipart(fields, "file", os.path.basename(wav), f.read(), "audio/wav")
    headers = {"Content-Type": ctype}
    if config.get("ASR_API_KEY"):
        headers["Authorization"] = f"Bearer {config.get('ASR_API_KEY')}"
    req = urllib.request.Request(f"{base}/audio/transcriptions", data=body, headers=headers)
    with urllib.request.urlopen(req, timeout=config.get_int("ASR_TIMEOUT_S", 90)) as r:
        j = json.load(r)
    return (j.get("text") or "").strip()


ENGINES = {"sensevoice": _sensevoice, "openai": _openai}


def transcribe(wav: str) -> tuple[str, dict]:
    engine = config.get("PASSPORT_ASR", "sensevoice").lower()
    info = {"engine": engine}
    if engine in ("none", "off", ""):
        return "", info
    fn = ENGINES.get(engine)
    if fn is None:
        info["error"] = f"unknown PASSPORT_ASR={engine!r}"
        return "", info
    try:
        text = fn(wav)
    except Exception as e:  # noqa: BLE001
        info["error"] = f"{type(e).__name__}: {e}"[:200]
        return "", info
    if text and looks_pathological(text):
        info["dropped"] = "repetition"
        info["raw"] = text[:200]
        return "", info
    return text, info


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: asr.py <16kHz mono wav>")
    t, i = transcribe(sys.argv[1])
    print(json.dumps({"text": t, **i}, ensure_ascii=False))
