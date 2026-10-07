#!/usr/bin/env python3
"""Known original-game race classification (README.md, "Known original-game
races"). No xenia run: the fixtures are recorded fork-regress S1 logs trimmed
to the lines analyze/dc3_flow.py reads (tests/fixtures/*/run_meta.json names
the source run; the trim was checked to leave parse() unchanged).

  S1-splash-race                  the Rnd::DoWorldEnd splash race   -> INCONCLUSIVE
  S1-pass                         a normal PASS                     -> PASS
  S1-fail-other-unmapped-read     a different pre-title unmapped
                                  read crash (guest 0 and 0xC)      -> FAIL

The mutation cases rewrite one line of the race fixture each, so every
condition of the match is shown to be load-bearing.

  /usr/bin/python3 tools/fork-regress/tests/test_known_game_race.py
"""
import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
HARNESS = HERE.parent
FIX = HERE / "fixtures"
RACE = "known_game_race:splash_postprocessor_uaf"


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


dc3_flow = load(HARNESS / "analyze" / "dc3_flow.py", "t_dc3_flow")
fr = load(HARNESS / "lib" / "fr.py", "t_fr")
compare = load(HARNESS / "compare.py", "t_compare")


def analyze(run_dir):
    meta = json.loads((run_dir / "run_meta.json").read_text())
    return dc3_flow.analyze(run_dir, meta)


class Fixtures(unittest.TestCase):
    def test_race_is_inconclusive(self):
        v, reasons, m, _ = analyze(FIX / "S1-splash-race")
        self.assertEqual(v, "INCONCLUSIVE")
        self.assertEqual(reasons[0], RACE)
        self.assertEqual(m["known_game_race"], "splash_postprocessor_uaf")
        self.assertEqual(m["failure_class"], RACE)
        # the original FAIL reasons are kept after the race reason
        self.assertIn("max NON_XMA faults 1 != 0 (SIGSEGV 1, XMA 0)", reasons)

    def test_pass_unchanged(self):
        v, reasons, m, _ = analyze(FIX / "S1-pass")
        self.assertEqual((v, reasons), ("PASS", []))
        self.assertIsNone(m["known_game_race"])

    def test_other_unmapped_read_stays_fail(self):
        v, reasons, m, _ = analyze(FIX / "S1-fail-other-unmapped-read")
        self.assertEqual(v, "FAIL")
        self.assertEqual(reasons, ["max SIGSEGV 2 != 0"])
        self.assertIsNone(m["known_game_race"])
        self.assertEqual(m["unmapped_read_count"], 2)


class Mutations(unittest.TestCase):
    """One edit of the race fixture each; every one must stay a plain FAIL."""

    def run_mutated(self, edit):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t) / "run"
            shutil.copytree(FIX / "S1-splash-race", d)
            log = d / "run.log"
            text = log.read_text()
            new = edit(text)
            self.assertNotEqual(new, text, "mutation did not apply")
            log.write_text(new)
            return analyze(d)

    def assert_fail(self, edit):
        v, reasons, m, _ = self.run_mutated(edit)
        self.assertEqual(v, "FAIL")
        self.assertNotIn(RACE, reasons)
        self.assertIsNone(m["known_game_race"])

    def test_control_identity_edit_still_matches(self):
        # the harness itself: a no-op rewrite (one extra blank line) still matches
        v, reasons, _, _ = self.run_mutated(lambda s: s + "\n")
        self.assertEqual(v, "INCONCLUSIVE")

    def test_other_lr(self):
        self.assert_fail(lambda s: s.replace("guest lr 82662B24", "guest lr 82662B28"))

    def test_other_read_address(self):
        self.assert_fail(lambda s: s.replace("unmapped guest 00000008", "unmapped guest 0000000C"))

    def test_other_crash_pc(self):
        self.assert_fail(lambda s: s.replace("crash_guest=0x82662B18", "crash_guest=0x82662B1C", 1))

    def test_second_non_xma_fault(self):
        self.assert_fail(lambda s: s.replace("XMA=0 NON_XMA=1", "XMA=0 NON_XMA=2"))

    def test_second_unmapped_read(self):
        line = next(l for l in s_lines() if "soft-fault read from unmapped" in l)
        self.assert_fail(lambda s: s.replace(line, line + line.replace("00000008 (", "00000010 (")))

    def test_after_title_screen(self):
        # the same fault, logged after title_screen
        line = next(l for l in s_lines() if "soft-fault read from unmapped" in l)
        title = "i> F8000088 DC3 Script: screen -> 'title_screen' (frame 1)\n"
        self.assert_fail(lambda s: s.replace(line, title + line))


def s_lines():
    return (FIX / "S1-splash-race" / "run.log").read_text().splitlines(keepends=True)


def verdict_json(d, verdict, reasons):
    d.mkdir(parents=True)
    (d / "verdict.json").write_text(json.dumps({
        "verdict": verdict, "reasons": reasons, "loaded": False, "measurements": {},
        "provenance": {"load": {"load1_max": 1.0, "load1_mean": 1.0}}}))


class Aggregation(unittest.TestCase):
    def make(self, t, spec):
        out = Path(t)
        for name, verdict, reasons in spec:
            verdict_json(out / "S1" / name, verdict, reasons)
        fr.aggregate("S1", out / "S1")
        return out, fr.summary(out)

    def test_retried_race_is_counted_not_passed(self):
        with tempfile.TemporaryDirectory() as t:
            out, s = self.make(t, [
                ("run-01", "INCONCLUSIVE", [RACE, "title_screen None s"]),
                ("run-01.retry1", "PASS", []),
                ("run-02", "PASS", [])])
            sc = s["scenarios"]["S1"]
            self.assertEqual(sc["verdict"], "PASS")       # two real PASSes
            self.assertEqual(sc["counts"]["PASS"], 2)
            self.assertEqual(sc["known_game_races"],
                             {"attempts": 3, "final": 0,
                              "by_race": {"splash_postprocessor_uaf": 1}})
            self.assertEqual(s["known_game_races"]["total"], 1)
            self.assertEqual(s["known_game_races"]["scenarios"]["S1"],
                             {"attempts": 3, "by_race": {"splash_postprocessor_uaf": 1}})

    def test_unretried_race_never_passes(self):
        with tempfile.TemporaryDirectory() as t:
            out, s = self.make(t, [
                ("run-01", "INCONCLUSIVE", [RACE]),
                ("run-02", "PASS", [])])
            sc = s["scenarios"]["S1"]
            self.assertEqual(sc["counts"], {"PASS": 1, "FAIL": 0, "INCONCLUSIVE": 1, "SKIPPED": 0})
            self.assertEqual(sc["verdict"], "INCONCLUSIVE")   # need 2 PASS; a race is not one
            self.assertEqual(sc["known_game_races"]["final"], 1)
            base = {"scenarios": {"S1": {"verdict": "PASS", "attempts": []}}}
            f = compare.compare(base, s)
            self.assertIn(("INCONCLUSIVE", "S1", f"PASS -> INCONCLUSIVE {sc['counts']}"), f)
            self.assertTrue(any(x[0] == "KNOWN-RACE" and "x1 of 2 attempts" in x[2] for x in f))
            # the baseline keeps the field
            cond = compare.condense(s)
            self.assertIn("known_game_races", cond["scenarios"]["S1"])

    def test_no_race_no_line(self):
        with tempfile.TemporaryDirectory() as t:
            out, s = self.make(t, [("run-01", "PASS", []), ("run-02", "PASS", [])])
            self.assertEqual(s["known_game_races"]["total"], 0)
            base = {"scenarios": {"S1": {"verdict": "PASS", "attempts": []}}}
            self.assertFalse(any(x[0] == "KNOWN-RACE" for x in compare.compare(base, s)))


if __name__ == "__main__":
    sys.exit(unittest.main())
