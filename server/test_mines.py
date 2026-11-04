#!/usr/bin/env python3
"""Standalone tests for mines.py with a fake publisher / injector and a fake
device that follows the firmware's rules (the owner's mines stay on the "device").

    python3 test_mines.py        (or: python3 -m pytest test_mines.py)
"""
import hashlib, os, sys, tempfile, threading, time, unittest

TMP = tempfile.mkdtemp(prefix="mines-test-")
os.environ["PASSPORT_ENV_FILE"] = os.devnull      # never pick up a real .env
os.environ["PASSPORT_STATE_DIR"] = TMP
os.environ["PASSPORT_MINES_END_WAIT_S"] = "0.5"  # the end event waits this long for the owner's layout
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mines  # noqa: E402


class Fake:
    def __init__(self):
        self.out, self.events = [], []

    def publish(self, cmd):
        self.out.append(cmd)
        return True

    def inject(self, kind, payload):
        self.events.append((kind, payload))

    def ops(self):
        return [c["op"] for c in self.out]


def dev(ev, f):
    mines.on_device(ev, f.publish, f.inject)


class MinesTest(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(TMP, "mines.json"), "w") as fh:   # fresh game and score
            fh.write("{}")
        self.f = Fake()

    def test_helpers(self):
        self.assertEqual(mines.cell_name(5, 4), "B2")
        self.assertEqual(mines.parse_cell(" b2 ", 4), 5)
        for bad in ("E1", "A5", "B", "2B", ""):
            with self.assertRaises(mines.MinesError):
                mines.parse_cell(bad, 4)
        self.assertEqual(mines.layout_str([6, 1], 3), "A2,C1")
        # Same value as the firmware host test (tests/test_cs_logic.c).
        self.assertEqual(mines.commit_of("A2,C1", "5f1c"), "c2f0952c64b081da")
        self.assertEqual(mines.clean_note("  they 走了 corners…  B2!  " + "x" * 80)[:20], "they corners B2! xxx")
        self.assertLessEqual(len(mines.clean_note("y" * 200)), mines.NOTE_MAX)

    def test_owner_first_steps_on_a_mine(self):
        f = self.f
        dev({"type": "mine_new", "game": 7, "size": 4, "mines": 3, "first": "owner"}, f)
        self.assertEqual(len(f.events), 1)
        self.assertIn("mine layout", f.events[0][1]["hint"])
        with self.assertRaises(mines.MinesError):
            mines.cmd_layout("A1,B2", f.publish)                       # 2 of 3
        with self.assertRaises(mines.MinesError):
            mines.cmd_layout("A1,A1,B2", f.publish)
        r = mines.cmd_layout("d4, a2,C1", f.publish)
        self.assertEqual(r["layout"], "A2,C1,D4")
        commit = f.out[-1]
        self.assertEqual(commit, {"op": "mine_commit", "game": 7, "commit": r["commit"]})
        with self.assertRaises(mines.MinesError):
            mines.cmd_layout("A1,B1,C3", f.publish)                    # never changes
        dev({"type": "mine_ready", "game": 7}, f)
        self.assertEqual(len(f.events), 1)                              # owner goes first: no turn event
        with self.assertRaises(mines.MinesError):
            mines.cmd_step("A1", "too early", f.publish)
        # The owner's safe step: answered at once, then the companion's turn.
        dev({"type": "mine_step", "game": 7, "cell": "B3"}, f)
        self.assertEqual(f.out[-1], {"op": "mine_result", "game": 7, "cell": "B3", "boom": False})
        self.assertEqual(len(f.events), 2)
        self.assertIn("B3", f.events[1][1]["hint"])
        self.assertIn("board", f.events[1][1])
        # A re-delivered step gets the same answer and no second event.
        dev({"type": "mine_step", "game": 7, "cell": "B3"}, f)
        self.assertEqual(f.out[-1]["boom"], False)
        self.assertEqual(len(f.events), 2)
        # The companion steps with a thought; a second step before the answer is refused.
        r = mines.cmd_step("a1", "They avoided corners last time... A1.", f.publish)
        self.assertEqual(f.out[-1], {"op": "mine_companion_step", "game": 7, "cell": "A1",
                                     "note": "They avoided corners last time... A1."})
        with self.assertRaises(mines.MinesError):
            mines.cmd_step("A2", "x", f.publish)
        n_out = len(f.out)
        mines.cmd_step("A1", "ignored", f.publish)                     # re-send of the pending one
        self.assertEqual(len(f.out), n_out + 1)
        self.assertEqual(f.out[-1]["note"], "They avoided corners last time... A1.")
        dev({"type": "mine_companion_result", "game": 7, "cell": "A1", "boom": False, "winner": ""}, f)
        self.assertEqual(mines.cmd_state()["turn"], "owner")
        with self.assertRaises(mines.MinesError):
            mines.cmd_step("A3", "x", f.publish)                       # owner's turn now
        with self.assertRaises(mines.MinesError):
            mines.cmd_reveal(f.publish)                                # not while playing
        # The owner hits C1: the companion wins, reveal goes out by itself, end event.
        dev({"type": "mine_step", "game": 7, "cell": "C1"}, f)
        self.assertEqual(f.out[-2], {"op": "mine_result", "game": 7, "cell": "C1", "boom": True})
        rev = f.out[-1]
        self.assertEqual(rev["op"], "mine_reveal")
        self.assertEqual(hashlib.sha256((rev["layout"] + rev["salt"]).encode()).hexdigest()[:16], commit["commit"])
        self.assertEqual(f.events[-1][1]["winner"], "companion")
        st = mines.cmd_state()
        self.assertEqual(st["over"], "companion")
        self.assertEqual(st["score_total"], {"owner": 0, "companion": 1})
        self.assertNotIn("salt", st)
        with self.assertRaises(mines.MinesError):
            mines.cmd_step("A3", "x", f.publish)
        self.assertEqual(mines.cmd_reveal(f.publish)["layout"], "A2,C1,D4")

    def test_companion_first_ready_before_commit_and_loses(self):
        f = self.f
        dev({"type": "mine_new", "game": 9, "size": 3, "mines": 2, "first": "companion"}, f)
        dev({"type": "mine_ready", "game": 9}, f)
        self.assertEqual(len(f.events), 1)                              # no commit yet: no turn event
        r = mines.cmd_layout("--random", f.publish)
        self.assertEqual(len(r["layout"].split(",")), 2)
        self.assertIn("mine step", r["next"])
        dev({"type": "mine_step", "game": 99, "cell": "A1"}, f)        # other game: ignored
        self.assertEqual(f.ops(), ["mine_commit"])
        mines.cmd_step("B2", "Middle. Always the middle.", f.publish)
        dev({"type": "mine_companion_result", "game": 9, "cell": "B2", "boom": True, "winner": "owner"}, f)
        self.assertEqual(f.ops(), ["mine_commit", "mine_companion_step", "mine_reveal"])
        self.assertEqual(f.events[-1][1]["winner"], "owner")
        self.assertEqual(mines.cmd_state()["score_total"], {"owner": 1, "companion": 0})
        dev({"type": "mine_companion_result", "game": 9, "cell": "B2", "boom": True, "winner": "owner"}, f)
        self.assertEqual(mines.cmd_state()["score_total"], {"owner": 1, "companion": 0})   # counted once

    def test_companion_first_turn_event_on_ready(self):
        f = self.f
        dev({"type": "mine_new", "game": 3, "size": 5, "mines": 4, "first": "companion"}, f)
        mines.cmd_layout("A1,B2,C3,E5", f.publish)
        dev({"type": "mine_ready", "game": 3}, f)
        self.assertEqual(len(f.events), 2)
        self.assertIn("you go first", f.events[1][1]["hint"])

    def test_clearing_wins(self):
        f = self.f
        dev({"type": "mine_new", "game": 4, "size": 3, "mines": 2, "first": "owner"}, f)
        mines.cmd_layout("C2,C3", f.publish)
        dev({"type": "mine_ready", "game": 4}, f)
        safe = ["A1", "A2", "A3", "B1", "B2", "B3", "C1"]
        comp = ["A1", "A2", "A3", "B1", "B2", "B3"]
        for i, cell in enumerate(safe):
            dev({"type": "mine_step", "game": 4, "cell": cell}, f)
            if i < len(safe) - 1:
                mines.cmd_step(comp[i], "", f.publish)
                dev({"type": "mine_companion_result", "game": 4, "cell": comp[i], "boom": False, "winner": ""}, f)
        self.assertEqual(mines.cmd_state()["over"], "owner")
        self.assertEqual(f.out[-1]["op"], "mine_reveal")
        # 1 start + 6 turn events + 1 end
        self.assertEqual(len(f.events), 8)

    def test_quit_and_new_game_replaces(self):
        f = self.f
        dev({"type": "mine_new", "game": 5, "size": 4, "first": "owner"}, f)
        dev({"type": "mine_quit", "game": 5}, f)
        self.assertEqual(mines.cmd_state()["over"], "quit")
        with self.assertRaises(mines.MinesError):
            mines.cmd_layout("--random", f.publish)
        dev({"type": "mine_new", "game": 6, "size": 7, "first": "x"}, f)   # bad size -> 4x4, owner first
        st = mines.cmd_state()
        self.assertEqual((st["game"], st["n"], st["m"], st["first"]), (6, 4, 3, "owner"))
        dev({"type": "mine_step", "game": 6, "cell": "A1"}, f)            # before the commit: ignored
        self.assertNotIn("mine_result", f.ops())

    def _boom_game(self, game=20):
        """4x4, owner first, companion's mines A1,B1,C1; companion steps D4 safe; owner steps A1 (boom)."""
        f = self.f
        dev({"type": "mine_new", "game": game, "size": 4, "first": "owner"}, f)
        mines.cmd_layout("A1,B1,C1", f.publish)
        dev({"type": "mine_ready", "game": game}, f)
        dev({"type": "mine_step", "game": game, "cell": "A2"}, f)
        mines.cmd_step("D4", "corner", f.publish)
        dev({"type": "mine_companion_result", "game": game, "cell": "D4", "boom": False, "winner": ""}, f)
        return f

    def test_owner_reveal_during_end_wait(self):
        f = self._boom_game()
        t0 = time.monotonic()
        th = threading.Timer(0.15, dev, ({"type": "mine_owner_reveal", "game": 20, "layout": "c4, B3,D3"}, f))
        th.start()
        dev({"type": "mine_step", "game": 20, "cell": "A1"}, f)        # boom: end, waits for the owner's layout
        th.join()
        self.assertLess(time.monotonic() - t0, 0.45)                  # did not sit out the whole wait
        end = f.events[-1][1]
        self.assertEqual(end["winner"], "companion")
        self.assertEqual(end["owner_layout"], "B3,C4,D3")
        self.assertTrue(end["owner_layout_consistent"])
        self.assertIn(" B . . m .", end["board"])
        self.assertIn(" D . . m f", end["board"])
        n = len(f.events)
        dev({"type": "mine_owner_reveal", "game": 20, "layout": "B3,C4,D3"}, f)   # re-sent: nothing new
        self.assertEqual(len(f.events), n)
        st = mines.cmd_state()
        self.assertEqual(st["owner_layout"], "B3,C4,D3")
        self.assertIn("m owner mine", st["board"])

    def test_owner_reveal_after_end_event(self):
        f = self._boom_game(21)
        t0 = time.monotonic()
        dev({"type": "mine_step", "game": 21, "cell": "A1"}, f)
        self.assertGreaterEqual(time.monotonic() - t0, 0.45)          # waited, then sent without it
        self.assertIsNone(f.events[-1][1]["owner_layout"])
        n = len(f.events)
        dev({"type": "mine_owner_reveal", "game": 21, "layout": "D4,B3,C4"}, f)   # D4: the companion stepped there safely
        self.assertEqual(len(f.events), n + 1)                        # one short follow-up
        follow = f.events[-1][1]
        self.assertEqual(follow["owner_layout"], "B3,C4,D4")
        self.assertFalse(follow["owner_layout_consistent"])             # contradicts the device's verdict
        dev({"type": "mine_owner_reveal", "game": 21, "layout": "B3,C4,D4"}, f)
        self.assertEqual(len(f.events), n + 1)

    def test_owner_reveal_validation_and_hidden_while_playing(self):
        f = self._boom_game(22)
        dev({"type": "mine_owner_reveal", "game": 22, "layout": "B3,C4"}, f)        # too few: ignored
        dev({"type": "mine_owner_reveal", "game": 22, "layout": "B3,B3,C4"}, f)     # repeated: ignored
        dev({"type": "mine_owner_reveal", "game": 22, "layout": "B3,C4,Z9"}, f)     # bad cell: ignored
        dev({"type": "mine_owner_reveal", "game": 99, "layout": "B3,C4,D3"}, f)     # other game
        self.assertNotIn("owner_layout", mines.cmd_state())
        # Stored if it overtakes the final result, but not shown until the game is over.
        dev({"type": "mine_owner_reveal", "game": 22, "layout": "B3,C4,D3"}, f)
        st = mines.cmd_state()
        self.assertNotIn("owner_layout", st)
        self.assertNotIn(" B . . m", st["board"])
        dev({"type": "mine_step", "game": 22, "cell": "A1"}, f)                   # end: no wait needed
        self.assertEqual(f.events[-1][1]["owner_layout"], "B3,C4,D3")

    def test_quit_with_reveal(self):
        f = self._boom_game(23)
        n = len(f.events)
        dev({"type": "mine_quit", "game": 23}, f)
        dev({"type": "mine_owner_reveal", "game": 23, "layout": "B3,C4,D3"}, f)
        self.assertEqual(len(f.events), n)                           # quiet: no event for a quit
        st = mines.cmd_state()
        self.assertEqual((st["over"], st["owner_layout"]), ("quit", "B3,C4,D3"))

    def test_board_text(self):
        f = self.f
        dev({"type": "mine_new", "game": 8, "size": 3, "first": "owner"}, f)
        mines.cmd_layout("A1,C3", f.publish)
        dev({"type": "mine_ready", "game": 8}, f)
        dev({"type": "mine_step", "game": 8, "cell": "B2"}, f)
        text = mines.cmd_state()["board"]
        self.assertIn(" A M . .", text)
        self.assertIn(" B . o .", text)
        self.assertIn("A1 A2 A3 B1 B2 B3 C1 C2 C3", text)


if __name__ == "__main__":
    unittest.main(verbosity=1)
