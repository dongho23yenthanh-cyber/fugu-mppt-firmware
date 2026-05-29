"""Host-side unit tests for the pure helpers in etc/fugu_health.py.

Covers the coredump-timestamp parsing and the health-table verdicts.

Run under the project venv (pulls in fugu_health's transport imports):
    .venv/bin/python -m pytest test/host_py/test_coredump_health.py
    .venv/bin/python test/host_py/test_coredump_health.py
"""
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "etc"))

import fugu_health as H

OK, WARN, BAD = H.OK, H.WARN, H.BAD


class TestParseCoredump(unittest.TestCase):
    def test_none(self):
        self.assertIsNone(H.parse_coredump(["coredump: none (ESP_ERR_NOT_FOUND)"]))

    def test_present_with_timestamp(self):
        cd = H.parse_coredump(["coredump: present addr=0x003d9000 size=32772 check=ok crashed=1748537400"])
        self.assertEqual(cd, {"check": "ok", "crashed": 1748537400})

    def test_present_without_timestamp(self):  # pre-feature firmware
        cd = H.parse_coredump(["coredump: present addr=0x003d9000 size=32772 check=ok"])
        self.assertEqual(cd, {"check": "ok", "crashed": 0})

    def test_present_bad_check(self):
        cd = H.parse_coredump(["coredump: present addr=0x1 size=8 check=BAD crashed=0"])
        self.assertEqual(cd["check"], "BAD")


class TestFmtAgo(unittest.TestCase):
    def test_ranges(self):
        self.assertEqual(H.fmt_ago(30), "30s ago")
        self.assertEqual(H.fmt_ago(300), "5m ago")
        self.assertEqual(H.fmt_ago(7200), "2h ago")
        self.assertEqual(H.fmt_ago(2 * 86400), "2d ago")


class TestParseStatus(unittest.TestCase):
    def test_mppt_line(self):  # exact on-wire format (glyphs/spacing matter to the regex)
        line = ("V=41.4/25.99 I=0.61/ 0.95A  25.5W 31℃38℃ 443sps  0㎅/s "
                "DCM(H|L|Lm)= 819| 520| 521 st=↑MPPT,1 lag=68131㎲ N=114777 rssi=-37")
        st = H.parse_status(line)
        self.assertAlmostEqual(st["vin"], 41.4, places=2)
        self.assertAlmostEqual(st["vout"], 25.99, places=2)
        self.assertEqual(st["watt"], 25.5)
        self.assertEqual(st["sps"], 443)
        self.assertIn("MPPT", st["st"])
        self.assertEqual(st["rssi"], -37)

    def test_garbage_returns_none(self):
        self.assertIsNone(H.parse_status("not a status line"))


class TestCoredumpRowVerdict(unittest.TestCase):
    def _row(self, cd):
        rows = H.build_rows(None, {}, {}, [], [], cd)
        return next(r for r in rows if r[0] == "Coredump")

    def test_none_is_ok(self):
        self.assertEqual(self._row(None)[2], OK)

    def test_recent_crash_is_bad(self):
        _, detail, v = self._row({"check": "ok", "crashed": int(time.time()) - 3600})
        self.assertEqual(v, BAD)
        self.assertIn("crashed", detail)

    def test_old_crash_is_warn(self):
        _, _, v = self._row({"check": "ok", "crashed": int(time.time()) - 3 * 86400})
        self.assertEqual(v, WARN)

    def test_unknown_time_is_warn(self):
        _, detail, v = self._row({"check": "ok", "crashed": 0})
        self.assertEqual(v, WARN)
        self.assertIn("unknown", detail)


if __name__ == "__main__":
    unittest.main(verbosity=2)
