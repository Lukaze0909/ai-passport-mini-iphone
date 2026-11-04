#!/usr/bin/env python3
"""mines.py — the companion's side of mutual minesweeper on the Passport.

Rules: two N×N fields (3×3/2, 4×4/3, 5×5/4 mines), no neighbour numbers. The owner
hides their mines on the device (they never leave it during play; the device judges
the companion's steps and reports only safe/boom). The companion chooses its own
layout; before play the device gets commit = sha256(layout + salt)[:16]; at the end
the device gets layout + salt and re-checks. First to step on the other's mine
loses; clearing all of the other's safe cells wins. Turns alternate.

Sides are "owner" (the person holding the device) and "companion" (the agent).

Who does what here:
  device -> gateway.on_event -> on_device(ev, publish, inject)
      mine_new               new game {size, first: "owner"|"companion"}; ask the companion
                             (event) to choose its layout
      mine_ready             the owner's mines are hidden; if the companion starts and has
                             committed: its turn (event)
      mine_step              the owner's step on the companion's field: answered here
                             automatically (no companion needed), then "your turn" (event)
                             or the end
      mine_companion_result  the device's verdict on the companion's step
                             {cell, boom, winner: "owner"|"companion"|""}; the end if fatal / last
      mine_quit              the owner left the game
      mine_owner_reveal      the owner's layout, sent by the device only after the game (end,
                             or a quit after a step); kept in the record, shown to the companion,
                             checked against the device's verdicts
  server -> device: mine_commit {commit}, mine_result {cell, boom},
                    mine_companion_step {cell, note}, mine_reveal {layout, salt}
  companion -> passportctl mine ... -> cmd_layout / cmd_step / cmd_state / cmd_reveal / cmd_reset
State: one JSON file (flock'd) in PASSPORT_STATE_DIR. Coordinates: rows A.., columns 1.. ("B2").
"""
import fcntl, json, os, re, secrets, time
import hashlib

import config

STATE_DIR = config.state_dir()
SIZES = {3: 2, 4: 3, 5: 4}
NOTE_MAX = 60
OWNER, COMPANION = "owner", "companion"
# The end event waits up to this long for the owner's layout (mine_owner_reveal follows
# the final result over MQTT; the gateway handles events in parallel threads).
END_WAIT_S = config.get_float("PASSPORT_MINES_END_WAIT_S", 3.0)
CTL = config.ctl()


class MinesError(Exception):
    pass


# ------------------------------------------------------------------ cells
def cell_name(i: int, n: int) -> str:
    return f"{chr(65 + i // n)}{i % n + 1}"


def parse_cell(s: str, n: int) -> int:
    m = re.fullmatch(r"\s*([A-Za-z])\s*([1-9])\s*", s or "")
    if not m:
        raise MinesError(f"bad cell {s!r} (use e.g. B2)")
    r, c = ord(m.group(1).upper()) - 65, int(m.group(2)) - 1
    if not (0 <= r < n and 0 <= c < n):
        raise MinesError(f"{s} is off the {n}x{n} field")
    return r * n + c


def layout_str(cells, n: int) -> str:
    """Canonical layout string: sorted, comma separated ("A2,C1"). This exact
    string (plus the salt) is what gets hashed and revealed."""
    return ",".join(cell_name(i, n) for i in sorted(cells))


def commit_of(layout: str, salt: str) -> str:
    return hashlib.sha256((layout + salt).encode("ascii")).hexdigest()[:16]


def clean_note(text: str) -> str:
    t = "".join(ch if 32 <= ord(ch) < 127 else " " for ch in (text or ""))
    return re.sub(r"\s+", " ", t).strip()[:NOTE_MAX]


# ------------------------------------------------------------------ state
def _paths():
    return os.path.join(STATE_DIR, "mines.json"), os.path.join(STATE_DIR, "mines.lock")


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
        self.data.setdefault("score", {OWNER: 0, COMPANION: 0})
        self.data.setdefault("game", None)
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


def _log(*a):
    print(time.strftime("%H:%M:%S"), "mines:", *a, flush=True)


# ------------------------------------------------------------------ views for the companion
def _grid(g, mine_cells, hit_cells, mark_hit):
    n = g["n"]
    rows = ["   " + " ".join(str(c + 1) for c in range(n))]
    for r in range(n):
        line = []
        for c in range(n):
            i = r * n + c
            line.append("X" if i in mine_cells and i in hit_cells else
                        "M" if i in mine_cells else mark_hit if i in hit_cells else ".")
        rows.append(f" {chr(65 + r)} " + " ".join(line))
    return "\n".join(rows)


def _owner_mines(g):
    """The owner's mines once the game is over and the device sent them, else an empty set."""
    if not g.get("over") or not g.get("owner_layout"):
        return set()
    return set(parse_cell(c, g["n"]) for c in g["owner_layout"].split(","))


def board_text(g) -> str:
    """The companion's view: its own field (M = its mine, o = the owner's safe step)
    and the owner's (f = the companion's footprint, X = where a mine went off,
    m = the owner's mine once revealed after the game; unknown before)."""
    n = g["n"]
    mine = set(parse_cell(c, n) for c in (g["layout"].split(",") if g.get("layout") else []))
    theirs = set(parse_cell(s["cell"], n) for s in g["owner_steps"])
    mine_hit = [parse_cell(s["cell"], n) for s in g["owner_steps"] if s.get("boom")]
    mine_view = _grid(g, mine, theirs, "o")
    csteps = [s for s in g["companion_steps"] if s.get("boom") is not None]
    foot = set(parse_cell(s["cell"], n) for s in csteps)
    boom = set(parse_cell(s["cell"], n) for s in csteps if s["boom"])
    owner = _owner_mines(g)
    owner_view = _grid(g, boom | owner, foot, "f").replace("M", "m")
    left = [cell_name(i, n) for i in range(n * n) if i not in foot]
    return (f"{n}x{n}, {g['m']} mines each. Score owner {g.get('score_owner', 0)} : "
            f"{g.get('score_companion', 0)} you\n"
            f"YOUR field (M your mine, o owner stepped safe{', X owner hit' if mine_hit else ''}):\n{mine_view}\n"
            f"OWNER's field (f your footprint, X you hit a mine{', m owner mine' if owner else ''}):\n{owner_view}\n"
            f"owner cells you have not stepped on: {' '.join(left)}")


def _public(g):
    """State for passportctl (the companion may see its own layout; the owner's
    layout is only stored after the game)."""
    if not g:
        return {"game": None}
    out = {k: v for k, v in g.items() if k != "salt"}
    if not g.get("over"):
        out.pop("owner_layout", None)
        out.pop("owner_layout_consistent", None)
    out["board"] = board_text(g)
    return out


# ------------------------------------------------------------------ device -> here
def on_device(ev: dict, publish, inject):
    """Gateway hook. publish(dict) sends a command to the device (bool);
    inject(kind, payload) hands an event to the companion."""
    t = ev.get("type") or ev.get("op")
    game = ev.get("game")
    if not isinstance(game, int):
        _log("ignored (no game id)", ev)
        return
    after = []          # (kind, payload) delivered after the lock is released
    with Store() as st:
        g = st.data.get("game")
        if t == "mine_new":
            n = ev.get("size") if ev.get("size") in SIZES else 4
            first = COMPANION if ev.get("first") == COMPANION else OWNER
            if g and not g.get("over"):
                _log(f"game {g['game']} replaced by {game}")
            st.data["game"] = g = {
                "game": game, "n": n, "m": SIZES[n], "first": first, "turn": first, "ready": False,
                "layout": None, "salt": None, "commit": None, "owner_steps": [], "companion_steps": [],
                "over": None, "revealed": False, "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "score_owner": st.data["score"][OWNER], "score_companion": st.data["score"][COMPANION],
            }
            after.append(("passport.mines", {
                "hint": (f"The owner started a game of mutual minesweeper: {n}x{n}, {SIZES[n]} mines each, "
                         f"{'you go' if first == COMPANION else 'the owner goes'} first. They are hiding their "
                         f"mines on the device (never uploaded). Choose yours: `{CTL} mine layout A2,C1,...` "
                         f"({SIZES[n]} cells, rows A.., columns 1..), or `{CTL} mine layout --random`. "
                         "The sha256 commitment is sent to the device automatically."),
                "game": game, "size": n, "mines": SIZES[n], "first": first}))
        elif not g or g["game"] != game:
            _log(f"{t} for game {game} ignored (current {g and g['game']})")
            return
        elif t == "mine_ready":
            g["ready"] = True
            if g["commit"] and g["turn"] == COMPANION and not g["over"]:
                after.append(_turn_event(g, "The owner has hidden their mines and you go first."))
        elif t == "mine_step":
            _owner_step(g, st.data, ev.get("cell"), publish, after)
        elif t == "mine_companion_result":
            _companion_result(g, st.data, ev, publish, after)
        elif t == "mine_owner_reveal":
            _owner_reveal(g, ev.get("layout"), after)
        elif t == "mine_quit":
            if not g["over"]:
                g["over"] = "quit"
                g["ended"] = time.strftime("%Y-%m-%dT%H:%M:%S")
                _log(f"game {game} quit by the owner")
        else:
            _log("unknown", ev)
    for kind, payload in after:
        if kind == "__end__":
            _deliver_end(payload, inject)
        else:
            inject(kind, payload)


def _check_owner(g, cells):
    """The owner's layout against the device's verdicts on the companion's steps."""
    n = g["n"]
    for s in g["companion_steps"]:
        if s.get("boom") is None:
            continue
        if bool(s["boom"]) != (parse_cell(s["cell"], n) in cells):
            return False
    return True


def _owner_reveal(g, layout, after):
    n = g["n"]
    try:
        cells = [parse_cell(c, n) for c in (layout or "").split(",") if c.strip()]
    except MinesError as e:
        _log("owner reveal:", e)
        return
    if len(set(cells)) != g["m"] or len(cells) != g["m"]:
        _log(f"owner reveal {layout!r}: need {g['m']} distinct cells")
        return
    canon = layout_str(cells, n)
    if g.get("owner_layout") == canon:
        return                                    # re-sent
    g["owner_layout"] = canon
    g["owner_layout_consistent"] = _check_owner(g, set(cells))
    _log(f"game {g['game']}: owner mines {canon} (consistent={g['owner_layout_consistent']})")
    if g.get("end_injected") and not g.get("end_had_owner") and g.get("over") in (OWNER, COMPANION):
        g["end_had_owner"] = True                 # the end event already went out: a short follow-up
        after.append(("passport.mines", {
            "hint": f"The owner's mines were {canon} (they were not in the end event yet).",
            "game": g["game"], "owner_layout": canon,
            "owner_layout_consistent": g["owner_layout_consistent"], "board": board_text(g)}))


def _deliver_end(game, inject):
    """End event, held up to END_WAIT_S for the owner's layout so it can ride along."""
    deadline = time.monotonic() + END_WAIT_S
    while True:
        with Store() as st:
            g = st.data.get("game")
            if not g or g["game"] != game or g.get("end_injected"):
                return
            if g.get("owner_layout") or time.monotonic() >= deadline:
                g["end_injected"] = True
                g["end_had_owner"] = bool(g.get("owner_layout"))
                score = st.data["score"]
                winner = g["over"]
                who = "the owner won" if winner == OWNER else "you won"
                payload = {
                    "hint": (f"Minesweeper is over: {who} (score owner {score[OWNER]} : "
                             f"{score[COMPANION]} you). Your layout and salt were revealed to the device "
                             "for verification"
                             f"{'; the owner mines came too (owner_layout, m on their field)' if g.get('owner_layout') else ''}. "
                             "Say something about it through your usual channel; a rematch starts when "
                             "the owner taps Again on the device."),
                    "game": game, "winner": winner, "your_layout": g["layout"],
                    "owner_layout": g.get("owner_layout"), "board": board_text(g)}
                if g.get("owner_layout"):
                    payload["owner_layout_consistent"] = g.get("owner_layout_consistent")
                break
        time.sleep(0.1)
    inject("passport.mines", payload)


def _turn_event(g, lead):
    return ("passport.mines", {
        "hint": (f"{lead} Your turn to step on the owner's field. Pick a cell and add a one-line "
                 "English inner thought (<= 60 chars, shown on the device): "
                 f"`{CTL} mine step C3 \"thought\"`."),
        "game": g["game"], "board": board_text(g)})


def _end(g, data, winner, publish, after):
    g["over"] = winner
    g["ended"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    data["score"][winner] += 1
    g["score_owner"], g["score_companion"] = data["score"][OWNER], data["score"][COMPANION]
    _reveal(g, publish)
    after.append(("__end__", g["game"]))          # _deliver_end, after the lock is released
    _log(f"game {g['game']} over: {winner} won ({data['score'][OWNER]}:{data['score'][COMPANION]})")


def _reveal(g, publish):
    if not g.get("layout"):
        return False
    ok = publish({"op": "mine_reveal", "game": g["game"], "layout": g["layout"], "salt": g["salt"]})
    g["revealed"] = g["revealed"] or bool(ok)
    return ok


def _owner_step(g, data, cell_s, publish, after):
    n = g["n"]
    try:
        cell = parse_cell(cell_s, n)
    except MinesError as e:
        _log("owner step:", e)
        return
    name = cell_name(cell, n)
    if not g.get("layout"):
        _log(f"owner step {name} before the companion's commit: ignored")
        return
    mines = set(parse_cell(c, n) for c in g["layout"].split(","))
    boom = cell in mines
    prev = next((s for s in g["owner_steps"] if s["cell"] == name), None)
    if prev is not None:                      # re-delivery / retry: same answer, nothing else
        publish({"op": "mine_result", "game": g["game"], "cell": name, "boom": prev["boom"]})
        if g["over"] and g["over"] != "quit":
            _reveal(g, publish)
        return
    if g["over"]:
        _log(f"owner step {name} after the end: ignored")
        return
    if g["turn"] != OWNER:
        _log(f"owner step {name} while turn={g['turn']}: answered anyway (the device decides turns)")
    g["owner_steps"].append({"cell": name, "boom": boom, "t": time.strftime("%H:%M:%S")})
    publish({"op": "mine_result", "game": g["game"], "cell": name, "boom": boom})
    safe = sum(1 for s in g["owner_steps"] if not s["boom"])
    if boom:
        _end(g, data, COMPANION, publish, after)
    elif safe >= n * n - g["m"]:
        _end(g, data, OWNER, publish, after)
    else:
        g["turn"] = COMPANION
        after.append(_turn_event(g, f"The owner stepped on your {name}: safe."))


def _companion_result(g, data, ev, publish, after):
    n = g["n"]
    try:
        name = cell_name(parse_cell(ev.get("cell"), n), n)
    except MinesError as e:
        _log("companion result:", e)
        return
    step = next((s for s in g["companion_steps"] if s["cell"] == name), None)
    if step is None:
        _log(f"result for {name}, which the companion never stepped on: ignored")
        return
    if step.get("boom") is not None:
        return                                # re-delivery
    step["boom"] = bool(ev.get("boom"))
    if g["over"]:
        return
    safe = sum(1 for s in g["companion_steps"] if s.get("boom") is False)
    if step["boom"] or ev.get("winner") == OWNER:
        _end(g, data, OWNER, publish, after)
    elif safe >= n * n - g["m"] or ev.get("winner") == COMPANION:
        _end(g, data, COMPANION, publish, after)
    else:
        g["turn"] = OWNER


# ------------------------------------------------------------------ companion -> device
def cmd_layout(spec: str, publish) -> dict:
    with Store() as st:
        g = st.data.get("game")
        if not g or g["over"]:
            raise MinesError("no game running (the owner starts one on the device)")
        if g["commit"]:
            raise MinesError(f"layout already committed ({g['layout']}); it cannot change mid-game")
        n, m = g["n"], g["m"]
        if spec.strip().lower() in ("--random", "random"):
            cells = set()
            while len(cells) < m:
                cells.add(secrets.randbelow(n * n))
        else:
            cells = [parse_cell(c, n) for c in spec.split(",") if c.strip()]
            if len(set(cells)) != len(cells):
                raise MinesError("a cell is listed twice")
            if len(cells) != m:
                raise MinesError(f"need exactly {m} cells for {n}x{n}, got {len(cells)}")
        g["layout"] = layout_str(cells, n)
        g["salt"] = secrets.token_hex(16)
        g["commit"] = commit_of(g["layout"], g["salt"])
        ok = publish({"op": "mine_commit", "game": g["game"], "commit": g["commit"]})
        out = {"published": bool(ok), "layout": g["layout"], "commit": g["commit"]}
        if g["ready"] and g["turn"] == COMPANION:
            out["next"] = f"the owner is ready and you go first: {CTL} mine step <cell> \"thought\""
        elif g["turn"] == COMPANION:
            out["next"] = "you go first once the owner has hidden their mines (you will get an event)"
        else:
            out["next"] = "the owner goes first; you will get an event when it is your turn"
        out["board"] = board_text(g)
        return out


def cmd_step(cell_s: str, note: str, publish) -> dict:
    with Store() as st:
        g = st.data.get("game")
        if not g or g["over"]:
            raise MinesError("no game running")
        n = g["n"]
        name = cell_name(parse_cell(cell_s, n), n)
        raw_note, note = note, clean_note(note)
        pending = next((s for s in g["companion_steps"] if s.get("boom") is None), None)
        if pending:
            if pending["cell"] != name:
                raise MinesError(f"your step {pending['cell']} is still waiting for the device's answer "
                                 f"(re-send it with: mine step {pending['cell']})")
            ok = publish({"op": "mine_companion_step", "game": g["game"], "cell": name, "note": pending["note"]})
            return {"published": bool(ok), "resent": name}
        if any(s["cell"] == name for s in g["companion_steps"]):
            raise MinesError(f"you already stepped on {name}")
        if not g["commit"]:
            raise MinesError(f"commit your layout first: {CTL} mine layout ...")
        if not g["ready"]:
            raise MinesError("the owner is still hiding their mines")
        if g["turn"] != COMPANION:
            raise MinesError("not your turn (the owner is stepping)")
        g["companion_steps"].append({"cell": name, "note": note, "boom": None, "t": time.strftime("%H:%M:%S")})
        g["turn"] = "companion_wait"
        ok = publish({"op": "mine_companion_step", "game": g["game"], "cell": name, "note": note})
        out = {"published": bool(ok), "cell": name, "note": note,
               "info": "the device judges it; the end (if any) comes as an event"}
        if note != re.sub(r"\s+", " ", (raw_note or "")).strip():
            out["note_changed"] = "the device shows ASCII only, <= 60 chars: write the thought in English"
        return out


def cmd_state() -> dict:
    with Store() as st:
        out = _public(st.data.get("game"))
        out["score_total"] = st.data["score"]
        return out


def cmd_reveal(publish) -> dict:
    with Store() as st:
        g = st.data.get("game")
        if not g or not g.get("layout"):
            raise MinesError("nothing to reveal")
        if not g["over"]:
            raise MinesError("the game is still running (revealing now would give the layout away)")
        return {"published": bool(_reveal(g, publish)), "layout": g["layout"], "salt": g["salt"]}


def cmd_reset() -> dict:
    with Store() as st:
        st.data["game"] = None
        return {"reset": True, "score": st.data["score"]}
