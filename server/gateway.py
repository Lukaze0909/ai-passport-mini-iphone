#!/usr/bin/env python3
"""AI Passport Mini iPhone — gateway (server side).

  device <-> MQTT broker <-> this gateway -> companion hook (your agent)

- voice:   reassemble IMA-ADPCM chunks from the device into WAV; on voice_send,
           transcribe (asr.py) and hand a `passport.voice` event to the companion
- memo:    voice clips from the Notes app go to memo_worker.py instead (ASR + LLM
           one-liner straight back to the device; the companion is not involved)
- games:   minesweeper (mines.py) and the daily Bingo card (bingo.py)
- astro:   astro dice rolls -> `passport.astro` event
- weather: every 15 min, Open-Meteo weather (+ optional AQI) -> retained MQTT topic
- audio:   serve short-lived audio files for the device at /passport-audio/<48 hex>
- status:  keep the latest device status in $PASSPORT_STATE_DIR/status.json

Events reach the companion through COMPANION_HOOK_CMD (event JSON on stdin) or,
when that is unset, as lines appended to $PASSPORT_OUTBOX. See README.md.
"""
import json, os, re, secrets, shlex, struct, subprocess, sys, threading, time, wave, fcntl
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

import config
import asr
import mines
import bingo

STATE = config.state_dir()
CLIPS, AUDIO = os.path.join(STATE, "clips"), os.path.join(STATE, "audio")
OUTBOX = config.path("PASSPORT_OUTBOX", os.path.join(STATE, "outbox.jsonl"))
HOOK_CMD = config.get("COMPANION_HOOK_CMD")
HOOK_TIMEOUT_S = config.get_int("COMPANION_HOOK_TIMEOUT_S", 900)
HOOK_LOCK = os.path.join(STATE, "hook.lock")
TZ = config.tz()
AUDIO_TTL_S = config.get_int("PASSPORT_AUDIO_TTL_S", 3600)
CLIP_KEEP_S = config.get_int("PASSPORT_CLIP_KEEP_S", 3 * 86400)
TRANSCRIBE_VOICE = config.get_bool("PASSPORT_TRANSCRIBE_VOICE", True)
CTL = config.ctl()


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ------------------------------------------------------------------ IMA ADPCM
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
        66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
        408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
        7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_decode(data: bytes, pred: int, idx: int) -> bytes:
    out = bytearray()
    for byte in data:
        for nib in (byte & 0x0F, byte >> 4):           # low nibble first
            step = STEP[idx]
            diff = step >> 3
            if nib & 1: diff += step >> 2
            if nib & 2: diff += step >> 1
            if nib & 4: diff += step
            pred = pred - diff if nib & 8 else pred + diff
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IDX[nib & 7]))
            out += struct.pack("<h", pred)
    return bytes(out)


# ------------------------------------------------------------------ clips
clips = {}          # clip_id -> {"chunks": {seq: pcm}, "began": t, "ended": bool, "meta": {}}
clips_lock = threading.Lock()


def on_audio(payload: bytes):
    if len(payload) < 12:
        return
    cid, seq, flags, pred, idx, _ = struct.unpack("<IHHhBB", payload[:12])
    pcm = adpcm_decode(payload[12:], pred, min(88, idx))
    with clips_lock:
        c = clips.setdefault(cid, {"chunks": {}, "began": time.time(), "ended": False, "meta": {}})
        c["chunks"][seq] = pcm
        if flags & 1:
            c["last_seq"] = seq


def finish_clip(cid: int) -> dict | None:
    with clips_lock:
        c = clips.pop(cid, None)
    if not c or not c["chunks"]:
        return None
    end = c.get("meta", {}).get("end") or {}
    last = end.get("last_seq") if isinstance(end.get("last_seq"), int) else max(c["chunks"])
    last = max(last, max(c["chunks"]))
    lens = sorted(len(v) for v in c["chunks"].values())
    typical = lens[len(lens) // 2]          # chunk size varies by firmware (3000 or 1500 B adpcm)
    pcm = bytearray()
    missing = 0
    for s in range(last + 1):
        chunk = c["chunks"].get(s)
        if chunk is None:
            missing += 1
            chunk = b"\x00" * typical
        pcm += chunk
    name = datetime.now(TZ).strftime("%Y%m%d-%H%M%S") + f"_{cid:08x}.wav"
    path = os.path.join(CLIPS, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
        w.writeframes(bytes(pcm))
    os.chmod(path, 0o600)
    secs = len(pcm) / 32000
    log(f"clip {cid:08x}: {secs:.1f}s chunks={len(c['chunks'])} missing={missing} -> {path}")
    return {"path": path, "seconds": round(secs, 1), "missing_chunks": missing}


# ------------------------------------------------------------------ companion hook
def make_event(kind: str, payload: dict) -> dict:
    return {"id": f"passport-{int(time.time() * 1000)}-{secrets.token_hex(3)}", "kind": kind, "source": "passport",
            "device": config.device_id(), "occurred_at": datetime.now(timezone.utc).isoformat(),
            "payload": payload}


def deliver(event: dict):
    """One event to the companion: COMPANION_HOOK_CMD (stdin = event JSON, one
    at a time) or, if unset or failing, a line in the outbox file."""
    line = json.dumps(event, ensure_ascii=False)
    if HOOK_CMD:
        try:
            with open(HOOK_LOCK, "w") as lf:
                fcntl.flock(lf, fcntl.LOCK_EX)
                r = subprocess.run(shlex.split(HOOK_CMD), input=line, text=True,
                                   capture_output=True, timeout=HOOK_TIMEOUT_S)
            log("hook", event["kind"], r.returncode, (r.stdout or r.stderr).strip()[-200:])
            if r.returncode == 0:
                return
        except Exception as e:  # noqa: BLE001
            log("hook failed", event["kind"], e)
    with open(OUTBOX, "a", encoding="utf-8") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.write(line + "\n")
    log("outbox", event["kind"])


def inject(kind: str, payload: dict):
    """Hand an event to the companion without blocking the caller."""
    event = make_event(kind, payload)
    threading.Thread(target=deliver, args=(event,), daemon=True).start()


info_meta = {}      # clip id -> voice_begin meta (kept after finish_clip pops the clip)
_mqtt = None        # the gateway's client, for publishing commands


def publish_command(cmd: dict) -> bool:
    if _mqtt is None:
        return False
    info = _mqtt.publish(config.topic("command"), json.dumps(cmd, ensure_ascii=True, separators=(",", ":")), qos=1)
    log("command", cmd.get("op"), cmd.get("cell", ""), "rc", info.rc)
    return info.rc == mqtt.MQTT_ERR_SUCCESS


# ------------------------------------------------------------------ astro dice
ASTRO_PLANETS = {"Sun": "☉", "Moon": "☽", "Mercury": "☿", "Venus": "♀", "Mars": "♂", "Jupiter": "♃",
                 "Saturn": "♄", "Uranus": "♅", "Neptune": "♆", "Pluto": "♇", "North Node": "☊",
                 "South Node": "☋"}
ASTRO_SIGNS = {"Aries": "♈", "Taurus": "♉", "Gemini": "♊", "Cancer": "♋", "Leo": "♌", "Virgo": "♍",
               "Libra": "♎", "Scorpio": "♏", "Sagittarius": "♐", "Capricorn": "♑", "Aquarius": "♒",
               "Pisces": "♓"}
_astro_last = {"key": None, "at": 0.0}


def on_astro(ev: dict):
    planet, sign, house = ev.get("planet"), ev.get("sign"), ev.get("house")
    if planet not in ASTRO_PLANETS or sign not in ASTRO_SIGNS or type(house) is not int or not 1 <= house <= 12:
        log("astro_roll rejected", ev)
        return
    key = (planet, sign, house)
    if _astro_last["key"] == key and time.time() - _astro_last["at"] < 10:   # QoS 1 re-delivery
        return
    _astro_last.update(key=key, at=time.time())
    log("astro_roll", planet, sign, house)
    inject("passport.astro", {
        "hint": (f"The owner rolled the astro dice on the device: {planet} / {sign} / house {house}. "
                 "Give them a short reading through your usual channel; if they may not know a "
                 "symbol, say which planet or sign it is."),
        "planet": planet, "sign": sign, "house": house,
        "symbols": f"{ASTRO_PLANETS[planet]} {ASTRO_SIGNS[sign]} {house}"})


# ------------------------------------------------------------------ device events
def run_memo(path: str, cid: int):
    r = subprocess.run([sys.executable, os.path.join(config.HERE, "memo_worker.py"), path, str(cid)],
                       capture_output=True, text=True, timeout=150)
    log("memo", r.returncode, (r.stdout or r.stderr).strip()[-200:])


def voice_event(d: dict, app: str) -> dict:
    payload = {"app": app, "audio_path": d["path"], "seconds": d["seconds"],
               "missing_chunks": d["missing_chunks"], "transcript": None}
    if TRANSCRIBE_VOICE:
        text, info = asr.transcribe(d["path"])
        payload["transcript"] = text or None
        payload["asr"] = info
    if app == "msg":
        payload["hint"] = ("The owner sent a voice message from the Messages app. Read the transcript "
                           "(or listen to audio_path), then reply with a text bubble: "
                           f"`{CTL} msg \"English text\"` (printable ASCII only, <= 280 chars).")
    else:
        payload["hint"] = ("The owner held the talk button in the Call app and sent a voice clip. "
                           "Read the transcript (or listen to audio_path), then answer by voice: "
                           f"`{CTL} say \"text\"`.")
    return payload


def on_event(ev: dict):
    t, cid = ev.get("type"), ev.get("id")
    if t == "astro_roll":
        on_astro(ev)
        return
    if isinstance(t, str) and t.startswith("mine_"):     # game events -> mines.py
        try:
            mines.on_device(ev, publish_command, inject)
        except Exception as e:  # noqa: BLE001
            log("mines failed", t, e)
        return
    if isinstance(t, str) and t.startswith("bingo_"):    # card events -> bingo.py
        try:
            bingo.on_device(ev, publish_command, inject)
        except Exception as e:  # noqa: BLE001
            log("bingo failed", t, e)
        return
    if t == "voice_begin" and isinstance(cid, int):
        with clips_lock:
            # setdefault: chunks that overtook voice_begin must not be wiped
            c = clips.setdefault(cid, {"chunks": {}, "began": time.time(), "ended": False, "meta": {}})
            c["meta"] = {**ev, **{k: v for k, v in c["meta"].items() if k == "end"}}
            info_meta[cid] = ev
            if len(info_meta) > 50:
                info_meta.pop(next(iter(info_meta)))
    elif t == "voice_end" and isinstance(cid, int):
        with clips_lock:
            if cid in clips:
                clips[cid]["ended"] = True
                clips[cid]["meta"]["end"] = ev
    elif t == "voice_cancel" and isinstance(cid, int):
        with clips_lock:
            clips.pop(cid, None)
        log(f"clip {cid:08x} cancelled on the device")
    elif t == "voice_send" and isinstance(cid, int):
        # Wait until every chunk voice_end announced has landed (QoS1 retries can lag), max 3 s.
        deadline = time.time() + 3.0
        while time.time() < deadline:
            with clips_lock:
                c = clips.get(cid)
                want = ((c or {}).get("meta", {}).get("end") or {}).get("chunks")
                have = len(c["chunks"]) if c else 0
            if c is None or (isinstance(want, int) and have >= want):
                break
            time.sleep(0.1)
        time.sleep(0.2)
        d = finish_clip(cid)
        if d:
            app = (info_meta.get(cid) or {}).get("app") or "call"
            if app == "memo":
                run_memo(d["path"], cid)        # Notes: handled without the companion
                return
            inject("passport.voice", voice_event(d, app))
    else:
        log("event", ev)


# ------------------------------------------------------------------ weather
# Condition strings stay Chinese: the device UI and its bitmap font are Chinese.
WMO = {0: ("晴", "sunny"), 1: ("晴", "sunny"), 2: ("多云", "cloudy"), 3: ("阴", "overcast"),
       45: ("雾", "fog"), 48: ("雾", "fog"), 51: ("小雨", "rain"), 53: ("小雨", "rain"),
       55: ("中雨", "rain"), 61: ("小雨", "rain"), 63: ("中雨", "rain"), 65: ("大雨", "rain"),
       71: ("小雪", "snow"), 73: ("中雪", "snow"), 75: ("大雪", "snow"), 80: ("阵雨", "rain"),
       81: ("阵雨", "rain"), 82: ("暴雨", "rain"), 95: ("雷雨", "rain"), 96: ("雷雨", "rain"),
       99: ("雷雨", "rain")}
WEEK = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]
WEATHER_LAT, WEATHER_LON = config.get("WEATHER_LAT"), config.get("WEATHER_LON")
WEATHER_EVERY_S = config.get_int("WEATHER_EVERY_S", 900)


def aqi_level(v: int) -> str:
    for lim, name in ((50, "优"), (100, "良"), (150, "轻度污染"), (200, "中度污染"), (300, "重度污染")):
        if v <= lim:
            return name
    return "严重污染"


def fetch_json(url):
    with urllib.request.urlopen(url, timeout=15) as r:
        return json.load(r)


def fetch_aqi(lat: str, lon: str) -> int | None:
    src = config.get("AQI_SOURCE", "open-meteo").lower()
    if src in ("none", "off"):
        return None
    if src == "waqi":
        token = config.get("AQI_WAQI_TOKEN")
        if not token:
            raise RuntimeError("AQI_SOURCE=waqi needs AQI_WAQI_TOKEN")
        station = config.get("AQI_WAQI_STATION") or f"geo:{lat};{lon}"
        url = f"https://api.waqi.info/feed/{urllib.parse.quote(station, safe=':;@')}/?token={urllib.parse.quote(token)}"
        return int(fetch_json(url)["data"]["aqi"])
    # open-meteo air quality (no key), US AQI scale
    j = fetch_json("https://air-quality-api.open-meteo.com/v1/air-quality?"
                   + urllib.parse.urlencode({"latitude": lat, "longitude": lon, "current": "us_aqi"}))
    v = j.get("current", {}).get("us_aqi")
    return None if v is None else int(round(v))


def build_weather() -> dict:
    q = urllib.parse.urlencode({
        "latitude": WEATHER_LAT, "longitude": WEATHER_LON,
        "current": "temperature_2m,weather_code,is_day",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min",
        "timezone": "auto", "forecast_days": 4})
    w = fetch_json("https://api.open-meteo.com/v1/forecast?" + q)
    cur, day = w["current"], w["daily"]
    cond, icon = WMO.get(cur["weather_code"], ("多云", "cloudy"))
    if not cur.get("is_day", 1) and icon in ("sunny", "cloudy"):
        icon = "night"
    out = {"city": config.get("WEATHER_CITY"), "temp": round(cur["temperature_2m"]), "cond": cond, "icon": icon,
           "hi": round(day["temperature_2m_max"][0]), "lo": round(day["temperature_2m_min"][0]),
           "updated": datetime.now(TZ).strftime("%H:%M"), "days": []}
    for i in range(1, 4):
        dt = datetime.strptime(day["time"][i], "%Y-%m-%d")
        out["days"].append({"d": WEEK[dt.weekday()], "icon": WMO.get(day["weather_code"][i], ("", "cloudy"))[1],
                            "hi": round(day["temperature_2m_max"][i]), "lo": round(day["temperature_2m_min"][i])})
    try:
        a = fetch_aqi(WEATHER_LAT, WEATHER_LON)
        if a is not None:
            out["aqi"] = a
            out["aqi_level"] = aqi_level(a)
    except Exception as e:  # noqa: BLE001
        log("aqi failed", e)
    return out


def weather_loop(client):
    if not (WEATHER_LAT and WEATHER_LON):
        log("weather disabled (set WEATHER_LAT and WEATHER_LON)")
        return
    while True:
        try:
            w = build_weather()
            client.publish(config.topic("weather"), json.dumps(w, ensure_ascii=False, separators=(",", ":")),
                           qos=1, retain=True)
            with open(os.path.join(STATE, "weather.json"), "w") as f:
                json.dump(w, f, ensure_ascii=False)
            log("weather", w["temp"], w["cond"], w.get("aqi"))
        except Exception as e:  # noqa: BLE001
            log("weather failed", e)
        time.sleep(WEATHER_EVERY_S)


# ------------------------------------------------------------------ http
HEX48 = re.compile(r"^/passport-audio/([0-9a-f]{48})$")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        m = HEX48.match(self.path)
        path = os.path.join(AUDIO, f"{m.group(1)}.pcm") if m else None
        if not path or not os.path.exists(path):
            self.send_error(404); return
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(path, "rb") as f:
            while chunk := f.read(4096):
                self.wfile.write(chunk)

    def log_message(self, *a):
        pass


def janitor():
    while True:
        now = time.time()
        for d, ttl in ((AUDIO, AUDIO_TTL_S), (CLIPS, CLIP_KEEP_S)):
            for n in os.listdir(d):
                p = os.path.join(d, n)
                if now - os.path.getmtime(p) > ttl:
                    os.remove(p)
        with clips_lock:
            for cid in [k for k, v in clips.items() if now - v["began"] > 600]:
                clips.pop(cid, None)
        time.sleep(300)


# ------------------------------------------------------------------ mqtt
def main():
    global _mqtt
    for d in (STATE, CLIPS, AUDIO):
        os.makedirs(d, exist_ok=True)
    client = config.mqtt_client(config.get("MQTT_CLIENT_ID", "passport-gateway"))
    _mqtt = client

    def on_connect(c, u, flags, rc, props=None):
        log("mqtt connected", rc)
        for t in ("status", "event", "audio"):
            c.subscribe(config.topic(t), qos=1)

    def on_message(c, u, msg):
        topic = msg.topic.rsplit("/", 1)[-1]
        try:
            if topic == "audio":
                on_audio(msg.payload)
            elif topic == "event":
                threading.Thread(target=on_event, args=(json.loads(msg.payload),), daemon=True).start()
            elif topic == "status":
                st = json.loads(msg.payload)
                st["seen"] = datetime.now(TZ).isoformat(timespec="seconds")
                with open(os.path.join(STATE, "status.json"), "w") as f:
                    json.dump(st, f, ensure_ascii=False)
        except Exception as e:  # noqa: BLE001
            log("message error", topic, e)

    client.on_connect, client.on_message = on_connect, on_message
    config.mqtt_connect(client, 60)
    threading.Thread(target=weather_loop, args=(client,), daemon=True).start()
    threading.Thread(target=janitor, daemon=True).start()
    threading.Thread(target=bingo.poll_loop, args=(publish_command, inject), daemon=True).start()
    srv = ThreadingHTTPServer((config.get("HTTP_BIND", "127.0.0.1"), config.get_int("HTTP_PORT", 8006)), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"gateway up: device={config.device_id()} state={STATE} "
        f"hook={'command' if HOOK_CMD else 'outbox ' + OUTBOX}")
    client.loop_forever(retry_first_connection=True)


if __name__ == "__main__":
    main()
