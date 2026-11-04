#!/usr/bin/env python3
"""Standalone tests for bingo.py with a fake publisher / injector, a fake clock
and a temporary checkins.jsonl (the real one is never read).

    python3 test_bingo.py        (or: python3 -m pytest test_bingo.py)
"""
import json, os, random, sys, tempfile, unittest
from datetime import datetime

TMP = tempfile.mkdtemp(prefix="bingo-test-")
HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["PASSPORT_ENV_FILE"] = os.devnull      # never pick up a real .env
os.environ["PASSPORT_STATE_DIR"] = TMP
os.environ["PASSPORT_CHECKINS_FILE"] = os.path.join(TMP, "checkins.jsonl")
os.environ["PASSPORT_TZ"] = "+08:00"              # the firmware host test instants below assume UTC+8
os.environ["BINGO_TASKS_FILE"] = os.path.join(HERE, "examples", "bingo_tasks.example.json")
sys.path.insert(0, HERE)
import bingo  # noqa: E402

CK = os.environ["PASSPORT_CHECKINS_FILE"]


def at(s):
    return datetime.fromisoformat(s).replace(tzinfo=bingo.TZ)


class Fake:
    def __init__(self):
        self.out, self.events = [], []

    def publish(self, cmd):
        self.out.append(cmd)
        return True

    def inject(self, kind, payload):
        self.events.append((kind, payload))

    def last(self):
        return self.out[-1]


def done_labels(cmd):
    cells = cmd["cells"].split(",")
    return {cells[i] for i in range(len(cells)) if cmd["done"] >> i & 1}


class BingoTest(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(TMP, "bingo.json"), "w") as fh:
            fh.write("{}")
        with open(CK, "w") as fh:
            fh.write("")
        bingo._sig = None
        bingo._rng = random.Random(7)
        self.clock = at("2026-11-02T08:00:00")
        bingo._now = lambda: self.clock
        self.f = Fake()

    def dev(self, ev):
        bingo.on_device(ev, self.f.publish, self.f.inject)

    def checkin(self, t, tag, ghost=False):
        e = {"t": t + "+08:00", "tag": tag, "label": tag}
        if ghost:
            e["ghost"] = "wrong tag"
        with open(CK, "a") as fh:
            fh.write(json.dumps(e) + "\n")

    def poll(self):
        bingo._sig = None
        return bingo.poll_checkins(self.f.publish, self.f.inject)

    def card(self):
        with bingo.Store() as st:
            return st.data["cards"][bingo.card_day()]

    # -------------------------------------------------------------- days
    def test_day_boundary(self):
        self.assertEqual(bingo.card_day(at("2026-11-02T05:59:59")), "2026-11-01")
        self.assertEqual(bingo.card_day(at("2026-11-02T06:00:00")), "2026-11-02")
        self.assertEqual(bingo.card_day(at("2026-11-03T01:30:00")), "2026-11-02")
        self.assertEqual(bingo.card_day(at("2026-03-01T05:00:00")), "2026-02-28")
        # The device's host test uses the same instants (tests/test_cs_logic.c).
        self.assertEqual(bingo.card_day(datetime.fromtimestamp(1793570399, bingo.TZ)), "2026-11-01")
        self.assertEqual(bingo.card_day(datetime.fromtimestamp(1793570400, bingo.TZ)), "2026-11-02")

    # -------------------------------------------------------------- rolling
    def test_roll_fixed_unique_and_whisper_event(self):
        self.dev({"type": "bingo_get", "n": 4, "day": ""})
        c = self.f.last()
        cells = c["cells"].split(",")
        self.assertEqual((c["op"], c["day"], c["n"], len(cells)), ("bingo_card", "2026-11-02", 4, 16))
        self.assertEqual(len(set(cells)), 16)
        for lab in bingo.FIXED:
            self.assertIn(lab, cells)
        self.assertEqual(c["fixed"], sum(1 << i for i, lab in enumerate(cells) if lab in bingo.FIXED))
        self.assertNotIn("whisper", c)
        self.assertFalse(c["bingo"])
        self.assertEqual(len(self.f.events), 1)                       # companion: write today's whisper
        self.assertEqual(self.f.events[0][0], "passport.bingo")
        self.assertIn("bingo whisper", self.f.events[0][1]["hint"])
        # Asking again: the same card, no second event.
        self.dev({"type": "bingo_get", "n": 5, "day": "2026-11-02"})
        self.assertEqual(self.f.last()["cells"], c["cells"])
        self.assertEqual(len(self.f.events), 1)
        # 5x5 at the next day (the owner's size choice is used for a new card).
        self.clock = at("2026-11-03T06:00:00")
        self.dev({"type": "bingo_get", "n": 5, "day": "2026-11-02"})
        c5 = self.f.last()
        self.assertEqual((c5["day"], c5["n"], len(set(c5["cells"].split(",")))), ("2026-11-03", 5, 25))

    def test_labels_fit_the_device(self):
        for lab in bingo.POOL:
            self.assertLessEqual(len(lab), bingo.LABEL_MAX)
            self.assertNotIn(",", lab)
        self.assertGreaterEqual(len(bingo.POOL) - len(bingo.FIXED), 25 - len(bingo.FIXED) + 5)

    def test_reroll_no_duplicates(self):
        self.dev({"type": "bingo_get", "n": 5})
        cells = self.f.last()["cells"].split(",")
        free = [lab for lab in cells if lab not in bingo.FIXED]
        seen_out = []
        for k, lab in enumerate(free[:12]):                             # many swaps in a row
            self.dev({"type": "bingo_reroll", "day": "2026-11-02", "cell": lab, "seq": k + 1})
            now = self.f.last()["cells"].split(",")
            self.assertEqual(len(set(now)), 25, now)
            self.assertNotIn(lab, now)
            seen_out.append(lab)
            for gone in seen_out:                                       # not back the same day (pool allows)
                self.assertNotIn(gone, now)
            self.assertEqual(self.f.last()["ack"], k + 1)
        # Fixed and ticked cells refuse; the card stays as it was.
        before = self.f.last()["cells"]
        self.dev({"type": "bingo_reroll", "day": "2026-11-02", "cell": bingo.FIXED[0], "seq": 50})
        self.assertEqual(self.f.last()["cells"], before)
        lab = [x for x in before.split(",") if x not in bingo.FIXED][0]
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": lab, "on": True, "seq": 51})
        self.dev({"type": "bingo_reroll", "day": "2026-11-02", "cell": lab, "seq": 52})
        self.assertEqual(self.f.last()["cells"], before)
        # The companion's reroll: same rules.
        with self.assertRaises(bingo.BingoError):
            bingo.cmd_reroll(bingo.FIXED[0].lower(), self.f.publish)
        # An exhausted pool still never duplicates.
        for _ in range(40):
            free = [x for x in self.f.last()["cells"].split(",") if x not in bingo.FIXED and x != lab]
            bingo.cmd_reroll(free[0], self.f.publish)
            now = self.f.last()["cells"].split(",")
            self.assertEqual(len(set(now)), 25)

    # -------------------------------------------------------------- ticks
    def test_ticks_seq_and_ack(self):
        self.dev({"type": "bingo_get", "n": 4})
        cells = self.f.last()["cells"].split(",")
        a, b = cells[0], cells[1]
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": a, "on": True, "seq": 5})
        self.assertEqual(done_labels(self.f.last()), {a})
        self.assertEqual(self.f.last()["ack"], 5)
        # Untick (seq 7) overtaken by a late tick re-send (seq 5) and an older one (seq 4): ignored.
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": a, "on": False, "seq": 7})
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": a, "on": True, "seq": 5})
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": a, "on": True, "seq": 4})
        self.assertEqual(done_labels(self.f.last()), set())
        self.assertEqual(self.f.last()["ack"], 7)
        # Labels match case-insensitively; an unknown label is acknowledged and ignored.
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": b.lower(), "on": True, "seq": 8})
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": "NOPE", "on": True, "seq": 9})
        self.assertEqual(done_labels(self.f.last()), {b})
        self.assertEqual(self.f.last()["ack"], 9)
        # The companion ticks / unticks.
        bingo.cmd_tick(a, True, self.f.publish)
        self.assertEqual(done_labels(self.f.last()), {a, b})
        self.assertEqual(self.card()["ticks"][a]["src"], "companion")
        bingo.cmd_tick(a, False, self.f.publish)
        with self.assertRaises(bingo.BingoError):
            bingo.cmd_tick("NOPE", True, self.f.publish)
        # A tick for a day with no card: acknowledged, today's card is sent.
        self.dev({"type": "bingo_tick", "day": "2026-10-01", "cell": a, "on": True, "seq": 30})
        self.assertEqual((self.f.last()["day"], self.f.last()["ack"]), ("2026-11-02", 30))

    def test_size_keeps_ticks_and_fixed(self):
        self.dev({"type": "bingo_get", "n": 4})
        cells = self.f.last()["cells"].split(",")
        mine = [x for x in cells if x not in bingo.FIXED][:3]
        for k, lab in enumerate(mine):
            self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": lab, "on": True, "seq": k + 1})
        self.dev({"type": "bingo_size", "day": "2026-11-02", "n": 5, "seq": 9})
        c = self.f.last()
        now = c["cells"].split(",")
        self.assertEqual((c["n"], len(set(now)), c["ack"]), (5, 25, 9))
        self.assertEqual(done_labels(c), set(mine))
        for lab in bingo.FIXED:
            self.assertIn(lab, now)
        with bingo.Store() as st:
            self.assertEqual(st.data["size"], 5)
        bingo.cmd_roll(4, self.f.publish)
        c = self.f.last()
        self.assertEqual((c["n"], len(set(c["cells"].split(",")))), (4, 16))
        self.assertEqual(done_labels(c), set(mine))

    # -------------------------------------------------------------- lines + whisper
    def test_lines(self):
        L = bingo.lines_of
        self.assertEqual(L(set(), 4), [])
        self.assertEqual(L({4, 5, 6, 7}, 4), [("row 2", [4, 5, 6, 7])])
        self.assertEqual(L({1, 5, 9, 13}, 4)[0][0], "col 2")
        self.assertEqual(L({0, 5, 10, 15}, 4)[0][0], "diag")
        self.assertEqual(L({3, 6, 9, 12}, 4)[0][0], "anti")
        self.assertEqual(L({0, 1, 2, 3}, 5), [])
        self.assertEqual(L({20, 21, 22, 23, 24}, 5)[0][0], "row 5")
        self.assertEqual(L({4, 9, 14, 19, 24}, 5)[0][0], "col 5")
        self.assertEqual(L({0, 6, 12, 18, 24}, 5)[0][0], "diag")
        self.assertEqual(L({4, 8, 12, 16, 20}, 5)[0][0], "anti")
        self.assertEqual(len(L(set(range(16)), 4)), 10)
        self.assertEqual(len(L(set(range(25)), 5)), 12)
        self.assertEqual(len(L({0, 6, 12, 18, 24, 4, 8, 16, 20}, 5)), 2)

    def test_whisper_hidden_until_bingo(self):
        self.dev({"type": "bingo_get", "n": 4})
        r = bingo.cmd_whisper("You did it, \u2764! Water + sun = me smiling.", self.f.publish)
        self.assertEqual(r["whisper"], "You did it, ! Water + sun = me smiling.")
        self.assertIn("changed", r)
        self.assertFalse(r["revealed_now"])
        cells = self.f.last()["cells"].split(",")
        row = cells[4:8]
        for k, lab in enumerate(row[:3]):
            self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": lab, "on": True, "seq": k + 1})
        for cmd in self.f.out:
            self.assertNotIn("whisper", cmd)                            # never before the line
            self.assertNotIn("Water", json.dumps(cmd))
        n_events = len(self.f.events)
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": row[3], "on": True, "seq": 4})
        c = self.f.last()
        self.assertTrue(c["bingo"])
        self.assertEqual(c["whisper"], "You did it, ! Water + sun = me smiling.")
        self.assertEqual(len(self.f.events), n_events + 1)              # companion: cheer them on
        ev = self.f.events[-1][1]
        self.assertEqual((ev["line"], ev["cells"], ev["whisper_by"]), ("row 2", row, "companion"))
        # A second line: no second event; the whisper keeps riding along.
        col = [cells[i] for i in (0, 4, 8, 12)]
        for k, lab in enumerate(col):
            if lab not in row:
                self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": lab, "on": True, "seq": 10 + k})
        self.assertEqual(len(self.f.events), n_events + 1)
        self.assertTrue(self.f.last()["bingo"])
        # Unticking the line keeps it revealed (the owner has read it).
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": row[0], "on": False, "seq": 20})
        self.assertIn("whisper", self.f.last())
        # The companion rewrites it afterwards: pushed at once.
        n = len(self.f.out)
        r = bingo.cmd_whisper("Updated note", self.f.publish)
        self.assertTrue(r["revealed_now"])
        self.assertEqual(len(self.f.out), n + 1)
        self.assertEqual(self.f.last()["whisper"], "Updated note")

    def test_bingo_event_from_device_uses_fallback(self):
        self.dev({"type": "bingo_get", "n": 5})
        cells = self.f.last()["cells"].split(",")
        diag = [cells[i] for i in (0, 6, 12, 18, 24)]
        # The owner's ticks were lost / reordered; bingo_bingo carries the line's cells.
        self.dev({"type": "bingo_bingo", "day": "2026-11-02", "line": "diag", "cells": ",".join(diag)})
        c = self.f.last()
        self.assertTrue(c["bingo"])
        self.assertIn(c["whisper"], bingo.FALLBACK_WHISPERS)
        self.assertEqual(done_labels(c), set(diag))
        ev = self.f.events[-1][1]
        self.assertEqual((ev["whisper_by"], ev["line"]), ("fallback", "diag"))
        self.assertIn("bingo whisper", ev["hint"])                    # the companion can still write one
        # The device repeating bingo_bingo: same answer, no second event.
        k = len(self.f.events)
        self.dev({"type": "bingo_bingo", "day": "2026-11-02", "line": "diag", "cells": ",".join(diag)})
        self.assertEqual(len(self.f.events), k)
        self.assertTrue(self.f.last()["bingo"])

    # -------------------------------------------------------------- check-ins
    def _force(self, labels):
        """Put AUTO labels on today's card (the roll is random)."""
        with bingo.Store() as st:
            card = st.data["cards"][bingo.card_day()]
            free = [i for i, x in enumerate(card["cells"]) if x not in bingo.FIXED and x not in labels]
            for lab in labels:
                if lab not in card["cells"]:
                    card["cells"][free.pop()] = lab

    def test_auto_rules(self):
        ck = [(at("2026-11-01T23:40:00"), "sleep"), (at("2026-11-02T07:10:00"), "wake"),
              (at("2026-11-02T15:00:00"), "out"), (at("2026-11-03T00:30:00"), "sleep")]
        self.assertEqual(bingo.auto_hits(ck, "2026-11-02"), {"UP<10", "BED<1", "SLEPT", "OUT"})
        # Edges: up at 10:00 is late; bed at 01:00 is late; 6h59 is not 7 h.
        ck = [(at("2026-11-02T03:00:00"), "sleep"), (at("2026-11-02T10:00:00"), "wake"),
              (at("2026-11-03T01:00:00"), "sleep")]
        self.assertEqual(bingo.auto_hits(ck, "2026-11-02"), {"SLEPT"})              # exactly 7 h counts
        ck = [(at("2026-11-02T03:01:00"), "sleep"), (at("2026-11-02T09:59:59"), "wake"),
              (at("2026-11-02T18:00:00"), "sleep")]
        self.assertEqual(bingo.auto_hits(ck, "2026-11-02"), {"UP<10", "BED<1"})
        # Getting up before 06:00 belongs to the day before; the card day's first wake counts.
        ck = [(at("2026-11-01T22:00:00"), "sleep"), (at("2026-11-02T05:30:00"), "wake"),
              (at("2026-11-02T06:10:00"), "wake")]
        self.assertEqual(bingo.auto_hits(ck, "2026-11-01"), {"SLEPT", "BED<1"})   # 05:30 is still the 1st
        self.assertEqual(bingo.auto_hits(ck, "2026-11-02"), {"UP<10", "SLEPT"})
        # A missed bed scan (gap > 16 h) is not a night's sleep.
        ck = [(at("2026-10-31T23:00:00"), "sleep"), (at("2026-11-02T09:00:00"), "wake")]
        self.assertEqual(bingo.auto_hits(ck, "2026-11-02"), {"UP<10"})
        # out outside the card day; both door states count as out.
        ck = [(at("2026-11-02T05:00:00"), "out"), (at("2026-11-03T06:00:00"), "out")]
        self.assertEqual(bingo.auto_hits(ck, "2026-11-02"), set())

    def test_poll_ticks_ghosts_and_veto(self):
        self.dev({"type": "bingo_get", "n": 5})
        self._force(bingo.AUTO)
        self.checkin("2026-11-01T23:30:00", "sleep")
        self.checkin("2026-11-02T07:30:00", "other", ghost=True)
        self.checkin("2026-11-02T07:31:00", "wake", ghost=True)   # a ghost never counts
        self.assertFalse(self.poll())
        self.checkin("2026-11-02T08:05:00", "wake")
        self.assertTrue(self.poll())
        c = self.f.last()
        self.assertEqual(done_labels(c), {"UP<10", "SLEPT"})
        auto = {c["cells"].split(",")[i] for i in range(25) if c["auto"] >> i & 1}
        self.assertEqual(auto, {"UP<10", "SLEPT"})
        # Unchanged file: nothing to do.
        n = len(self.f.out)
        self.assertFalse(bingo.poll_checkins(self.f.publish, self.f.inject))
        self.assertEqual(len(self.f.out), n)
        # The owner unticks SLEPT (disagrees): the poller leaves it off for the day.
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": "SLEPT", "on": False, "seq": 3})
        self.checkin("2026-11-02T13:00:00", "out")
        self.poll()
        self.assertEqual(done_labels(self.f.last()), {"UP<10", "OUT"})
        # The owner's own tick of OUT, then the out check-in turns out to be a ghost: the tick stays.
        self.dev({"type": "bingo_tick", "day": "2026-11-02", "cell": "OUT", "on": True, "seq": 4})
        with open(CK) as fh:
            lines = fh.read().splitlines()
        e = json.loads(lines[-1])
        e["ghost"] = "test"
        lines[-1] = json.dumps(e)
        with open(CK, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        self.poll()
        self.assertIn("OUT", done_labels(self.f.last()))
        # An auto tick whose check-in becomes a ghost is withdrawn.
        with open(CK) as fh:
            lines = fh.read().splitlines()
        for k, line in enumerate(lines):
            e = json.loads(line)
            if e["tag"] == "wake" and "ghost" not in e:
                e["ghost"] = "test"
                lines[k] = json.dumps(e)
        with open(CK, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        self.poll()
        self.assertNotIn("UP<10", done_labels(self.f.last()))

    def test_bed_late_on_the_same_card_and_day_rollover(self):
        self.dev({"type": "bingo_get", "n": 4})
        self._force(["BED<1"])
        self.clock = at("2026-11-03T00:40:00")                         # still the 2nd's card
        self.checkin("2026-11-03T00:35:00", "sleep")
        self.poll()
        self.assertEqual((self.f.last()["day"], done_labels(self.f.last())), ("2026-11-02", {"BED<1"}))
        # After 06:00 the poller still fixes up the previous card (late scan), and rolls today's.
        self.clock = at("2026-11-03T06:30:00")
        n_ev = len(self.f.events)
        self.checkin("2026-11-03T06:20:00", "wake")
        self.poll()
        with bingo.Store() as st:
            self.assertIn("2026-11-03", st.data["cards"])
            self.assertIn("BED<1", st.data["cards"]["2026-11-02"]["ticks"])
        self.assertEqual(self.f.events[n_ev][0], "passport.bingo")      # whisper request for the 3rd

    def test_poll_creates_card_and_auto_bingo_reveals(self):
        # A check-in before the device asked: the card is rolled, auto ticks land;
        # a line completed by auto ticks reveals the whisper and tells the companion.
        self.checkin("2026-11-02T07:00:00", "wake")
        self.poll()
        self.assertEqual(self.f.events[0][0], "passport.bingo")
        with bingo.Store() as st:
            card = st.data["cards"]["2026-11-02"]
            row = card["cells"][0:4]
            if "UP<10" not in row:                                      # put UP<10 into row 1
                j = card["cells"].index("UP<10")
                card["cells"][0], card["cells"][j] = card["cells"][j], card["cells"][0]
                row = card["cells"][0:4]
            for lab in row:
                if lab != "UP<10":
                    card["ticks"][lab] = {"src": "owner", "t": "x"}
            card["ticks"].pop("UP<10", None)
        self.poll()
        c = self.f.last()
        self.assertTrue(c["bingo"])
        self.assertIn("whisper", c)
        self.assertEqual(self.f.events[-1][1]["how"], "auto")


if __name__ == "__main__":
    unittest.main(verbosity=1)
