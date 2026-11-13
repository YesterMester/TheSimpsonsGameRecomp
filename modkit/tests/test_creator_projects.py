"""Creator workflows preserve normal progress, source edits and package credit."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher"))
import mod_packages as packages
import mod_projects as projects

REPO = Path(__file__).resolve().parents[2]
BUNDLE = REPO / "launcher/ui/modkit-templates.json"


class CreatorProjects(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name)
        self.mods = self.base / "mods"
        self.metadata = {"template": "mission-walk", "name": "My mission", "id": "creator.mission",
                         "author": "Example Creator", "description": "Our route"}
        self.project = projects.create(self.mods, BUNDLE, self.metadata)
        self.folder = Path(self.project["folder"])

    def test_templates_match_public_commented_sources(self):
        sys.path.insert(0, str(REPO / "tools/modkit"))
        import generate_templates
        self.assertEqual(projects.catalog(BUNDLE), generate_templates.bundle())
        for item in projects.catalog(BUNDLE)["templates"]:
            if item["id"] == "file-replacements":
                continue
            source = REPO / "modkit/examples" / item["id"] / "main.lua"
            self.assertEqual(item["files"]["main.lua"], source.read_text(encoding="utf-8"))

    def test_create_keeps_profile_disabled_and_exports_credits_without_private_editor_data(self):
        self.assertEqual(packages.profile(self.mods)["enabled"], [])
        self.assertEqual(json.loads((self.folder / ".luarc.json").read_text())["workspace.library"], [".modkit/api"])
        result = projects.dispatch(self.mods, BUNDLE, {"action": "export", "id": self.project["id"]})
        installed = packages.install(Path(result["path"]), self.base / "other-player")
        self.assertEqual(installed["author"], "Example Creator")
        self.assertEqual(installed["description"], "Our route")
        self.assertEqual(installed["id"], self.project["id"])
        self.assertFalse((Path(installed["folder"]) / ".modkit").exists())
        self.assertEqual(packages.profile(self.base / "other-player")["enabled"], [])

    def test_stale_source_save_preserves_newer_external_edit(self):
        source = projects.read(self.folder, "main.lua")
        path = self.folder / "main.lua"
        path.write_text(source["text"] + "\n-- Another editor's change.\n")
        with self.assertRaisesRegex(packages.ModError, "changed since"):
            projects.write(self.folder, "main.lua", "-- old edit", source["revision"])
        self.assertIn("Another editor", path.read_text())

    def test_source_edit_changes_fingerprint_and_round_trips_package(self):
        before = packages.manifest(self.folder)["fingerprint"]
        source = projects.read(self.folder, "main.lua")
        edited = projects.write(self.folder, "main.lua", source["text"] + "\n-- Modder's change.\n", source["revision"])
        self.assertNotEqual(source["revision"], edited["revision"])
        self.assertNotEqual(before, projects.validate(self.mods, self.project["id"])["fingerprint"])
        export = projects.dispatch(self.mods, BUNDLE, {"action": "export", "id": self.project["id"]})
        imported = packages.install(Path(export["path"]), self.base / "installed")
        self.assertEqual((Path(imported["folder"]) / "main.lua").read_text(), edited["text"])

    def test_bad_json_or_changed_identity_does_not_damage_manifest(self):
        source = projects.read(self.folder, "mod.json")
        for invalid in ["{", '{"id":"another.mod","name":"No"}', source["text"].replace('"1.0.0"', '"broken"')]:
            with self.assertRaises((ValueError, packages.ModError)):
                projects.write(self.folder, "mod.json", invalid, source["revision"])
            self.assertEqual(projects.read(self.folder, "mod.json"), source)

    def test_duplicate_identity_does_not_leave_a_partial_project(self):
        request = dict(self.metadata, name="Another title")
        with self.assertRaisesRegex(packages.ModError, "already exists"):
            projects.create(self.mods, BUNDLE, request, folder_name="another-folder")
        self.assertFalse((self.mods / "another-folder").exists())
        self.assertFalse(list(self.mods.glob(".create-*")))

    def test_private_test_session_never_selects_installed_profile_or_copies_vanilla_saves(self):
        packages.write_json(packages.profile_path(self.mods), {"version": 1, "enabled": []})
        user = self.base / "normal-progress"
        original = user / "0123456789ABCDEF" / "save"
        original.parent.mkdir(parents=True); original.write_bytes(b"player progress")
        session = projects.prepare_test(self.mods, self.project["id"], user)
        self.assertNotEqual(session["user_data"], user)
        self.assertEqual(original.read_bytes(), b"player progress")
        self.assertFalse((session["user_data"] / "0123456789ABCDEF").exists())
        self.assertEqual(packages.profile(self.mods)["enabled"], [])
        self.assertEqual(packages.read_json(session["profile"])["enabled"], [self.project["id"]])
        (session["user_data"] / "test-progress").write_text("kept")
        self.assertEqual(projects.prepare_test(self.mods, self.project["id"], user)["user_data"], session["user_data"])
        self.assertEqual((session["user_data"] / "test-progress").read_text(), "kept")

    def test_editor_rejects_links_traversal_private_metadata_and_nontext(self):
        original = self.base / "vanilla.txt"; original.write_text("untouched")
        (self.folder / "link.lua").symlink_to(original)
        for path in ["../vanilla.txt", "link.lua", ".modkit/project.json", "/tmp/outside.lua"]:
            with self.assertRaises(packages.ModError):
                projects.read(self.folder, path)
        self.assertEqual(original.read_text(), "untouched")

    def test_linked_project_and_test_directory_cannot_write_outside_their_roots(self):
        linked = self.mods / "linked"
        linked.symlink_to(self.folder, target_is_directory=True)
        with self.assertRaises(packages.ModError):
            projects.find(self.mods, self.project["id"])
        linked.unlink()
        user = self.base / "userdata"; user.mkdir()
        original = self.base / "protected"; original.mkdir()
        (user / "mod-creator-tests").symlink_to(original, target_is_directory=True)
        with self.assertRaises(packages.ModError):
            projects.prepare_test(self.mods, self.project["id"], user)
        self.assertEqual(list(original.iterdir()), [])

    def test_invalid_metadata_and_unresolved_dependencies_stop_validation(self):
        request = dict(self.metadata, id="creator.other", name="Another mod", author="name\nextra")
        with self.assertRaises(packages.ModError):
            projects.create(self.mods, BUNDLE, request)
        m = packages.read_json(self.folder / "mod.json")
        m["dependencies"] = {"missing.mod": "^1.0.0"}; packages.write_json(self.folder / "mod.json", m)
        with self.assertRaises(packages.ModError):
            projects.validate(self.mods, self.project["id"])
        with self.assertRaises(packages.ModError):
            projects.prepare_test(self.mods, self.project["id"], self.base / "progress")
        self.assertFalse((self.base / "progress").exists())

    def test_old_build_capabilities_stop_test_before_any_progress_is_created(self):
        with self.assertRaisesRegex(packages.ModError, "Update the game build"):
            projects.prepare_test(self.mods, self.project["id"], self.base / "progress", {"api": 1})
        self.assertFalse((self.base / "progress").exists())
        descriptor = packages.read_json(REPO / "modkit/api.json")
        self.assertTrue(projects.prepare_test(self.mods, self.project["id"], self.base / "progress", descriptor)["profile"].is_file())

    def test_unsupported_api_names_cannot_be_exported_as_a_valid_project(self):
        manifest = packages.read_json(self.folder / "mod.json")
        manifest["requires_operations"] = ["missions.not_implemented"]
        packages.write_json(self.folder / "mod.json", manifest)
        for action in ["validate", "export"]:
            with self.assertRaisesRegex(packages.ModError, "do not provide"):
                projects.dispatch(self.mods, BUNDLE, {"action": action, "id": self.project["id"]})
        self.assertFalse((self.mods / "exports").exists())

    def test_combat_template_requires_native_deaths_and_visibility_before_touching_progress(self):
        project = projects.create(self.mods, BUNDLE, dict(self.metadata,
            template="combat-workshop", id="creator.combat", name="Combat"))
        descriptor = packages.read_json(REPO / "modkit/api.json")
        descriptor["events"].pop("entity.died")
        with self.assertRaisesRegex(packages.ModError, "entity.died"):
            projects.prepare_test(self.mods, project["id"], self.base / "progress", descriptor)
        self.assertFalse((self.base / "progress").exists())
        descriptor = packages.read_json(REPO / "modkit/api.json")
        descriptor["operations"] = [op for op in descriptor["operations"]
                                    if op["name"] != "entities.set_visible"]
        with self.assertRaisesRegex(packages.ModError, "entities.set_visible"):
            projects.prepare_test(self.mods, project["id"], self.base / "progress", descriptor)
        self.assertFalse((self.base / "progress").exists())


if __name__ == "__main__":
    unittest.main()
