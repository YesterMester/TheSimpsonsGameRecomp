"""The public mission template must use the normal creator/package workflow."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/modkit"))
import modkit as creator
import packages


class MissionProjects(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.mods = self.root / "mods"

    def command(self, *arguments):
        with contextlib.redirect_stdout(io.StringIO()):
            creator.main(["--mods", str(self.mods), *arguments])

    def test_create_validate_pack_install_keeps_credits_and_stays_disabled(self):
        self.command("new", "Walking", "--id", "creator.walk",
                     "--author", "Example Creator", "--template", "mission-walk")
        project = self.mods / "Walking"
        self.command("validate", "Walking")
        archive = self.root / "walk.zip"
        self.command("pack", "Walking", "--out", str(archive))
        installed = self.root / "installed"
        result = packages.install(archive, installed)
        self.assertEqual(result["id"], "creator.walk")
        self.assertEqual(result["author"], "Example Creator")
        self.assertEqual(packages.profile(installed)["enabled"], [])
        self.assertEqual(packages.profile(self.mods)["enabled"], [])
        manifest = packages.manifest(project)
        self.assertEqual(manifest["kind"], "lua")
        self.assertEqual(manifest["save_mode"], "state")
        self.assertEqual(manifest["locales"], {"en": "locales/en.json"})
        self.assertIn("game.missions.register", (project / "main.lua").read_text())
        completion = json.loads((project / ".luarc.json").read_text())
        self.assertEqual(completion["runtime.version"], "Lua 5.4")

    def test_native_template_mismatch_does_not_create_a_partial_project(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.command("new", "Wrong", "--kind", "native", "--template", "mission-walk")
        self.assertFalse((self.mods / "Wrong").exists())

    def test_combat_workshop_packages_native_capabilities_and_creator_credit(self):
        self.command("new", "Combat", "--id", "creator.combat", "--author", "Encounter Creator",
                     "--template", "combat-workshop")
        self.command("validate", "Combat")
        project = self.mods / "Combat"
        manifest = packages.manifest(project)
        self.assertIn("entity.died", manifest["requires_events"])
        for operation in ["entities.health", "entities.damage", "world.spawn", "world.destroy", "entities.set_visible"]:
            self.assertIn(operation, manifest["requires_operations"])
        archive = self.root / "combat.zip"
        self.command("pack", "Combat", "--out", str(archive))
        result = packages.install(archive, self.root / "installed")
        self.assertEqual(result["author"], "Encounter Creator")
        self.assertEqual(packages.profile(self.root / "installed")["enabled"], [])
        self.assertIn('game.on("entity.died"', (Path(result["folder"]) / "main.lua").read_text())


if __name__ == "__main__":
    unittest.main()
