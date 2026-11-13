"""Creator command behavior against bounded session replies."""
import contextlib
import argparse
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/modkit"))
import developer
import packages


class ReloadCommandTests(unittest.TestCase):
    def execute(self, replies, cancel=False, clock=None):
        args = SimpleNamespace(developer_command="reload", cancel=cancel,
                               session="private/session.json", timeout=1)
        output = io.StringIO()
        with mock.patch.object(developer, "request", side_effect=replies) as request, \
                mock.patch.object(developer.time, "sleep"), \
                contextlib.redirect_stdout(output):
            if clock:
                with mock.patch.object(developer.time, "monotonic", side_effect=clock):
                    developer.execute(args, None, None)
            else:
                developer.execute(args, None, None)
        return json.loads(output.getvalue()), request.call_args_list

    def test_waits_without_repeating_reload(self):
        result, calls = self.execute([
            {"state": "preparing", "generation": 3},
            {"reload": {"state": "ready", "generation": 3}},
            {"reload": {"state": "applied", "generation": 3}},
        ])
        self.assertEqual(result["state"], "applied")
        self.assertEqual([call.args[1]["action"] for call in calls],
                         ["reload", "inspect", "inspect"])

    def test_preflight_failure_is_reported(self):
        with self.assertRaisesRegex(packages.ModError, "restart_required: Changed data"):
            self.execute([{"state": "preparing", "generation": 1},
                          {"reload": {"state": "failed", "generation": 1,
                                      "code": "restart_required", "error": "Changed data"}}])

    def test_cancel_does_not_start_another_job(self):
        result, calls = self.execute([{"state": "cancelled", "generation": 2}], cancel=True)
        self.assertEqual(result["state"], "cancelled")
        self.assertEqual([call.args[1]["action"] for call in calls], ["cancel_reload"])

    def test_replaced_generation_is_not_claimed(self):
        with self.assertRaisesRegex(packages.ModError, "Another reload"):
            self.execute([{"state": "preparing", "generation": 1},
                          {"reload": {"state": "applied", "generation": 2}}])

    def test_timeout_leaves_the_accepted_job_visible(self):
        result, calls = self.execute([{"state": "preparing", "generation": 1}], clock=[10, 12])
        self.assertEqual(result["state"], "preparing")
        self.assertEqual(len(calls), 1)


class SourceCommandTests(unittest.TestCase):
    def args(self, action, **values):
        return SimpleNamespace(source_command=action, session="private/session.json",
                               owner="creator.library", timeout=2, wait=False,
                               stream="brt/brt/challenge_mode/challenge_mode_design.str", **values)

    def state(self, phase="ready", owned=True, **values):
        return {"phase": phase, "owned": owned, "ready": phase == "ready" and owned,
                "stream": "brt/brt/challenge_mode/challenge_mode_design.str" if owned else "",
                "loaded": 3 if phase == "ready" else 0, "files": 3,
                "definitions": 343, "message": "Objects ready", **values}

    def test_load_wait_only_requests_the_source_once(self):
        args = self.args("load")
        args.wait = True
        output = io.StringIO()
        with mock.patch.object(developer, "request", side_effect=[True, self.state("loading"), self.state()]) as request, \
                mock.patch.object(developer.time, "sleep"), contextlib.redirect_stderr(output):
            result = developer.source(args)
        self.assertTrue(result["ready"])
        self.assertIn("0/3 native files", output.getvalue())
        self.assertEqual([c.args[1]["method"] for c in request.call_args_list],
                         ["assets.load_source", "assets.source_status", "assets.source_status"])
        self.assertTrue(all(c.args[1]["owner"] == args.owner for c in request.call_args_list))

    def test_dependency_status_keeps_the_primary_path(self):
        with mock.patch.object(developer, "request", return_value=self.state()):
            result = developer.source(self.args("status"))
        self.assertEqual(result["stream"], "brt/brt/challenge_mode/challenge_mode_design.str")

    def test_release_reports_a_shared_source_without_waiting_for_other_owners(self):
        with mock.patch.object(developer, "request", side_effect=[True, self.state(owned=False)]) as request:
            result = developer.source(self.args("release"))
        self.assertTrue(result["released"])
        self.assertFalse(result["owned"])
        self.assertEqual(result["phase"], "ready")
        self.assertEqual(len(request.call_args_list), 2)

    def test_cancel_withdraws_once_and_reports_the_preserved_source(self):
        with mock.patch.object(developer, "request", side_effect=[True, self.state()]) as request:
            result = developer.source(self.args("cancel"))
        self.assertTrue(result["canceled"])
        self.assertTrue(result["owned"])
        self.assertTrue(result["ready"])
        self.assertEqual([c.args[1]["method"] for c in request.call_args_list],
                         ["assets.cancel_source", "assets.source_status"])

    def test_failed_preparation_reports_the_native_message(self):
        state = self.state("failed", owned=False)
        state["message"] = "The source stream is no longer installed"
        with mock.patch.object(developer, "request", return_value=state), \
                self.assertRaisesRegex(packages.ModError, "source_failed: The source stream"):
            developer.source(self.args("wait"))

    def test_timeout_does_not_release_or_repeat_an_accepted_load(self):
        args = self.args("load")
        args.wait = True
        with mock.patch.object(developer, "request", return_value=True) as request, \
                mock.patch.object(developer.time, "monotonic", side_effect=[10, 10, 13]), \
                self.assertRaisesRegex(packages.ModError, "Accepted work has not been cancelled"):
            developer.source(args)
        self.assertEqual([c.args[1]["method"] for c in request.call_args_list], ["assets.load_source"])

    def test_ambiguous_mutation_timeout_is_not_retried(self):
        with mock.patch.object(developer, "request", side_effect=packages.ModError("Creator request timed out")) as request, \
                self.assertRaisesRegex(packages.ModError, "Creator request timed out"):
            developer.source(self.args("load"))
        self.assertEqual(request.call_count, 1)

    def test_invalid_timeout_rejects_before_mutating_the_game(self):
        args = self.args("load")
        args.timeout = 31
        with mock.patch.object(developer, "request") as request, self.assertRaises(packages.ModError):
            developer.source(args)
        request.assert_not_called()

    def test_parser_requires_an_owner_and_preserves_a_path_with_spaces(self):
        parser = argparse.ArgumentParser()
        developer.register(parser.add_subparsers())
        args = parser.parse_args(["dev", "--session", "private/session.json", "source", "load",
                                  "brt/folder with spaces/source.str", "--owner", "creator.library", "--wait"])
        self.assertEqual(args.stream, "brt/folder with spaces/source.str")
        self.assertEqual(args.owner, "creator.library")
        self.assertTrue(args.wait)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parser.parse_args(["dev", "--session", "private/session.json", "source", "release"])


if __name__ == "__main__":
    unittest.main()
