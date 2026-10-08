#!/usr/bin/env python3
"""Check launcher patches on synthetic game files, without an installed game."""

import importlib.util
import io
import json
from contextlib import ExitStack
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request


class LauncherPatches(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.TemporaryDirectory(prefix="simpsons-launcher-check-")
        launcher = Path(cls.root.name) / "launcher"
        launcher.mkdir()
        source = Path(__file__).resolve().parents[2] / "launcher" / "launcher.py"
        shutil.copyfile(source, launcher / "launcher.py")
        spec = importlib.util.spec_from_file_location("patch_launcher", launcher / "launcher.py")
        cls.app = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.app)

    @classmethod
    def tearDownClass(cls):
        cls.root.cleanup()

    def setUp(self):
        self.data = tempfile.TemporaryDirectory(dir=self.root.name)
        self.addCleanup(self.data.cleanup)
        self.app.GAMEDATA = Path(self.data.name)
        self.lua = self.app.GAMEDATA / "simpsons_gameflow.lua"
        self.backup = self.lua.with_name(self.lua.name + ".original")
        self.original = self.gameflow()
        self.lua.write_bytes(self.original)

    def gameflow(self, newline=b"\r\n", final_newline=True):
        lines = [b"-- synthetic script, preserve non-UTF8 comment: \xff"]
        for episode in self.app.EPISODE_NAMES:
            lines.append(f'episode = NewEpisode(game, "{episode}")'.encode())
            if episode == "LAND_OF_CHOCOLATE":
                lines.append(b"\tepisode:SetDefault() -- original start")
        return newline.join(lines) + (newline if final_newline else b"")

    def test_every_episode_and_exact_restore(self):
        self.assertEqual(len(self.app.start_episode_options()), 18)
        for episode in self.app.EPISODE_NAMES:
            self.assertTrue(self.app.set_start_episode(episode)[0], episode)
            self.assertEqual(self.app.start_episode_state(), episode)
            text = self.app._read_lua(self.lua)
            self.assertEqual(len(self.app._SET_DEFAULT_RE.findall(text)), 1)
            self.assertNotIn(b"\n", self.lua.read_bytes().replace(b"\r\n", b""))
        self.assertEqual(self.backup.read_bytes(), self.original)
        self.assertTrue(self.app.set_start_episode("LAND_OF_CHOCOLATE")[0])
        self.assertEqual(self.lua.read_bytes(), self.original)
        self.assertFalse(self.backup.exists())

    def test_lf_and_missing_final_newline(self):
        for final in (False, True):
            original = self.gameflow(b"\n", final)
            self.lua.write_bytes(original)
            self.assertTrue(self.app.set_start_episode("MEET_THY_PLAYER")[0])
            self.assertEqual(self.app.start_episode_state(), "MEET_THY_PLAYER")
            self.assertTrue(self.app.set_start_episode("LAND_OF_CHOCOLATE")[0])
            self.assertEqual(self.lua.read_bytes(), original)

    def test_unknown_and_ambiguous_scripts_are_unchanged(self):
        self.assertFalse(self.app.set_start_episode("UNKNOWN")[0])
        self.assertEqual(self.lua.read_bytes(), self.original)
        self.assertFalse(self.backup.exists())
        for suffix in (b'\nepisode = NewEpisode(game, "SPR_HUB")\n',
                       b'\nepisode:SetDefault()\n'):
            malformed = self.original + suffix
            self.lua.write_bytes(malformed)
            self.assertFalse(self.app.set_start_episode("SPR_HUB")[0])
            self.assertEqual(self.lua.read_bytes(), malformed)
            self.assertFalse(self.backup.exists())

    def test_external_edits_and_backup_are_kept(self):
        self.assertTrue(self.app.set_start_episode("SPR_HUB")[0])
        modified = self.lua.read_bytes() + b"-- another mod\r\n"
        self.lua.write_bytes(modified)
        self.assertFalse(self.app.set_start_episode("LAND_OF_CHOCOLATE")[0])
        self.assertEqual(self.lua.read_bytes(), modified)
        self.assertEqual(self.backup.read_bytes(), self.original)

    def test_failed_replace_preserves_original_and_can_retry(self):
        with mock.patch.object(self.app.os, "replace", side_effect=OSError("test write failure")):
            self.assertFalse(self.app.set_start_episode("SPR_HUB")[0])
        self.assertEqual(self.lua.read_bytes(), self.original)
        self.assertEqual(self.backup.read_bytes(), self.original)
        self.assertFalse(list(self.lua.parent.glob("*.tmp")))
        self.assertTrue(self.app.set_start_episode("SPR_HUB")[0])
        self.assertTrue(self.app.set_start_episode("LAND_OF_CHOCOLATE")[0])
        self.assertEqual(self.lua.read_bytes(), self.original)

    def test_concurrent_episode_requests(self):
        results = []
        threads = [threading.Thread(target=lambda e=e: results.append(self.app.set_start_episode(e)))
                   for e in self.app.EPISODE_NAMES]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(len(results), 18)
        self.assertTrue(all(okay for okay, _ in results), results)
        self.assertTrue(self.app.set_start_episode("LAND_OF_CHOCOLATE")[0])
        self.assertEqual(self.lua.read_bytes(), self.original)

    def test_logo_languages_case_and_unrelated_movies(self):
        movies = self.app.GAMEDATA / "Movies"
        originals = {}
        for language in ("de", "en", "fr"):
            folder = movies / language
            folder.mkdir(parents=True)
            for name in (*self.app.LOGO_MOVIES, "level_intro"):
                path = folder / (name.upper() + ".VP6")
                originals[path] = (language + name).encode()
                path.write_bytes(originals[path])
        self.assertTrue(self.app.patch_skip_intro(True)[0])
        self.assertEqual(self.app.patch_skip_intro_state(), "on")
        self.assertTrue(all((movies / lang / "LEVEL_INTRO.VP6").is_file() for lang in ("de", "en", "fr")))
        self.assertTrue(self.app.patch_skip_intro(False)[0])
        self.assertEqual(self.app.patch_skip_intro_state(), "off")
        for path, data in originals.items():
            self.assertEqual(path.read_bytes(), data)

    def test_logo_collision_does_not_overwrite(self):
        folder = self.app.GAMEDATA / "movies" / "de"
        folder.mkdir(parents=True)
        name = self.app.LOGO_MOVIES[0]
        enabled = folder / (name + ".vp6")
        disabled = folder / (name.upper() + ".VP6.DISABLED")
        enabled.write_bytes(b"active copy")
        disabled.write_bytes(b"disabled copy")
        for enable in (True, False):
            self.assertFalse(self.app.patch_skip_intro(enable)[0])
            self.assertEqual(enabled.read_bytes(), b"active copy")
            self.assertEqual(disabled.read_bytes(), b"disabled copy")

    def test_saved_settings_keep_native_paths_and_visual_choices(self):
        self.app.GAME_TOML = self.app.GAMEDATA / "simpsons.toml"
        self.app.capture_keys = lambda: {}
        self.app.GAME_TOML.write_text("native_resolve_copy_free = false\nnative_buffer_write_watches = false\n")
        self.app.write_settings({"gpu": "d3d11", "ink_outlines": "soft",
                                 "ink_outline_color": "FF8000", "ink_outline_strength": 0.8,
                                 "eye_shading": "original", "audio_maxqframes": 16})
        values = self.app.read_settings()
        for key, value in {"gpu": "d3d11", "ink_outlines": "soft", "ink_outline_color": "FF8000",
                           "ink_outline_strength": 0.8, "eye_shading": "original", "audio_maxqframes": 16}.items():
            self.assertEqual(values[key], value)
        text = self.app.GAME_TOML.read_text()
        self.assertIn("native_resolve_copy_free = false", text)
        self.assertIn("native_buffer_write_watches = false", text)

    def test_old_refresh_rate_setting_becomes_frame_rate(self):
        self.app.GAME_TOML = self.app.GAMEDATA / "simpsons.toml"
        self.app.capture_keys = lambda: {}
        for refresh, frame_rate in ((None, 60), ("30.0", 30), ("60.0", 60), ("90.0", 90),
                                    ("120.0", 120)):
            with self.subTest(refresh=refresh):
                text = "resolution_scale = 2\n"
                if refresh:
                    text += (f"{self.app.SETTINGS_BEGIN}\nvideo_mode_refresh_rate = {refresh}\n"
                             f"{self.app.SETTINGS_END}\n")
                self.app.GAME_TOML.write_text(text)
                self.assertEqual(self.app.read_settings()["frame_rate"], frame_rate)
                self.app.write_settings({})
                text = self.app.GAME_TOML.read_text()
                self.assertNotIn("video_mode_refresh_rate", text)
                self.assertIn(f"frame_rate = {frame_rate}", text)
                self.assertIn("menu_frame_rate = 30", text)
                self.assertIn("resolution_scale = 2", text)
                self.assertEqual(self.app.read_settings()["frame_rate"], frame_rate)
        self.app.write_settings({"frame_rate": 0, "menu_frame_rate": 0})
        values = self.app.read_settings()
        self.assertEqual((values["frame_rate"], values["menu_frame_rate"]), (0, 0))

    def test_launch_errors_return_http_json_and_close_diagnostics(self):
        (self.app.GAMEDATA / "default.xex").write_bytes(b"synthetic game image")
        (self.app.GAMEDATA / "movies").mkdir()
        server = self.app.LauncherServer(("127.0.0.1", 0), self.app.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for error in (FileNotFoundError(2, "No such file or directory", "simpsons"),
                          PermissionError(13, "Permission denied", "simpsons"),
                          OSError(8, "Exec format error", "simpsons")):
                with self.subTest(error=error), ExitStack() as stack:
                    for name, value in {"game_running_pids": [], "retire_instant_popin_patch": None,
                                        "installed_build_variant": None, "repair_saves": [],
                                        "auto_backup_saves": None, "write_settings": {},
                                        "diagnostics_enabled": True}.items():
                        stack.enter_context(mock.patch.object(self.app, name, return_value=value))
                    stack.enter_context(mock.patch.object(self.app, "PLAT", "Linux"))
                    stack.enter_context(mock.patch.object(self.app, "BUILD_DIR", self.app.GAMEDATA))
                    guard = stack.enter_context(mock.patch.object(self.app, "start_save_guard"))
                    diag = stack.enter_context(mock.patch.object(self.app, "DiagSession")).return_value
                    output = io.StringIO()
                    diag.game_output_handle.return_value = output
                    stack.enter_context(mock.patch.object(self.app.subprocess, "Popen", side_effect=error))
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{server.server_port}/api/launch", data=b"{}",
                        headers={"X-Token": self.app.TOKEN, "Content-Type": "application/json"})
                    with self.assertRaises(urllib.error.HTTPError) as caught:
                        urllib.request.urlopen(request, timeout=5)
                    with caught.exception as response:
                        self.assertEqual(response.code, 409)
                        body = json.load(response)
                    self.assertFalse(body["ok"])
                    self.assertIn(str(error), body["msg"])
                    self.assertTrue(output.closed)
                    guard.assert_not_called()
                    diag.finalize.assert_called_once_with(None, [body["msg"]])
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
