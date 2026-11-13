#!/usr/bin/env python3
"""Check progress arithmetic and rejection of misleading or incomplete evidence."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from native_coverage import ALLOCATIONS, DEPENDENCIES
from native_progress import OUTCOMES, ROUTE_SYSTEMS, bar, draw_share, read_sample, route_share, summarize


class ProgressCheck(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "sample.json"
        self.sample = {
            "version": 1, "backend": "vulkan", "strict": False,
            "complete": True, "failed": False, "frames": 1,
            "operations": {"native_draw_call": 3, "legacy_draw_packet": 1},
            "dependencies": {name: {"calls": 0, "bytes": 0} for name in DEPENDENCIES},
            "allocations": {name: {"calls": 0, "virtual_bytes": 0, "resident_bytes": 0}
                            for name in ALLOCATIONS},
            "route_coverage": {"version": 1, "systems": {
                name: dict.fromkeys((*OUTCOMES, "attempts", "in_flight"), 0)
                for name in ROUTE_SYSTEMS.values()}},
        }

    def row(self, sample=None):
        self.path.write_text(json.dumps(self.sample if sample is None else sample))
        return read_sample(self.path)

    def outcomes(self, system="draw_dispatch", **values):
        counter = self.sample["route_coverage"]["systems"][system]
        counter.update(values)
        counter["attempts"] = sum(counter[key] for key in (*OUTCOMES, "in_flight"))
        return counter

    def test_native_retained_share(self):
        self.outcomes(native=3, retained=1)
        row = self.row()
        self.assertEqual(row["errors"], [])
        self.assertEqual(draw_share([row])["percent"], 75)

    def test_mixed_and_failures_stay_in_denominator(self):
        self.outcomes(native=3, retained=1, mixed=1, failed=1, skipped=7)
        result = draw_share([self.row()])
        self.assertEqual((result["percent"], result["executed"], result["attempts"]), (50, 6, 13))

    def test_unclassified_prevents_percentage(self):
        self.outcomes(native=3, unclassified=1)
        self.assertIsNone(draw_share([self.row()])["percent"])

    def test_unexercised_is_not_full_native(self):
        result = draw_share([self.row()])
        self.assertIsNone(result["percent"])
        self.assertIn("not exercised", result["reason"])

    def test_only_skipped_is_not_full_native(self):
        self.outcomes(skipped=7)
        self.assertIsNone(draw_share([self.row()])["percent"])

    def test_balanced_in_flight_is_still_incomplete(self):
        self.outcomes(native=3, in_flight=1)
        row = self.row()
        self.assertTrue(row["errors"])
        self.assertIsNone(draw_share([row])["percent"])

    def test_inconsistent_attempts_rejected(self):
        self.outcomes(native=3)["attempts"] = 4
        self.assertTrue(self.row()["errors"])

    def test_invalid_numeric_types_rejected(self):
        counter = self.outcomes()
        for value in (-1, True, 1.5, "3", None):
            with self.subTest(value=value):
                counter["native"] = value
                self.assertTrue(self.row()["errors"])

    def test_missing_counter_rejected(self):
        del self.sample["route_coverage"]["systems"]["textures"]["mixed"]
        self.assertTrue(self.row()["errors"])

    def test_missing_or_unrecognized_system_rejected(self):
        for name in ("geometry", "additional_system"):
            with self.subTest(name=name):
                sample = copy.deepcopy(self.sample)
                if name == "geometry":
                    del sample["route_coverage"]["systems"][name]
                else:
                    sample["route_coverage"]["systems"][name] = {}
                self.assertTrue(self.row(sample)["errors"])

    def test_report_must_finish(self):
        for name, value in (("complete", False), ("failed", True), ("frames", 0)):
            with self.subTest(name=name):
                sample = copy.deepcopy(self.sample)
                sample[name] = value
                row = self.row(sample)
                self.assertTrue(row["errors"])
                self.assertIsNone(draw_share([row])["percent"])

    def test_schema_versions_are_strict(self):
        for value in (True, 2, "1", None):
            with self.subTest(value=value):
                self.sample["route_coverage"]["version"] = value
                self.assertTrue(self.row()["errors"])

    def test_old_report_keeps_its_own_draw_denominator(self):
        del self.sample["route_coverage"]
        row = self.row()
        self.assertEqual(draw_share([row])["percent"], 75)
        self.assertIsNone(route_share([row], "geometry")["percent"])

    def test_mixed_generations_never_drop_missing_samples(self):
        self.outcomes(native=3, retained=1)
        new = self.row()
        old = copy.deepcopy(self.sample)
        del old["route_coverage"]
        self.assertIsNone(draw_share([new, self.row(old)])["percent"])

    def test_report_weighting_uses_attempts(self):
        self.outcomes(native=1)
        first = self.row()
        self.outcomes(native=0, retained=3)
        result = draw_share([first, self.row()])
        self.assertEqual((result["percent"], result["executed"]), (25, 4))

    def test_near_full_ratio_never_rounds_to_full(self):
        self.outcomes(native=2**64 - 1, retained=1)
        value = draw_share([self.row()])["percent"]
        self.assertLess(value, 100)
        self.assertIn("<100%", bar(value))
        self.assertNotEqual(bar(value).split()[0], "█" * 20)

    def test_no_overall_or_campaign_claim(self):
        self.outcomes(native=3)
        summary = summarize([self.row()], "synthetic arithmetic check only")
        self.assertIsNone(summary["overall_completion_percent"])
        self.assertFalse(summary["full_game_native_proven"])
        self.assertFalse(summary["current_build_qualified"])

    def test_portable_snapshot_preserves_counts_and_hash(self):
        self.outcomes(native=3, retained=1)
        row = self.row()
        output = self.path.parent / "snapshot.json"
        result = subprocess.run([
            sys.executable, str(Path(__file__).with_name("native_progress.py")),
            str(self.path), "--scope", "synthetic CLI check",
            "--relative-to", str(self.path.parent), "--output", str(output),
        ], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        snapshot = json.loads(output.read_text())
        self.assertEqual(snapshot["samples"][0]["path"], "sample.json")
        self.assertEqual(snapshot["samples"][0]["sha256"], row["sha256"])
        self.assertEqual(snapshot["systems"]["Draw command dispatch"]["percent"], 75)
        self.assertFalse(snapshot["current_build_qualified"])

    def test_relative_snapshot_rejects_outside_or_missing_root(self):
        self.row()
        for root in (self.path.parent / "missing", self.path.parent / "other"):
            if root.name == "other":
                root.mkdir()
            with self.subTest(root=root):
                result = subprocess.run([
                    sys.executable, str(Path(__file__).with_name("native_progress.py")),
                    str(self.path), "--scope", "synthetic CLI rejection",
                    "--relative-to", str(root),
                ], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
