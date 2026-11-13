"""An authored mission and prop scene share one source-safe project transaction."""
from pathlib import Path
import copy
import json
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher"))
import mod_documents as documents
import mod_packages as packages
import mod_projects as projects


def mission():
    result = documents.empty("mission")
    result["level_root"] = "GAMEHUB/GAMEHUB.str"
    result["objectives"] = [{"id": "point-1", "title": "Reach Frink", "kind": "reach",
        "position": {"x": 1, "y": 2, "z": 3}, "checkpoint": True},
        {"id": "point-2", "title": "Wait for the delivery", "kind": "wait", "seconds": 2,
         "requires": ["point-1"]}]
    return documents.validate("mission", result)


class MissionProjects(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name); self.mods = self.root / "mods"; self.user = self.root / "user"
        self.project = projects.create(self.mods, projects.DEFAULT_BUNDLE,
            {"template": "mission-editor", "id": "creator.route", "name": "My route", "author": "Créateur"})
        self.folder = Path(self.project["folder"])
        self.fingerprint = packages.manifest(self.folder)["fingerprint"]
        self.storage = self.user / "mod-creator-tests/creator.route/mod-storage/creator.route"
        self.saved = mission()
        self.scene = documents.empty("scene"); self.scene["name"] = "Route scenery"

    def draft(self, kind, document, fingerprint=None):
        packages.write_json(self.storage / (kind + "-draft.json"),
            {"schema": 1, "fingerprint": fingerprint or self.fingerprint, kind: document})

    def apply(self):
        return projects.dispatch(self.mods, projects.DEFAULT_BUNDLE,
            {"action": "apply-drafts", "id": "creator.route"}, self.user)

    def test_both_drafts_apply_and_export_atomically_with_credit(self):
        self.assertEqual(self.project["documents"], ["scene", "mission"])
        self.assertEqual((self.folder / "scene.lua").read_text(), (ROOT / "modkit/examples/scene-editor/main.lua").read_text())
        self.draft("scene", self.scene); self.draft("mission", self.saved)
        result = self.apply()
        self.assertEqual(result["documents"], ["scene", "mission"])
        self.assertEqual(result["objectives"], 2)
        updated = packages.manifest(self.folder)
        self.assertEqual(updated["scene"], self.scene); self.assertEqual(updated["mission"], self.saved)
        self.assertEqual(updated["author"], "Créateur")
        exported = projects.dispatch(self.mods, projects.DEFAULT_BUNDLE, {"action": "export", "id": "creator.route"})
        imported = packages.install(Path(exported["path"]), self.root / "imported")
        self.assertEqual(imported["mission"], self.saved)
        self.assertEqual(imported["scene"], self.scene)
        self.assertEqual(imported["author"], "Créateur")
        self.assertEqual(packages.profile(self.root / "imported")["enabled"], [])
        self.assertEqual(self.apply()["documents"], [])

    def test_stale_mission_does_not_partially_apply_a_valid_scene(self):
        self.draft("scene", self.scene); self.draft("mission", self.saved, "a" * 64)
        before = (self.folder / "mod.json").read_bytes()
        with self.assertRaisesRegex(packages.ModError, "sources changed"): self.apply()
        self.assertEqual((self.folder / "mod.json").read_bytes(), before)

    def test_invalid_graph_does_not_partially_apply_other_documents(self):
        invalid = copy.deepcopy(self.saved); invalid["objectives"][0]["requires"] = ["point-2"]
        self.draft("scene", self.scene); self.draft("mission", invalid)
        before = (self.folder / "mod.json").read_bytes()
        with self.assertRaisesRegex(packages.ModError, "cycle"): self.apply()
        self.assertEqual((self.folder / "mod.json").read_bytes(), before)

    def test_one_available_document_can_apply_without_fabricating_the_other(self):
        self.draft("mission", self.saved)
        self.assertEqual(self.apply()["documents"], ["mission"])
        self.assertEqual(packages.manifest(self.folder)["scene"], documents.empty("scene"))

    def test_atomic_bundle_applies_without_individual_draft_files(self):
        packages.write_json(self.storage / "project-drafts.json", {"schema": 1, "fingerprint": self.fingerprint,
            "documents": {"scene": self.scene, "mission": self.saved}})
        self.assertEqual(self.apply()["documents"], ["scene", "mission"])
        self.assertEqual(packages.manifest(self.folder)["mission"], self.saved)
        self.assertEqual(self.apply()["documents"], [])

    def test_invalid_bundle_never_applies_its_other_document(self):
        bad = copy.deepcopy(self.saved); bad["objectives"][0]["requires"] = ["point-2"]
        packages.write_json(self.storage / "project-drafts.json", {"schema": 1, "fingerprint": self.fingerprint,
            "documents": {"scene": self.scene, "mission": bad}})
        before = (self.folder / "mod.json").read_bytes()
        with self.assertRaisesRegex(packages.ModError, "cycle"): self.apply()
        self.assertEqual((self.folder / "mod.json").read_bytes(), before)

    def test_later_single_draft_is_selected_without_losing_the_bundled_mission(self):
        import os
        path = self.storage / "project-drafts.json"
        packages.write_json(path, {"schema": 1, "fingerprint": self.fingerprint,
            "documents": {"scene": documents.empty("scene"), "mission": self.saved}})
        self.draft("scene", self.scene)
        os.utime(path, ns=(1000000000, 1000000000))
        self.assertEqual(self.apply()["documents"], ["scene", "mission"])
        self.assertEqual(packages.manifest(self.folder)["scene"], self.scene)

    def test_saved_metadata_is_validated_before_replacement(self):
        opened = projects.read(self.folder, "mod.json")
        value = json.loads(opened["text"]); value["mission"] = self.saved
        value["mission"]["objectives"][0]["requires"] = ["point-2"]
        with self.assertRaisesRegex(packages.ModError, "cycle"):
            projects.write(self.folder, "mod.json", json.dumps(value), opened["revision"])
        self.assertEqual(projects.read(self.folder, "mod.json")["revision"], opened["revision"])

    def test_document_policy_rejects_bad_identity_clocks_and_pointer_fields(self):
        for field, value in [("schema", True), ("revision", 4294967297), ("time_limit", float("inf"))]:
            invalid = copy.deepcopy(self.saved); invalid[field] = value
            with self.assertRaises(ValueError): documents.validate("mission", invalid)
        invalid = copy.deepcopy(self.saved); invalid["objectives"][0]["entity"] = "entity:123"
        with self.assertRaises(ValueError): documents.validate("mission", invalid)
        invalid = copy.deepcopy(self.saved); invalid["objectives"][0]["radius"] = 1e-100
        with self.assertRaises(ValueError): documents.validate("mission", invalid)
        for obj in invalid["objectives"]: obj["optional"] = True
        invalid["objectives"][0]["radius"] = 1
        with self.assertRaises(ValueError): documents.validate("mission", invalid)


if __name__ == "__main__": unittest.main()
