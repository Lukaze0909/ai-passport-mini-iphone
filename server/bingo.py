#!/usr/bin/env python3
"""bingo.py — the companion's side of the daily habit Bingo on the Passport.

A card day runs 06:00 -> 06:00 in PASSPORT_TZ. The first time a card is needed
(the device asks, a check-in lands, or the companion runs passportctl) the server
rolls it: the FIXED cells plus random ones from POOL, shuffled, 4x4 or 5x5 (the
owner's last choice). The task pool comes from BINGO_TASKS_FILE (see
examples/bingo_tasks.example.json). Labels are short ASCII (the device font is
12 px; a word must fit a 42 px cell, the label wraps at its space).

Sides: "owner" (the person with the device), "companion" (the agent), "auto" (check-ins).

Who does what:
  device -> gateway.on_event -> on_device(ev, publish, inject)
      bingo_get     today's card please (n = size for a new card)
      bingo_tick    owner's tick / untick {"cell":"WATER","on":true,"seq":17}
      bingo_reroll  swap one unticked, non-fixed cell (no duplicates, not one swapped away today)
      bingo_size    today's card again at another size (ticked + fixed cells kept)
      bingo_bingo   the owner's first line on the device: answered with the whisper
  server -> device: {"op":"bingo_card", day, n, cells, done/auto/fixed masks, ack, bingo[, whisper]}
      after every change; "ack" = the highest change number of the owner's applied. The whisper
      rides along only once the card has a line, so the device never holds it before.
  check-ins (optional, PASSPORT_CHECKINS_FILE: JSONL {"t": ISO time, "tag": "..."}, entries
      with a "ghost" key ignored): poll_checkins() every ~60 s (gateway thread) ticks the
      AUTO labels. Never unticks what the owner or the companion ticked; if the owner unticks
      an auto tick it stays off that day.
  companion -> passportctl bingo state|tick|untick|reroll|whisper|roll
  Events to the companion (kind passport.bingo): a new card (write today's whisper), the first line.
State: one JSON file (flock'd) in PASSPORT_STATE_DIR, last KEEP_DAYS cards.
"""
import fcntl, hashlib, json, os, random, re, time
from datetime import datetime, timedelta

import config

STATE_DIR = config.state_dir()
CHECKINS = config.path("PASSPORT_CHECKINS_FILE", "") if config.get("PASSPORT_CHECKINS_FILE") else ""
TASKS_FILE = config.path("BINGO_TASKS_FILE", os.path.join("examples", "bingo_tasks.example.json"))
TZ = config.tz()
CTL = config.ctl()
DAY_START_H = 6
KEEP_DAYS = 7
WHISPER_MAX = 80
LABEL_MAX = 8
WORD_MAX = 5
SIZES = (4, 5)
SLEPT_MIN_H, SLEPT_MAX_GAP_H = 7, 16      # a gap over 16 h means a missed scan, not sleep
AUTO_RULES = ("up_before_10", "bed_before_1", "slept_7h", "went_out")
OWNER, COMPANION = "owner", "companion"


class BingoError(Exception):
    pass


def _label_ok(lab: str) -> bool:
    return (0 < len(lab) <= LABEL_MAX and all(32 <= ord(c) < 127 and c not in ',"\\' for c in lab)
            and all(len(w) <= WORD_MAX for w in lab.split(" ")))


def load_tasks(path: str) -> dict:
    """The task file, checked against the device's limits."""
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    pool = d.get("pool") or {}
    fixed = list(d.get("fixed") or [])
    auto = dict(d.get("auto") or {})
    bad = [lab for lab in pool if not _label_ok(lab)]
    if bad:
        raise ValueError(f"{path}: labels too long or with bad characters: {bad}")
    for lab in fixed + list(auto):
        if lab not in pool:
            raise ValueError(f"{path}: {lab!r} is not in the pool")
    for lab, rule in auto.items():
        if rule not in AUTO_RULES:
            raise ValueError(f"{path}: unknown auto rule {rule!r} for {lab!r} (known: {', '.join(AUTO_RULES)})")
    if len(pool) < 30:
        raise ValueError(f"{path}: the pool needs at least 30 labels, has {len(pool)}")
    tags = {"up": "wake", "bed": "sleep", "out": "out", **(d.get("checkin_tags") or {})}
    whispers = tuple(clean_whisper(w) for w in (d.get("fallback_whispers") or []) if clean_whisper(w))
    return {"pool": pool, "fixed": tuple(fixed), "auto": auto, "tags": tags,
            "whispers": whispers or ("Bingo! A whole line. Nicely done.",)}


def clean_whisper(text: str) -> str:
    t = "".join(ch if 32 <= ord(ch) < 127 else " " for ch in (text or ""))
    return re.sub(r"\s+", " ", t).strip()[:WHISPER_MAX]


_TASKS = load_tasks(TASKS_FILE)
POOL = _TASKS["pool"]                     # label -> meaning (shown to the companion; the device shows the label)
FIXED = _TASKS["fixed"]
AUTO = tuple(_TASKS["auto"])              # labels ticked from check-ins
AUTO_RULE = _TASKS["auto"]                # label -> rule
CHECKIN_TAGS = _TASKS["tags"]
FALLBACK_WHISPERS = _TASKS["whispers"]

_rng = random.SystemRandom()


def _now() -> datetime:
    return datetime.now(TZ)


def _log(*a):
    print(time.strftime("%H:%M:%S"), "bingo:", *a, flush=True)


# ------------------------------------------------------------------ days
def card_day(dt: datetime | None = None) -> str:
    dt = (dt or _now()).astimezone(TZ)
    return (dt - timedelta(hours=DAY_START_H)).date().isoformat()


def day_start(day: str) -> datetime:
    y, m, d = (int(x) for x in day.split("-"))
    return datetime(y, m, d, DAY_START_H, tzinfo=TZ)


# ------------------------------------------------------------------ state
def _paths():
    return os.path.join(STATE_DIR, "bingo.json"), os.path.join(STATE_DIR, "bingo.lock")


class Store:
    """with Store() as st: ... — read-modify-write under an exclusive flock."""

    def __enter__(self):
        path, lock = _paths()
        os.makedirs(STATE_DIR, exist_ok=True)
        self._lf = open(lock, "a")
        fcntl.flock(self._lf, fcntl.LOCK_EX)
        self.path = path
        try:
            with open(path) as f:
                self.data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            self.data = {}
        self.data.setdefault("size", 4)
        self.data.setdefault("ack", 0)
        self.data.setdefault("cards", {})
        return self

    def __exit__(self, et, ev, tb):
        try:
            if et is None:
                tmp = self.path + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(self.data, f, ensure_ascii=False, indent=1)
                os.chmod(tmp, 0o600)
                os.replace(tmp, self.path)
        finally:
            fcntl.flock(self._lf, fcntl.LOCK_UN)
            self._lf.close()
        return False


# ------------------------------------------------------------------ cards
def _stamp() -> str:
    return _now().strftime("%Y-%m-%dT%H:%M:%S")


def _draw(n: int, keep, exclude) -> list:
    """FIXED + keep (in that order, no repeats), filled from POOL avoiding `exclude`
    when it can, shuffled. Never a duplicate."""
    cells = []
    for lab in list(FIXED) + list(keep):
        if lab not in cells and len(cells) < n * n:
            cells.append(lab)
    rest = [lab for lab in POOL if lab not in cells and lab not in exclude]
    if len(rest) < n * n - len(cells):
        rest = [lab for lab in POOL if lab not in cells]
    cells += _rng.sample(rest, n * n - len(cells))
    _rng.shuffle(cells)
    return cells


def _new_card(day: str, n: int) -> dict:
    return {"day": day, "n": n, "cells": _draw(n, [], ()), "ticks": {}, "seq": {}, "veto": [],
            "swapped": [], "whisper": None, "whisper_by": None, "bingo": None, "created": _stamp()}


def _ensure(st, day: str, n: int | None, after) -> dict:
    cards = st.data["cards"]
    card = cards.get(day)
    if card:
        return card
    n = n if n in SIZES else st.data["size"]
    card = cards[day] = _new_card(day, n)
    for old in sorted(cards)[:-KEEP_DAYS]:
        cards.pop(old, None)
    _log(f"new card {day} {n}x{n}: {', '.join(card['cells'])}")
    after.append(("passport.bingo", {
        "hint": (f"Today's Bingo card ({day}, 06:00 to 06:00, {n}x{n}) has been rolled for the owner. "
                 f"Write today's hidden whisper: `{CTL} bingo whisper \"English text\"` (printable ASCII, "
                 f"<= {WHISPER_MAX} chars). It stays on the server and appears on the device only when the "
                 f"owner completes their first line. Ticks do not wake you; you get another event at the "
                 f"first line. To see the card: `{CTL} bingo state`."),
        "day": day, "size": n, "cells": card["cells"],
        "meanings": {lab: POOL.get(lab, "") for lab in card["cells"]}}))
    return card


def _today(st, after, n=None) -> dict:
    return _ensure(st, card_day(), n, after)


def _ticked_idx(card) -> set:
    return {i for i, lab in enumerate(card["cells"]) if lab in card["ticks"]}


def lines_of(ticked: set, n: int) -> list:
    """Complete lines: [("row 2", [indices]), ...] in the device's order."""
    cand = [(f"row {r + 1}", [r * n + c for c in range(n)]) for r in range(n)]
    cand += [(f"col {c + 1}", [r * n + c for r in range(n)]) for c in range(n)]
    cand += [("diag", [i * n + i for i in range(n)]), ("anti", [i * n + n - 1 - i for i in range(n)])]
    return [(name, idx) for name, idx in cand if all(i in ticked for i in idx)]


def _mask(card, pred) -> int:
    return sum(1 << i for i, lab in enumerate(card["cells"]) if pred(lab))


def card_cmd(st, card) -> dict:
    cmd = {"op": "bingo_card", "day": card["day"], "n": card["n"], "cells": ",".join(card["cells"]),
           "done": _mask(card, lambda lab: lab in card["ticks"]),
           "auto": _mask(card, lambda lab: card["ticks"].get(lab, {}).get("src") == "auto"),
           "fixed": _mask(card, lambda lab: lab in FIXED),
           "ack": st.data["ack"], "bingo": bool(card["bingo"])}
    if card["bingo"]:
        cmd["whisper"] = card["whisper"] or ""
    return cmd


def _fallback(day: str) -> str:
    return FALLBACK_WHISPERS[int(hashlib.sha256(day.encode()).hexdigest(), 16) % len(FALLBACK_WHISPERS)]


def grid_text(card) -> str:
    n, w = card["n"], max(len(lab) for lab in card["cells"]) + 2
    rows = []
    for r in range(n):
        cells = []
        for c in range(n):
            lab = card["cells"][r * n + c]
            t = card["ticks"].get(lab)
            mark = "  " if not t else {OWNER: "v ", "auto": "a ", COMPANION: "c "}.get(t["src"], "v ")
            cells.append((mark + lab).ljust(w + 2))
        rows.append(" | ".join(cells))
    return "\n".join(rows) + "\n(v ticked by the owner, a from a check-in, c ticked by you; fixed: " + ", ".join(
        lab for lab in card["cells"] if lab in FIXED) + ")"


def _changed(st, card, publish, after, why: str):
    """After any change to a card: the first line reveals the whisper (and tells
    the companion); then the whole card goes to the device."""
    lines = lines_of(_ticked_idx(card), card["n"])
    if lines and not card["bingo"]:
        name, idx = lines[0]
        labs = [card["cells"][i] for i in idx]
        if not card["whisper"]:
            card["whisper"], card["whisper_by"] = _fallback(card["day"]), "fallback"
        card["bingo"] = {"t": _stamp(), "line": name, "cells": labs, "by": why}
        _log(f"{card['day']} BINGO {name}: {labs} ({why}); whisper by {card['whisper_by']}")
        fb = card["whisper_by"] == "fallback"
        after.append(("passport.bingo", {
            "hint": (f"The owner completed their first Bingo line ({name}: {', '.join(labs)})! "
                     f"The device now shows BINGO! and {'a fallback' if fb else 'your'} whisper: "
                     f"\"{card['whisper']}\". "
                     + (f"You had not written one today; `{CTL} bingo whisper \"...\"` replaces it "
                        "(pushed to the device's note at once). " if fb else "")
                     + "Cheer them on through your usual channel."),
            "day": card["day"], "line": name, "cells": labs, "whisper": card["whisper"],
            "whisper_by": card["whisper_by"], "how": why, "grid": grid_text(card)}))
    elif card["bingo"] and not lines:
        _log(f"{card['day']}: no full line any more (the whisper stays revealed)")
    publish(card_cmd(st, card))


def _publish_today(st, publish, fallback_card=None):
    card = st.data["cards"].get(card_day()) or fallback_card
    if card:
        publish(card_cmd(st, card))


def _find(card, label: str) -> str:
    want = re.sub(r"\s+", " ", (label or "").strip()).upper()
    for lab in card["cells"]:
        if lab.upper() == want:
            return lab
    raise BingoError(f"{label!r} is not on the {card['day']} card: {', '.join(card['cells'])}")


def _set(card, lab: str, on: bool, src: str):
    if on:
        if lab not in card["ticks"] or src != "auto":
            card["ticks"][lab] = {"src": src, "t": _stamp()}
        if src != "auto" and lab in card["veto"]:
            card["veto"].remove(lab)
    else:
        t = card["ticks"].pop(lab, None)
        if src == OWNER and lab in AUTO and lab not in card["veto"] and t and t["src"] == "auto":
            card["veto"].append(lab)        # the owner disagreed with the check-in: not again today


def _reroll(card, lab: str) -> str:
    if lab in FIXED:
        raise BingoError(f"{lab} is fixed (always on the card)")
    if lab in card["ticks"]:
        raise BingoError(f"{lab} is ticked; only unticked cells can be swapped")
    pick = [x for x in POOL if x not in card["cells"] and x not in card["swapped"] and x != lab]
    if not pick:
        pick = [x for x in POOL if x not in card["cells"] and x != lab]
    if not pick:
        raise BingoError("the pool is empty")
    new = _rng.choice(pick)
    card["cells"][card["cells"].index(lab)] = new
    card["swapped"].append(lab)
    card["seq"].pop(lab, None)
    return new


def _resize(card, n: int):
    """Same day, new size: fixed and ticked labels stay (ticks too), the rest is drawn again."""
    keep = [lab for lab in card["cells"] if lab in card["ticks"]]
    card["cells"] = _draw(n, keep, set(card["swapped"]))
    card["n"] = n
    for lab in list(card["ticks"]):
        if lab not in card["cells"]:
            card["ticks"].pop(lab)


def _seq_ok(st, card, lab, seq) -> bool:
    """The owner's change numbers: an older one for the same cell (a late re-send) is ignored."""
    if not isinstance(seq, int) or seq <= 0:
        return True
    st.data["ack"] = max(st.data["ack"], seq)
    if seq <= card["seq"].get(lab, 0):
        return False
    card["seq"][lab] = seq
    return True


# ------------------------------------------------------------------ device -> here
def on_device(ev: dict, publish, inject):
    """Gateway hook. publish(dict) sends a command to the device (bool);
    inject(kind, payload) hands an event to the companion."""
    t = ev.get("type")
    after = []
    with Store() as st:
        today = card_day()
        day = ev.get("day") if isinstance(ev.get("day"), str) else ""
        seq = ev.get("seq")
        if t == "bingo_get":
            n = ev.get("n") if ev.get("n") in SIZES else None
            if n and today not in st.data["cards"]:
                st.data["size"] = n
            card = _today(st, after, n)
            _apply_auto(st, card, _read_checkins(), publish, after, publish_unchanged=True)
        elif t in ("bingo_tick", "bingo_reroll", "bingo_size", "bingo_bingo"):
            card = st.data["cards"].get(day or today)
            if card is None:
                if (day or today) != today:
                    _log(f"{t} for {day}: no such card (ack {seq})")
                    if isinstance(seq, int):
                        st.data["ack"] = max(st.data["ack"], seq)
                    _publish_today(st, publish)
                    return
                card = _today(st, after)
            if t == "bingo_tick":
                try:
                    lab = _find(card, ev.get("cell"))
                except BingoError as e:
                    _log("tick:", e)
                    if isinstance(seq, int):
                        st.data["ack"] = max(st.data["ack"], seq)
                    _publish_today(st, publish, card)
                    return
                if _seq_ok(st, card, lab, seq):
                    _set(card, lab, bool(ev.get("on")), OWNER)
                    _log(f"{card['day']} {lab} {'ticked' if ev.get('on') else 'unticked'} by the owner (seq {seq})")
                _changed(st, card, publish, after, OWNER)
            elif t == "bingo_reroll":
                if isinstance(seq, int):
                    st.data["ack"] = max(st.data["ack"], seq)
                try:
                    lab = _find(card, ev.get("cell"))
                    new = _reroll(card, lab)
                    _log(f"{card['day']} {lab} swapped for {new} by the owner")
                except BingoError as e:
                    _log("reroll:", e)
                _changed(st, card, publish, after, OWNER)
            elif t == "bingo_size":
                if isinstance(seq, int):
                    st.data["ack"] = max(st.data["ack"], seq)
                n = ev.get("n")
                if n in SIZES:
                    st.data["size"] = n
                    if n != card["n"]:
                        _resize(card, n)
                        _apply_auto(st, card, _read_checkins(), lambda c: True, after)
                        _log(f"{card['day']} now {n}x{n}: {', '.join(card['cells'])}")
                _changed(st, card, publish, after, OWNER)
            else:   # bingo_bingo: the owner's line on the device; its cells are ticked there
                labs = [x for x in (ev.get("cells") or "").split(",") if x]
                for x in labs:
                    if x in card["cells"] and x not in card["ticks"]:
                        _set(card, x, True, OWNER)
                _log(f"{card['day']} device says BINGO {ev.get('line')}: {labs}")
                _changed(st, card, publish, after, OWNER)
        else:
            _log("unknown", ev)
            return
    for kind, payload in after:
        inject(kind, payload)


# ------------------------------------------------------------------ check-ins
def _read_checkins(path: str | None = None) -> list:
    """[(aware datetime, tag)], ghosts and broken lines skipped, oldest first."""
    out = []
    path = path or CHECKINS
    if not path:
        return out
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    e = json.loads(line)
                    if not isinstance(e, dict) or "ghost" in e or not isinstance(e.get("tag"), str):
                        continue
                    dt = datetime.fromisoformat(e["t"])
                    out.append((dt if dt.tzinfo else dt.replace(tzinfo=TZ), e["tag"]))
                except (ValueError, KeyError, TypeError):
                    continue
    except FileNotFoundError:
        pass
    out.sort(key=lambda x: x[0])
    return out


def auto_rules_hit(checkins: list, day: str) -> set:
    """Rules (AUTO_RULES) earned on card `day` (06:00 -> 06:00)."""
    up_tag, bed_tag, out_tag = CHECKIN_TAGS["up"], CHECKIN_TAGS["bed"], CHECKIN_TAGS["out"]
    s = day_start(day)
    e = s + timedelta(days=1)
    hits = set()
    ups = [t for t, tag in checkins if tag == up_tag and s <= t < e]
    if any(t < s.replace(hour=10) for t in ups):
        hits.add("up_before_10")
    if any(tag == bed_tag and s.replace(hour=18) <= t < s.replace(hour=1) + timedelta(days=1)
           for t, tag in checkins):
        hits.add("bed_before_1")
    if ups:
        q = ups[0]
        beds = [t for t, tag in checkins if tag == bed_tag and t < q]
        if beds and timedelta(hours=SLEPT_MIN_H) <= q - beds[-1] <= timedelta(hours=SLEPT_MAX_GAP_H):
            hits.add("slept_7h")
    if any(tag == out_tag and s <= t < e for t, tag in checkins):
        hits.add("went_out")
    return hits


def auto_hits(checkins: list, day: str) -> set:
    """AUTO labels earned on card `day`."""
    rules = auto_rules_hit(checkins, day)
    return {lab for lab, rule in AUTO_RULE.items() if rule in rules}


def _apply_auto(st, card, checkins, publish, after, publish_unchanged=False) -> bool:
    hits = auto_hits(checkins, card["day"])
    changed = False
    for lab in AUTO:
        if lab not in card["cells"]:
            continue
        t = card["ticks"].get(lab)
        if lab in hits and not t and lab not in card["veto"]:
            _set(card, lab, True, "auto")
            changed = True
            _log(f"{card['day']} {lab} ticked from check-ins")
        elif lab not in hits and t and t["src"] == "auto":
            card["ticks"].pop(lab)                  # its check-in was marked ghost
            changed = True
            _log(f"{card['day']} {lab} auto tick withdrawn")
    if changed:
        _changed(st, card, publish, after, "auto")
    elif publish_unchanged:
        publish(card_cmd(st, card))
    return changed


_sig = None


def poll_checkins(publish, inject, force=False) -> bool:
    """Gateway thread, ~every 60 s: does nothing unless the check-in file changed.
    A change makes sure today's card exists and applies the AUTO rules to it
    (and to the previous card day's card, for a late BED<1)."""
    global _sig
    if not CHECKINS:
        return False
    try:
        stt = os.stat(CHECKINS)
        sig = (stt.st_size, stt.st_mtime_ns)
    except FileNotFoundError:
        return False
    if sig == _sig and not force:
        return False
    _sig = sig
    checkins = _read_checkins()
    after, changed = [], False
    with Store() as st:
        today = card_day()
        _today(st, after)
        prev = (day_start(today) - timedelta(days=1)).date().isoformat()
        for day in (prev, today):
            card = st.data["cards"].get(day)
            if card:
                changed |= _apply_auto(st, card, checkins, publish, after)
    for kind, payload in after:
        inject(kind, payload)
    return changed


def poll_loop(publish, inject, every_s=60):   # gateway thread
    if not CHECKINS:
        _log("check-ins disabled (PASSPORT_CHECKINS_FILE not set)")
        return
    while True:
        try:
            poll_checkins(publish, inject)
        except Exception as e:  # noqa: BLE001
            _log("poll failed", e)
        time.sleep(every_s)


# ------------------------------------------------------------------ companion -> device
def _view(st, card, extra=None) -> dict:
    lines = lines_of(_ticked_idx(card), card["n"])
    out = {"day": card["day"], "size": f"{card['n']}x{card['n']}",
           "ticked": f"{len(card['ticks'])}/{card['n'] ** 2}",
           "ticks": {lab: t["src"] for lab, t in card["ticks"].items()},
           "lines": [name for name, _ in lines], "bingo": card["bingo"],
           "whisper": card["whisper"], "whisper_by": card["whisper_by"],
           "hidden_from_owner": not card["bingo"], "swapped_today": card["swapped"],
           "vetoed_auto": card["veto"], "grid": grid_text(card)}
    out.update(extra or {})
    return out


def _events(out: dict, after: list) -> dict:
    """passportctl runs as the companion itself: events go into the output, not the hook."""
    if after:
        out["events"] = [p["hint"] for _, p in after]
    return out


def cmd_state(publish=None) -> dict:
    after = []
    with Store() as st:
        card = _today(st, after)
        if after and publish:
            publish(card_cmd(st, card))         # just rolled: the device gets it now
        return _events(_view(st, card, {"meanings": {lab: POOL.get(lab, "") for lab in card["cells"]}}), after)


def cmd_tick(label: str, on: bool, publish) -> dict:
    after = []
    with Store() as st:
        card = _today(st, after)
        lab = _find(card, label)
        _set(card, lab, on, COMPANION)
        _changed(st, card, publish, after, COMPANION)
        return _events(_view(st, card, {"done": f"{lab} {'ticked' if on else 'unticked'}"}), after)


def cmd_reroll(label: str, publish) -> dict:
    after = []
    with Store() as st:
        card = _today(st, after)
        lab = _find(card, label)
        new = _reroll(card, lab)
        _changed(st, card, publish, after, COMPANION)
        return _events(_view(st, card, {"done": f"{lab} -> {new}"}), after)


def cmd_whisper(text: str, publish) -> dict:
    clean = clean_whisper(text)
    if not clean:
        raise BingoError("empty whisper (ASCII English only)")
    after = []
    with Store() as st:
        card = _today(st, after)
        card["whisper"], card["whisper_by"] = clean, COMPANION
        if card["bingo"] or after:
            publish(card_cmd(st, card))         # revealed already: the device's note shows the new one
        out = {"day": card["day"], "whisper": clean, "chars": len(clean),
               "revealed_now": bool(card["bingo"])}
        if clean != re.sub(r"\s+", " ", (text or "")).strip():
            out["changed"] = f"the device shows printable ASCII only, <= {WHISPER_MAX} chars"
        return out


def cmd_roll(size, publish) -> dict:
    """Re-roll today's card (ticked + fixed cells stay), optionally at a new size."""
    after = []
    with Store() as st:
        card = _today(st, after, size)
        n = size if size in SIZES else card["n"]
        if size in SIZES:
            st.data["size"] = size
        _resize(card, n)
        _changed(st, card, publish, after, COMPANION)
        return _events(_view(st, card, {"done": f"rolled {n}x{n}"}), after)
