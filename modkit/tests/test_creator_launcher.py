"""Testing a source project cannot repair, back up or restore vanilla saves."""
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "launcher"))
import mod_packages as packages
import mod_projects as projects


class CreatorLauncher(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        launcher = self.root / "launcher"; launcher.mkdir()
        shutil.copy2(REPO / "launcher/launcher.py", launcher / "launcher.py")
        spec = importlib.util.spec_from_file_location("creator_launcher_check", launcher / "launcher.py")
        self.app = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.app)
        self.app.MODS_DIR = launcher / "mods"
        self.app.USER_DATA = self.root / "userdata"
        self.app.GAMEDATA = self.root / "gamedata"; self.app.GAMEDATA.mkdir()
        (self.app.GAMEDATA / "default.xex").write_bytes(b"fixture")
        (self.app.GAMEDATA / "movies").mkdir()
        self.app.GAME_BIN = self.root / "game"; self.app.GAME_BIN.touch()
        self.app.BUILD_DIR = self.root
        self.app.UI_DIR = REPO / "launcher/ui"
        shutil.copy2(REPO / "modkit/api.json", self.root / "simpsons-modkit.json")
        self.app.BACKUPS_DIR = self.root / "backups"
        self.app.PLAT = "Linux"
        projects.create(self.app.MODS_DIR, self.app.UI_DIR / "modkit-templates.json",
                        {"template": "health-tools", "name": "Tools", "id": "creator.tools"})
        packages.write_json(packages.profile_path(self.app.MODS_DIR), {"version": 1, "enabled": ["unrelated.mod"]})

    def test_test_launch_uses_private_profile_without_vanilla_save_operations(self):
        names = ["repair_saves", "auto_backup_saves", "start_save_guard", "retire_instant_popin_patch"]
        with mock.patch.object(self.app, "game_running_pids", return_value=[]), \
                mock.patch.object(self.app, "modkit_ready", return_value=True), \
                mock.patch.object(self.app, "write_settings"), \
                mock.patch.object(self.app, "diagnostics_enabled", return_value=False), \
                mock.patch.object(self.app.subprocess, "Popen") as popen, \
                mock.patch.object(self.app.threading, "Thread") as thread:
            from contextlib import ExitStack
            with ExitStack() as stack:
                untouched = [stack.enter_context(mock.patch.object(self.app, name)) for name in names]
                result = self.app.change_project({"action": "test", "id": "creator.tools"})
                self.assertTrue(result["ok"], result)
                for operation in untouched:
                    operation.assert_not_called()
            command = popen.call_args.args[0]
            private = self.app.USER_DATA / "mod-creator-tests/creator.tools"
            self.assertEqual(command[command.index("--user_data_root") + 1], str(private))
            self.assertEqual(command[command.index("--mods_profile_file") + 1], str(private / "mods-profile.json"))
            self.assertIn("--mods_developer=true", command)
            self.assertTrue(thread.call_args.kwargs["args"][-1])
        self.assertEqual(packages.profile(self.app.MODS_DIR)["enabled"], ["unrelated.mod"])

    def test_failed_creator_process_does_not_restore_or_stop_vanilla_cache_guard(self):
        cache = self.app.USER_DATA / "cache"; cache.mkdir(parents=True)
        (cache / "keep").write_text("player cache")
        good = self.app.BACKUPS_DIR / "goodcache"; good.mkdir(parents=True)
        (good / "replace").write_text("normal fallback")
        process = mock.Mock(); process.wait.return_value = 1
        with mock.patch.object(self.app, "stop_save_guard") as stop:
            self.app._watch_game_exit(process, isolated=True)
            stop.assert_not_called()
        self.assertEqual((cache / "keep").read_text(), "player cache")
        self.assertFalse((cache / "replace").exists())

    def test_project_edits_are_rejected_while_the_game_runs(self):
        with mock.patch.object(self.app, "game_running_pids", return_value=[42]):
            result = self.app.change_project({"action": "create", "name": "Nope", "id": "creator.nope", "template": "health-tools"})
            self.assertFalse(result["ok"])
        self.assertFalse((self.app.MODS_DIR / "creator.nope").exists())


if __name__ == "__main__":
    unittest.main()
