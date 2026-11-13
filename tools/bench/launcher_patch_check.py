#!/usr/bin/env python3
"""Check launcher patches on synthetic game files, without an installed game."""

import importlib.util
import io
import json
import os
from contextlib import ExitStack
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import unittest
from unittest import mock
import subprocess
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
        # Only launcher.py is copied, so its settings land in the temporary
        # folder; the modules it imports come from the real launcher folder.
        if str(source.parent) not in sys.path:
            sys.path.insert(0, str(source.parent))
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

    # Add to Steam builds a steam:// URL from a POSIX launcher path; it exists
    # only on Linux and the Steam Deck.
    @unittest.skipIf(os.name == "nt", "Linux-only: Add to Steam uses POSIX launcher paths")
    def test_steam_request_encodes_paths_and_reports_failure(self):
        directory = Path(self.data.name) / "launcher with spaces"
        directory.mkdir()
        script = directory / "simpsons-launcher.sh"
        script.write_text("#!/bin/sh\nexit 0\n")
        with mock.patch.object(self.app, "LAUNCHER_DIR", directory), \
                mock.patch.object(self.app, "PLAT", "Linux"), \
                mock.patch.object(self.app, "install_desktop_entry", return_value=directory / "The Simpsons.desktop"), \
                mock.patch.object(self.app.steam_shortcut, "existing_shortcut", side_effect=[None, "The Simpsons Game", None]), \
                mock.patch.object(self.app.shutil, "which", side_effect=lambda name: "/usr/bin/steam" if name == "steam" else None), \
                mock.patch.object(self.app, "_run_steam_command", return_value=subprocess.CompletedProcess([], 0, "", "")) as run, \
                mock.patch.object(Path, "touch") as touch:
            self.assertTrue(self.app.add_to_steam()[0])
            url = run.call_args.args[0][1]
            self.assertTrue(url.startswith("steam://addnonsteamgame/%2F"))
            self.assertIn("launcher%20with%20spaces", url)
            touch.assert_called_once()
            run.return_value = subprocess.CompletedProcess([], 1, "", "Steam is unavailable")
            okay, message = self.app.add_to_steam()
            self.assertFalse(okay)
            self.assertIn("Steam is unavailable", message)

    def test_steam_existing_shortcut_is_not_added_twice(self):
        directory = Path(self.data.name)
        (directory / "simpsons-launcher.sh").write_text("#!/bin/sh\nexit 0\n")
        with mock.patch.object(self.app, "LAUNCHER_DIR", directory), \
                mock.patch.object(self.app, "PLAT", "Linux"), \
                mock.patch.object(self.app, "install_desktop_entry", return_value=directory / "game.desktop"), \
                mock.patch.object(self.app.steam_shortcut, "existing_shortcut", return_value="The Simpsons Game"), \
                mock.patch.object(self.app, "_run_steam_command") as run:
            okay, message = self.app.add_to_steam()
            self.assertTrue(okay)
            self.assertIn("Already in Steam", message)
            run.assert_not_called()

    def test_starting_steam_is_not_killed_on_request_timeout(self):
        with mock.patch.object(self.app.subprocess, "Popen") as popen:
            process = popen.return_value
            process.wait.side_effect = subprocess.TimeoutExpired(["steam"], 5)
            self.assertIsNone(self.app._run_steam_command(["steam"], {}))
            process.kill.assert_not_called()
            process.terminate.assert_not_called()

    def test_extractor_runtime_does_not_change_other_child_environments(self):
        directory = Path(self.data.name)
        runtime = directory / "extract-xiso-runtime"
        runtime.mkdir()
        (runtime / "VCRUNTIME140.dll").write_bytes(b"test DLL")
        with mock.patch.object(self.app, "PLAT", "Windows"), \
                mock.patch.object(self.app, "EXTRACT_XISO", directory / "extract-xiso.exe"), \
                mock.patch.dict(self.app.os.environ, {"PATH": "original"}):
            self.assertTrue(self.app.extractor_environment()["PATH"].startswith(str(runtime)))
            self.assertEqual(self.app.external_environment()["PATH"], "original")

    # The Linux bundle's library paths only; Windows also upper-cases
    # environment names, so the forced-Linux comparison cannot hold there.
    @unittest.skipIf(os.name == "nt", "Linux-only: the bundled launcher's LD_LIBRARY_PATH handling")
    def test_bundled_launcher_keeps_private_libraries_out_of_child_processes(self):
        with mock.patch.object(self.app, "FROZEN", True), \
                mock.patch.object(self.app, "PLAT", "Linux"), \
                mock.patch.object(self.app.sys, "_MEIPASS", "/tmp/launcher-bundle", create=True), \
                mock.patch.dict(self.app.os.environ, {
                    "LD_LIBRARY_PATH": "/tmp/launcher-bundle",
                    "LD_LIBRARY_PATH_ORIG": "/player/libs",
                    "QT_PLUGIN_PATH": "/tmp/launcher-bundle/PySide6/Qt/plugins",
                    "SteamAppId": "1234"}, clear=True):
            child = self.app.external_environment()
            self.assertEqual(child["LD_LIBRARY_PATH"], "/player/libs")
            self.assertNotIn("QT_PLUGIN_PATH", child)
            self.assertEqual(child["SteamAppId"], "1234")
            self.assertEqual(self.app.os.environ["LD_LIBRARY_PATH"], "/tmp/launcher-bundle")

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

    def test_start_episode_progress_chain(self):
        # Earlier episodes count as done through the game's own episode cheat,
        # chained to the chosen episode's start (after frankyfife's fork).
        cases = {"TREEHUGGER": ("SCORE_EVENT_TREEHUGGER_START", "SCORE_EVENT_TREEHUGGER_CHEAT"),
                 "MEET_THY_PLAYER": ("SCORE_EVENT_MEET_THY_PLAYER_START", "SCORE_EVENT_MEET_THY_PLAYER_CHEAT"),
                 "SPR_HUB": ("SCORE_EVENT_RESTART_GAME_TIME", "SCORE_EVENT_BARTMAN_BEGINS_CHEAT")}
        for episode, (trigger, cheat) in cases.items():
            self.assertTrue(self.app.set_start_episode(episode)[0], episode)
            text = self.app._read_lua(self.lua)
            self.assertIn(f'launcherStart:FindEvent("{trigger}")', text)
            self.assertIn(f'launcherStart:AddEventOp("{cheat}")', text)
            self.assertEqual(text.count("launcherStart:AddEventOp"), 1)
        # The video game hub shares Springfield's start event, so it gets no chain.
        self.assertTrue(self.app.set_start_episode("GAME_HUB")[0])
        self.assertNotIn("launcherStart", self.app._read_lua(self.lua))
        self.assertTrue(self.app.set_start_episode("LAND_OF_CHOCOLATE")[0])
        self.assertEqual(self.lua.read_bytes(), self.original)

    def test_start_episode_written_by_an_older_launcher(self):
        # Launchers before 0.0.7.0 wrote the same move without the progress
        # chain. Such a file is still the launcher's own: it can be changed and
        # restored instead of being refused as an external edit.
        original = self.app._read_lua(self.lua)
        self.backup.write_bytes(self.original)
        self.app._write_lua(self.lua, self.app._episode_text(original, "TREEHUGGER", progress=False))
        self.assertEqual(self.app.start_episode_state(), "TREEHUGGER")
        self.assertTrue(self.app.set_start_episode("MOB_RULES")[0])
        self.assertIn('AddEventOp("SCORE_EVENT_MOB_RULES_CHEAT")', self.app._read_lua(self.lua))
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
        self.app.GAME_TOML.write_text(
            "native_resolve_copy_free = false\nnative_buffer_write_watches = false\n"
            f"{self.app.SETTINGS_BEGIN}\n{self.app.SETTINGS_VERSION_MARKER} "
            f"{self.app.SETTINGS_VERSION}\n{self.app.SETTINGS_END}\n")
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

    def test_unqualified_resolve_shortcuts_turn_off_once(self):
        self.app.GAME_TOML = self.app.GAMEDATA / "simpsons.toml"
        self.app.capture_keys = lambda: {}
        for platform in ("Linux", "Windows"):
            with self.subTest(platform=platform), mock.patch.object(self.app, "PLAT", platform):
                # The texture-glitch report includes v4 with copy-free enabled.
                self.app.GAME_TOML.write_text(
                    "native_resolve_copy_free = true\nnative_buffer_write_watches = true\n"
                    "native_vertex_buffers = true\n"
                    f"{self.app.SETTINGS_BEGIN}\n{self.app.SETTINGS_VERSION_MARKER} 4\n"
                    f"{self.app.SETTINGS_END}\n")
                self.app.write_settings({"resolution_scale": 2, "audio_maxqframes": 16})
                text = self.app.GAME_TOML.read_text()
                for name in ("native_resolve_copy_free", "native_buffer_write_watches"):
                    self.assertIn(f"{name} = false", text)
                    self.assertNotIn(f"{name} = true", text)
                self.assertIn("native_vertex_buffers = true", text)
                self.assertEqual(self.app.read_settings()["resolution_scale"], 2)
                self.assertEqual(self.app.read_settings()["audio_maxqframes"], 16)
                # A later deliberate developer override is not a migration loop.
                self.app.GAME_TOML.write_text(text.replace("native_resolve_copy_free = false",
                                                           "native_resolve_copy_free = true"))
                self.app.write_settings({})
                self.assertIn("native_resolve_copy_free = true", self.app.GAME_TOML.read_text())

    def test_runtime_defaults_on_linux_and_windows(self):
        self.app.GAME_TOML = self.app.GAMEDATA / "simpsons.toml"
        self.app.capture_keys = lambda: {}
        for platform, timer_sleep in (("Linux", True), ("Windows", False)):
            with self.subTest(platform=platform), mock.patch.object(self.app, "PLAT", platform):
                self.app.GAME_TOML.write_text("native_vertex_buffers = false\n")
                self.app.write_settings({})
                text = self.app.GAME_TOML.read_text()
                self.assertIn(f"# {platform} runtime defaults", text)
                self.assertIn("native_index_buffers = true", text)
                self.assertIn("native_gpu_calls = true", text)
                self.assertIn("native_resolve_copy_free = false", text)
                self.assertTrue(self.app.read_settings()["native_gpu_calls"])
                # A saved value stays.
                self.assertIn("native_vertex_buffers = false", text)
                self.assertNotIn("native_vertex_buffers = true", text)
                self.assertEqual("timer_queue_sleep = true" in text, timer_sleep)
                # Configs from before v6 migrate native_gpu_calls = false once,
                # while keeping an explicit v6 false override.
                self.app.GAME_TOML.write_text(
                    "native_gpu_calls = false\n"
                    f"{self.app.SETTINGS_BEGIN}\n{self.app.SETTINGS_VERSION_MARKER} 5\n"
                    f"{self.app.SETTINGS_END}\n")
                self.assertTrue(self.app.read_settings()["native_gpu_calls"])
                self.app.write_settings({"native_gpu_calls": False})
                self.assertFalse(self.app.read_settings()["native_gpu_calls"])

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

    def test_cutscene_patch_persists_without_editing_game_files(self):
        self.app.GAME_TOML = self.app.GAMEDATA / "simpsons.toml"
        self.app.capture_keys = lambda: {}
        self.assertTrue(self.app.read_settings()["cutscene_skip"])
        server = self.app.LauncherServer(("127.0.0.1", 0), self.app.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def request(enable):
            req = urllib.request.Request(
                f"http://127.0.0.1:{server.server_port}/api/patch",
                data=json.dumps({"id": "cutscene_skip", "enable": enable}).encode(),
                headers={"X-Token": self.app.TOKEN, "Content-Type": "application/json"})
            return urllib.request.urlopen(req, timeout=5)

        try:
            with mock.patch.object(self.app, "game_running_pids", return_value=[]):
                for enabled in (False, True):
                    with request(enabled) as response:
                        self.assertTrue(json.load(response)["ok"])
                    self.assertEqual(self.app.read_settings()["cutscene_skip"], enabled)
                    patch = next(p for p in self.app.patches_list() if p["id"] == "cutscene_skip")
                    self.assertEqual(patch["state"], "on" if enabled else "off")
                    self.assertTrue(patch["available"])
                saved = self.app.GAME_TOML.read_bytes()
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    request("false")
                self.assertEqual(caught.exception.code, 400)
                caught.exception.close()
            with mock.patch.object(self.app, "game_running_pids", return_value=[123]):
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    request(False)
                self.assertEqual(caught.exception.code, 409)
                caught.exception.close()
            self.assertEqual(self.app.GAME_TOML.read_bytes(), saved)
            self.assertEqual(self.lua.read_bytes(), self.original)
            self.assertFalse(self.backup.exists())
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

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
