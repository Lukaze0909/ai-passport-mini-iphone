#!/usr/bin/env python3
"""passportctl — the companion's hand on the Passport.

  passportctl say "text" [caption]   TTS -> 16 kHz audio -> the device downloads and plays it
  passportctl msg "English text"     text bubble in the Messages app (ASCII only, <=280 chars)
  passportctl status                 latest device status + weather
  passportctl clips                  recent voice clips from the device
  passportctl mine state             minesweeper: current game, both fields from your side, score
  passportctl mine layout A2,C1      choose your mines (exact count for the size); commits sha256
  passportctl mine layout --random   ...or let the dice choose
  passportctl mine step C3 "thought" step on the owner's field, with a one-line English thought (<=60 chars)
  passportctl mine reveal            re-send layout + salt after the end (sent automatically)
  passportctl mine reset             forget the current game (the score stays)
  passportctl bingo state            Bingo: today's card (06:00 day), ticks and sources, whisper
  passportctl bingo tick LABEL       tick a cell for the owner (e.g. "WATER"; case-insensitive)
  passportctl bingo untick LABEL
  passportctl bingo reroll LABEL     swap one unticked cell for another from the pool
  passportctl bingo whisper "text"   today's hidden whisper (English/ASCII, <=80), shown at the first line
  passportctl bingo roll [4|5]       re-roll today's card (ticked + fixed cells stay), optionally resize

Settings come from the environment / .env (see .env.example).
"""
import json, os, secrets, shlex, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import config  # noqa: E402

STATE = config.state_dir()
AUDIO = os.path.join(STATE, "audio")


# ------------------------------------------------------------------ IMA ADPCM (download)
# Block = [int16 LE predictor][uint8 index][uint8 0] + 512 bytes (1024 samples, low nibble first).
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
        66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
        408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
        7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]
BLOCK_SAMPLES = 1024


def adpcm_encode(pcm: bytes) -> bytes:
    import array
    smp = array.array("h"); smp.frombytes(pcm[: len(pcm) & ~1])
    out = bytearray(); pred, idx = 0, 0
    for b in range(0, len(smp), BLOCK_SAMPLES):
        blk = smp[b:b + BLOCK_SAMPLES]
        if len(blk) & 1: blk.append(blk[-1])
        out += struct.pack("<hBB", pred, idx, 0)
        nibs = []
        for x in blk:
            step = STEP[idx]; diff = x - pred; nib = 0
            if diff < 0: nib = 8; diff = -diff
            q = step >> 3
            if diff >= step: nib |= 4; diff -= step; q += step
            if diff >= step >> 1: nib |= 2; diff -= step >> 1; q += step >> 1
            if diff >= step >> 2: nib |= 1; q += step >> 2
            pred = pred - q if nib & 8 else pred + q
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IDX[nib & 7]))
            nibs.append(nib)
        for i in range(0, len(nibs), 2):
            out.append(nibs[i] | (nibs[i + 1] << 4))
    return bytes(out)


def download_via():
    """How the device fetches `say` audio: "http" (from AUDIO_ORIGIN) or "mqtt" (chunks on <dev>/playchunk)."""
    return config.get("PASSPORT_AUDIO_VIA", "mqtt").lower()


CHUNK_BLOCKS = 3          # 3 x 516 B = 1548 B body (+12 B header): fits the device 2048 B MQTT rx buffer unfragmented
BLOCK_BYTES = 4 + BLOCK_SAMPLES // 2


def publish_play_mqtt(cmd, data):
    """Command first, then ADPCM chunks on <dev>/playchunk, QoS1, paced ~1.5x real time."""
    step = CHUNK_BLOCKS * BLOCK_BYTES
    bodies = [data[i:i + step] for i in range(0, len(data), step)]
    cmd = dict(cmd, via="mqtt", chunks=len(bodies), bytes=len(data))
    c = config.mqtt_client(f"passportctl-{os.getpid()}")
    config.mqtt_connect(c, 30)
    c.loop_start()
    ok = True
    try:
        info = c.publish(config.topic("command"),
                         json.dumps(cmd, ensure_ascii=False, separators=(",", ":")), qos=1)
        info.wait_for_publish(10); ok = info.is_published()
        sid = cmd["id"][:8].encode("ascii")
        chunk_s = CHUNK_BLOCKS * BLOCK_SAMPLES / 16000 / 1.5
        t0 = time.monotonic()
        for seq, body in enumerate(bodies):
            if seq >= 2:                       # first two go out at once for the pre-buffer
                wait = t0 + (seq - 1) * chunk_s - time.monotonic()
                if wait > 0: time.sleep(wait)
            info = c.publish(config.topic("playchunk"),
                             sid + struct.pack("<HH", seq, len(bodies)) + body, qos=1)
            info.wait_for_publish(10)
            ok = ok and info.is_published()
    finally:
        c.loop_stop(); c.disconnect()
    return ok, len(bodies)


def download_fmt():
    """Audio format the device firmware expects: "adpcm" (IMA ADPCM blocks) or "pcm" (s16le)."""
    return config.get("PASSPORT_AUDIO_FMT", "adpcm").lower()


def publish(topic, payload):
    c = config.mqtt_client(f"passportctl-{os.getpid()}")
    config.mqtt_connect(c, 30)
    c.loop_start()
    info = c.publish(topic, payload, qos=1)
    info.wait_for_publish(10)
    ok = info.is_published()
    c.loop_stop(); c.disconnect()
    return ok


def send_command(cmd: dict) -> bool:
    return publish(config.topic("command"), json.dumps(cmd, ensure_ascii=True, separators=(",", ":")))


def say(text, caption=None):
    os.makedirs(AUDIO, exist_ok=True)
    tts = config.get("TTS_CMD") or f"node {os.path.join(config.HERE, 'passport_tts.mjs')}"
    with tempfile.TemporaryDirectory() as td:
        mp3 = os.path.join(td, "s.mp3")
        r = subprocess.run(shlex.split(tts) + [text, mp3], capture_output=True, text=True, timeout=180)
        if r.returncode != 0:
            sys.exit(f"TTS failed: {r.stderr.strip()[-300:]}")
        token = secrets.token_hex(24)
        pcm = f"{AUDIO}/{token}.pcm"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", mp3, "-ac", "1", "-ar", "16000",
                        "-af", "loudnorm=I=-13:TP=-1.0", "-f", "s16le", pcm], check=True, timeout=120)
        os.chmod(pcm, 0o600)
    secs = os.path.getsize(pcm) / 32000
    fmt = download_fmt()
    if fmt == "adpcm":
        data = adpcm_encode(open(pcm, "rb").read())
        with open(pcm + ".tmp", "wb") as f:
            f.write(data)
        os.chmod(pcm + ".tmp", 0o600)
        os.replace(pcm + ".tmp", pcm)        # gateway serves <token>.pcm whatever the format
    origin = config.get("AUDIO_ORIGIN")
    use_mqtt = fmt == "adpcm" and download_via() == "mqtt"
    if not use_mqtt and not origin:
        sys.exit("say: HTTP download needs AUDIO_ORIGIN (public URL prefix ending in /passport-audio/)")
    cmd = {"op": "play", "url": origin + token, "id": token[:12],
           "text": caption if caption is not None else text[:60]}
    if fmt == "adpcm":
        cmd["fmt"] = "adpcm"
    if use_mqtt:
        # The device refuses a new clip while one is still playing: wait for the previous one.
        busy = os.path.join(STATE, "play_until")
        try:
            wait = float(open(busy).read()) - time.time()
            if 0 < wait < 120: time.sleep(wait)
        except (FileNotFoundError, ValueError):
            pass
        t_start = time.time()
        ok, n = publish_play_mqtt(cmd, open(pcm, "rb").read())
        with open(busy, "w") as f:
            f.write(str(t_start + secs + 1.5))
        print(json.dumps({"published": ok, "seconds": round(secs, 1), "id": cmd["id"],
                          "via": "mqtt", "chunks": n}, ensure_ascii=False))
        return
    ok = publish(config.topic("command"), json.dumps(cmd, ensure_ascii=False, separators=(",", ":")))
    print(json.dumps({"published": ok, "seconds": round(secs, 1), "id": cmd["id"]}, ensure_ascii=False))


def msg(text):
    """Text message to the Passport Messages app (English/ASCII only; device shows printable ASCII)."""
    clean = "".join(ch if 32 <= ord(ch) < 127 else " " for ch in text).strip()[:280]
    cmd = {"op": "msg", "id": secrets.token_hex(6), "text": clean, "ts": int(time.time())}
    ok = send_command(cmd)
    print(json.dumps({"published": ok, "id": cmd["id"], "chars": len(clean),
                      "dropped_non_ascii": clean != text.strip()[:280]}))


def status():
    for name in ("status.json", "weather.json"):
        p = os.path.join(STATE, name)
        print(name, open(p).read() if os.path.exists(p) else "(none yet)")


def clips():
    d = os.path.join(STATE, "clips")
    for n in sorted(os.listdir(d) if os.path.isdir(d) else [])[-10:]:
        print(os.path.join(d, n))


def mine(args):
    """Minesweeper (see mines.py)."""
    import mines  # noqa: E402
    pub = send_command

    try:
        if args[:1] == ["state"] and len(args) == 1:
            out = mines.cmd_state()
        elif args[:1] == ["layout"] and len(args) == 2:
            out = mines.cmd_layout(args[1], pub)
        elif args[:1] == ["step"] and len(args) in (2, 3):
            out = mines.cmd_step(args[1], args[2] if len(args) == 3 else "", pub)
        elif args[:1] == ["reveal"] and len(args) == 1:
            out = mines.cmd_reveal(pub)
        elif args[:1] == ["reset"] and len(args) == 1:
            out = mines.cmd_reset()
        else:
            print(__doc__); sys.exit(2)
    except mines.MinesError as e:
        sys.exit(f"mine: {e}")
    board = out.pop("board", None)
    print(json.dumps(out, ensure_ascii=False))
    if board:
        print(board)


def bingo_cmd(args):
    """Bingo (see bingo.py)."""
    import bingo  # noqa: E402
    pub = send_command

    try:
        if args[:1] == ["state"] and len(args) == 1:
            out = bingo.cmd_state(pub)
        elif args[:1] in (["tick"], ["untick"]) and len(args) >= 2:
            out = bingo.cmd_tick(" ".join(args[1:]), args[0] == "tick", pub)
        elif args[:1] == ["reroll"] and len(args) >= 2:
            out = bingo.cmd_reroll(" ".join(args[1:]), pub)
        elif args[:1] == ["whisper"] and len(args) == 2:
            out = bingo.cmd_whisper(args[1], pub)
        elif args[:1] == ["roll"] and len(args) in (1, 2) and (len(args) == 1 or args[1] in ("4", "5")):
            out = bingo.cmd_roll(int(args[1]) if len(args) == 2 else None, pub)
        else:
            print(__doc__); sys.exit(2)
    except bingo.BingoError as e:
        sys.exit(f"bingo: {e}")
    grid = out.pop("grid", None)
    print(json.dumps(out, ensure_ascii=False))
    if grid:
        print(grid)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "mine":
        mine(sys.argv[2:]); sys.exit(0)
    if len(sys.argv) >= 2 and sys.argv[1] == "bingo":
        bingo_cmd(sys.argv[2:]); sys.exit(0)
    if len(sys.argv) >= 3 and sys.argv[1] == "say":
        say(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    elif len(sys.argv) == 3 and sys.argv[1] == "msg":
        msg(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == "status":
        status()
    elif len(sys.argv) == 2 and sys.argv[1] == "clips":
        clips()
    else:
        print(__doc__); sys.exit(2)
