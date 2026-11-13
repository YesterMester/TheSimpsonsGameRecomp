#!/usr/bin/env python3
"""Check updating an installed Linux package, without a network or a game.

Builds a stand-in for this version's Linux package from the launcher sources,
updates an older installation the way the 0.0.6.5 launcher does (it copies
only the files it knew about), then checks that the new launcher still
starts, reports what is missing and installs it from the same release.
Also checks how the example mods are put into the mods folder.
"""

import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
# What the launcher in 0.0.6.5 and earlier copies from a Linux package.
OLD_MANAGED = ["simpsons", "extract-xiso", "launcher/ui", "launcher/launcher.py",
               "launcher/simpsons-launcher.sh", "README.md", "build_variant.txt"]
OLD_GLOBS = ["*.so*"]


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_package(folder):
    """The layout release.yml packages, with stand-ins for the binaries."""
    launcher = folder / "launcher"
    shutil.copytree(ROOT / "launcher/ui", launcher / "ui",
                    ignore=shutil.ignore_patterns("native_shaders", "__pycache__", "modules"))
    modules = launcher / "ui/modules"
    modules.mkdir()
    for source in sorted((ROOT / "launcher").glob("*.py")):
        shutil.copy2(source, launcher / source.name)
        if source.name != "launcher.py":
            shutil.copy2(source, modules / source.name)
    for name in ("simpsons-launcher.sh", "simpsons-modkit.sh"):
        shutil.copy2(ROOT / "launcher" / name, launcher / name)
    (launcher / "licenses").mkdir()
    (launcher / "licenses/versions.txt").write_text("stand-in\n")
    for name in ("simpsons", "modkit_streams", "extract-xiso", "librexruntimerd.so",
                 "launcher/simpsons-launcher", "launcher/simpsons-modkit", "Play.sh", "Modkit.sh"):
        (folder / name).write_text("new " + name + "\n")
        (folder / name).chmod(0o755)
    shutil.copy2(ROOT / "modkit/api.json", folder / "simpsons-modkit.json")
    (folder / "README.md").write_text("new readme\n")
    (folder / "build_variant.txt").write_text("x86-64-v3\n")
    (launcher / "launcher.json").write_text('{"engine": {"Linux": "simpsons"}, "extract_xiso": '
                                            '{"Linux": "extract-xiso"}, "lib_dirs": {"Linux": ["."]}}\n')


def old_update(package, install):
    for rel in OLD_MANAGED:
        source, target = package / rel, install / rel
        if source.is_dir():
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(source, target)
        elif source.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    for pattern in OLD_GLOBS:
        for source in package.glob(pattern):
            shutil.copy2(source, install / source.name)


class Reply(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class UpdateFromOlderLauncher(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="simpsons-update-check-")
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name)
        self.package = base / "package"
        self.package.mkdir()
        build_package(self.package)
        self.archive = base / "TheSimpsonsGame-Recompiled-Linux-x64.tar.gz"
        with tarfile.open(self.archive, "w:gz") as archive:
            archive.add(self.package, arcname=".")
        # A 0.0.6.5 installation with the player's own files in it.
        self.install = base / "install"
        (self.install / "launcher/ui").mkdir(parents=True)
        (self.install / "launcher/mods/my.mod").mkdir(parents=True)
        (self.install / "launcher/mods/my.mod/keep.txt").write_text("mine\n")
        (self.install / "launcher/launcher.json").write_text(
            '{"engine": {"Linux": "simpsons"}, "extract_xiso": {"Linux": "extract-xiso"}, '
            '"lib_dirs": {"Linux": ["."]}, "ui_theme": "light"}\n')
        (self.install / "simpsons.toml").write_text("frame_rate = 90\n")
        for name in ("simpsons", "extract-xiso", "librexruntimerd.so", "launcher/launcher.py",
                     "launcher/simpsons-launcher.sh", "launcher/ui/index.html", "Play.sh"):
            (self.install / name).write_text("old\n")
        (self.install / "build_variant.txt").write_text("x86-64-v3\n")
        old_update(self.package, self.install)
        for name in list(sys.modules):
            if name.split(".")[0] in ("mod_packages", "mod_projects", "modkit_process", "steam_shortcut"):
                del sys.modules[name]
        self.path = list(sys.path)
        self.addCleanup(lambda: sys.path.__setitem__(slice(None), self.path))
        sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != (ROOT / "launcher").resolve()]
        with mock.patch("platform.system", return_value="Linux"):
            self.app = load(self.install / "launcher/launcher.py", "updated_launcher")

    def urlopen(self, request, timeout=None):
        url = request.full_url if hasattr(request, "full_url") else request
        if url.endswith("/releases/tags/v" + self.app.VERSION):
            return Reply(json.dumps({"tag_name": "v" + self.app.VERSION, "assets": [
                {"name": self.archive.name, "browser_download_url": "https://example.invalid/" + self.archive.name}
            ]}).encode())
        if url.endswith(self.archive.name):
            return Reply(self.archive.read_bytes())
        raise AssertionError("unexpected request " + url)

    def test_starts_and_finishes_the_update(self):
        app = self.app
        # The modules came from the copy inside ui/, the only place the old
        # updater put them.
        self.assertEqual(Path(app.mod_packages.__file__).parent, self.install / "launcher/ui/modules")
        self.assertFalse(app.modkit_ready())
        missing = app.missing_release_files()
        self.assertEqual(missing, ["simpsons-modkit.json", "modkit_streams", "launcher/simpsons-launcher",
                                   "launcher/simpsons-modkit", "launcher/mod_packages.py"])
        with mock.patch.object(app.urllib.request, "urlopen", side_effect=self.urlopen), \
                mock.patch.object(app, "game_running_pids", return_value=[]), \
                mock.patch.object(app, "cpu_runs_default_build", return_value=True):
            app.check_updates()
            self.assertTrue(app.update_state["update_available"])
            self.assertTrue(app.update_state["repair"])
            ok, message = app.apply_update()
        self.assertTrue(ok, message)
        self.assertEqual(app.missing_release_files(), [])
        self.assertTrue(app.modkit_ready())
        for name in ("launcher/steam_shortcut.py", "launcher/modkit_app.py", "Modkit.sh",
                     "launcher/simpsons-modkit.sh", "launcher/licenses/versions.txt"):
            self.assertTrue((self.install / name).is_file(), name)
        self.assertEqual((self.install / "Play.sh").read_text(), "new Play.sh\n")
        self.assertTrue((self.install / "launcher/simpsons-launcher").stat().st_mode & 0o111)
        # The player's files are as they were.
        self.assertEqual((self.install / "launcher/mods/my.mod/keep.txt").read_text(), "mine\n")
        self.assertEqual(json.loads((self.install / "launcher/launcher.json").read_text())["ui_theme"], "light")
        self.assertEqual((self.install / "simpsons.toml").read_text(), "frame_rate = 90\n")

    def test_game_mode_finishes_before_opening_a_window(self):
        app = self.app
        started = []
        with mock.patch.object(app.urllib.request, "urlopen", side_effect=self.urlopen), \
                mock.patch.object(app, "game_running_pids", return_value=[]), \
                mock.patch.object(app, "cpu_runs_default_build", return_value=True), \
                mock.patch.object(app, "gaming_mode", return_value=True), \
                mock.patch.object(app.shutil, "which", return_value=None), \
                mock.patch.object(app.os, "execv", side_effect=lambda path, argv: started.append((path, argv))):
            app.finish_update_before_window()
            self.assertEqual(started[0][0], str(self.install / "launcher/simpsons-launcher.sh"))
            self.assertEqual(app.missing_release_files(), [])
            # A complete installation starts normally.
            started.clear()
            self.assertFalse(app.finish_update_before_window())
            self.assertEqual(started, [])

    def test_desktop_and_offline_starts_open_the_launcher(self):
        app = self.app
        with mock.patch.object(app, "gaming_mode", return_value=False), \
                mock.patch.object(app.os, "execv", side_effect=AssertionError("restarted")):
            self.assertFalse(app.finish_update_before_window())
        with mock.patch.object(app, "gaming_mode", return_value=True), \
                mock.patch.object(app.shutil, "which", return_value=None), \
                mock.patch.object(app.urllib.request, "urlopen", side_effect=OSError("offline")), \
                mock.patch.object(app.os, "execv", side_effect=AssertionError("restarted")):
            self.assertFalse(app.finish_update_before_window())
        self.assertIn("simpsons-modkit.json", app.missing_release_files())

    def test_complete_package_and_checkout_ask_for_nothing(self):
        app = self.app
        with mock.patch.object(app, "ROOT", self.package):
            self.assertEqual(app.missing_release_files(), [])
        with mock.patch.object(app, "BUILD_VARIANT_FILE", self.install / "absent.txt"):
            self.assertEqual(app.missing_release_files(), [])


class BundledMods(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="simpsons-bundled-check-")
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name)
        self.bundled, self.mods = base / "bundled", base / "mods"
        shutil.copytree(ROOT / "launcher/ui/bundled-mods", self.bundled)
        self.packages = load(ROOT / "launcher/mod_packages.py", "bundled_mod_packages")
        self.names = sorted(p.name for p in self.bundled.iterdir() if (p / "mod.json").is_file())
        self.assertTrue(self.names)

    def seed(self):
        return self.packages.seed_bundled(self.bundled, self.mods)

    def test_added_once_and_left_disabled(self):
        self.assertEqual(self.seed(), [self.packages.manifest(self.bundled / n, verify=False)["id"] for n in self.names])
        installed = self.packages.installed(self.mods)
        self.assertEqual(len(installed), len(self.names))
        self.assertEqual(self.packages.profile(self.mods)["enabled"], [])
        self.assertEqual(self.seed(), [])

    def test_removed_copy_stays_removed(self):
        identifier = self.seed()[0]
        shutil.rmtree(self.mods / identifier)
        self.assertEqual(self.seed(), [])
        self.assertFalse((self.mods / identifier).exists())

    def test_edited_copy_is_kept_and_untouched_copy_follows_the_launcher(self):
        identifier = self.seed()[0]
        name = next(n for n in self.names if self.packages.manifest(self.bundled / n, verify=False)["id"] == identifier)
        with (self.bundled / name / "README.md").open("a") as stream:
            stream.write("\nA later launcher changed this.\n")
        self.assertEqual(self.seed(), [identifier])
        self.assertIn("A later launcher", (self.mods / identifier / "README.md").read_text())
        (self.mods / identifier / "README.md").write_text("The player's notes.\n")
        with (self.bundled / name / "README.md").open("a") as stream:
            stream.write("\nAnd again.\n")
        self.assertEqual(self.seed(), [])
        self.assertEqual((self.mods / identifier / "README.md").read_text(), "The player's notes.\n")

    def test_existing_identical_copy_is_adopted(self):
        name = self.names[0]
        identifier = self.packages.manifest(self.bundled / name, verify=False)["id"]
        shutil.copytree(self.bundled / name, self.mods / identifier)
        self.assertEqual(self.seed(), [])
        with (self.bundled / name / "README.md").open("a") as stream:
            stream.write("\nNewer.\n")
        self.assertEqual(self.seed(), [identifier])


if __name__ == "__main__":
    unittest.main(verbosity=2)
