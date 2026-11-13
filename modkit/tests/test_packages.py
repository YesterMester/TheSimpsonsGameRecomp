#!/usr/bin/env python3
"""Exercise real package imports, profiles and cross-language fingerprints."""
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher"))
import mod_packages as p


class Packages(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="simpsons-package-check-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def project(self, identifier="test.mod", **extra):
        folder = self.root / "source" / identifier
        manifest = {"id": identifier, "name": "A mod", "version": "1.0.0", "api": 1,
                    "kind": "lua", "entry": "main.lua", "save_mode": "state", "author": "Creator"}
        manifest.update(extra)
        p.write_json(folder / "mod.json", manifest)
        (folder / "main.lua").write_text("game.on('frame', function() end)\n")
        return folder

    def test_pack_install_credits_and_integrity(self):
        source = self.project()
        archive = p.pack(source, self.root / "mod.zip")
        installed = p.install(archive, self.root / "mods")
        self.assertEqual(installed["author"], "Creator")
        self.assertEqual(p.fingerprint(source), p.fingerprint(Path(installed["folder"])))
        self.assertFalse(p.profile(self.root / "mods")["enabled"])
        (Path(installed["folder"]) / "main.lua").write_text("changed")
        with self.assertRaises(p.ModError):
            p.manifest(Path(installed["folder"]))

    def test_reproducible_archive(self):
        source = self.project()
        a = p.pack(source, self.root / "a.zip")
        b = p.pack(source, self.root / "b.zip")
        self.assertEqual(a.read_bytes(), b.read_bytes())

    def test_model_variants_share_credit_disabled_import_and_priority(self):
        document = {"format": "MMdl", "version": 10, "resource_id": "1" * 32,
                    "source_sha256": "a" * 64,
                    "changes": [{"field": "property:0", "value": "reference:1"}]}
        projects = []
        for identifier in ("test.low", "test.high"):
            source = self.project(identifier, kind="data", model_edits="assets/models.json")
            p.write_json(source / "assets/models.json", {"version": 1, "models": [document]})
            projects.append(p.manifest(source))
        conflict = p.conflicts(projects)
        self.assertEqual(conflict, [{"path": "model:" + "1" * 32, "winner": "test.high", "overridden": ["test.low"]}])
        installed = p.install(p.pack(Path(projects[1]["folder"]), self.root / "variant.zip"), self.root / "mods")
        self.assertEqual(installed["author"], "Creator")
        self.assertEqual(p.model_variants(Path(installed["folder"]), installed), [document])
        self.assertFalse(p.profile(self.root / "mods")["enabled"])

    def test_invalid_model_catalog_never_packs(self):
        source = self.project(kind="data", model_edits="assets/models.json")
        for catalog in ({"version": True, "models": []},
                        {"version": 1, "models": [{"format": "MMdl", "version": 10,
                         "resource_id": "1" * 32, "source_sha256": "a" * 64,
                         "changes": [{"field": "condition:0", "value": 100}]}]}):
            p.write_json(source / "assets/models.json", catalog)
            with self.assertRaises(p.ModError):
                p.pack(source, self.root / "invalid.zip")
            self.assertFalse((self.root / "invalid.zip").exists())

    def test_mesh_package_credit_priority_and_bad_coordinates(self):
        document = {"format": "EARS_MESH", "version": 1, "resource_id": "2" * 32,
                    "source_sha256": "b" * 64, "changes": [{"geometry": "geometry:0", "positions": [[0, 0, 0]]}]}
        mods = []
        for identifier in ("mesh.low", "mesh.high"):
            source = self.project(identifier, kind="data", mesh_edits="assets/mesh.json")
            p.write_json(source / "assets/mesh.json", {"version": 1, "meshes": [document]})
            mods.append(p.manifest(source))
        self.assertEqual(p.conflicts(mods), [{"path": "mesh:" + "2" * 32, "winner": "mesh.high", "overridden": ["mesh.low"]}])
        installed = p.install(p.pack(Path(mods[1]["folder"]), self.root / "mesh.zip"), self.root / "mods")
        self.assertEqual(installed["author"], "Creator")
        self.assertFalse(p.profile(self.root / "mods")["enabled"])
        self.assertEqual(p.mesh_variants(Path(installed["folder"]), installed), [document])
        source = Path(mods[0]["folder"])
        for value in (True, "0", 10**400):
            document["changes"][0]["positions"][0][0] = value
            p.write_json(source / "assets/mesh.json", {"version": 1, "meshes": [document]})
            with self.assertRaises(p.ModError):
                p.pack(source, self.root / "bad-mesh.zip")
            self.assertFalse((self.root / "bad-mesh.zip").exists())

    def test_zip_traversal_and_duplicate_paths(self):
        for names in [["../escaped"], ["/absolute"], ["C:\\escape"], ["file", "FILE"]]:
            archive = self.root / "bad.zip"
            with zipfile.ZipFile(archive, "w") as z:
                for name in names:
                    z.writestr(name, "bad")
            with self.assertRaises(p.ModError):
                p.install(archive, self.root / "mods")
        self.assertFalse((self.root / "escaped").exists())
        self.assertFalse(list((self.root / "mods").glob(".import-*")))

    def test_authored_mesh_normals_match_native_package_validation(self):
        source = self.project(kind="data", mesh_edits="assets/mesh.json")
        document = {"format": "EARS_MESH", "version": 1, "resource_id": "3" * 32,
                    "source_sha256": "c" * 64,
                    "changes": [{"geometry": "geometry:0", "positions": [[0, 0, 0]]}]}
        cases = [([0, 0, 1], True), ([.9950001, 0, 0], True),
                 ([.99499, 0, 0], False), ([.7, .7, .14], True),
                 ([0, 0, 0], False), ([1.1, 0, 0], False),
                 ([True, 0, 0], False), ([10**400, 0, 0], False),
                 ([1, 0], False), (None, False)]
        for normal, accepted in cases:
            document["changes"][0]["normals"] = [normal]
            p.write_json(source / "assets/mesh.json", {"version": 1, "meshes": [document]})
            if accepted:
                self.assertEqual(p.mesh_variants(source, p.manifest(source)), [document])
            else:
                with self.assertRaises(p.ModError):
                    p.manifest(source)
            if os.environ.get("MODKIT_CHECK_EXE"):
                result = subprocess.run([os.environ["MODKIT_CHECK_EXE"], "--validate-package", str(source)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)
                if accepted:
                    self.assertEqual(result.stdout.strip(), p.fingerprint(source))

    def test_uv_only_packages_and_optional_attribute_counts(self):
        source = self.project(kind="data", mesh_edits="assets/mesh.json")
        document = {"format": "EARS_MESH", "version": 1, "resource_id": "4" * 32,
                    "source_sha256": "d" * 64}
        changes = [({"geometry": "geometry:0", "uvs": [[-2, 3.5]]}, True),
                   ({"geometry": "geometry:0", "normals": [[0, 0, 1]]}, True),
                   ({"geometry": "geometry:0", "positions": [[0, 0, 0]], "normals": [[0, 1, 0]], "uvs": [[.5, 2]]}, True),
                   ({"geometry": "geometry:0", "uvs": [[True, 0]]}, False),
                   ({"geometry": "geometry:0", "uvs": [[10**400, 0]]}, False),
                   ({"geometry": "geometry:0", "uvs": [[1]]}, False),
                   ({"geometry": "geometry:0", "positions": [], "uvs": [[0, 0]]}, False),
                   ({"geometry": "geometry:0", "uvs": [[0, 0]], "unknown": []}, False),
                   ({"geometry": "geometry:0"}, False)]
        for change, accepted in changes:
            document["changes"] = [change]
            p.write_json(source / "assets/mesh.json", {"version": 1, "meshes": [document]})
            if accepted:
                self.assertEqual(p.mesh_variants(source, p.manifest(source)), [document])
            else:
                with self.assertRaises(p.ModError):
                    p.manifest(source)
            if os.environ.get("MODKIT_CHECK_EXE"):
                result = subprocess.run([os.environ["MODKIT_CHECK_EXE"], "--validate-package", str(source)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)
                if accepted:
                    self.assertEqual(result.stdout.strip(), p.fingerprint(source))

    def test_texture_images_keep_credit_disabled_import_and_priority(self):
        document = {"format": "EARS_ITXD", "version": 1, "resource_id": "5" * 32,
                    "source_sha256": "e" * 64,
                    "changes": [{"texture": "texture:0", "levels": ["00010203"]}]}
        mods = []
        for identifier in ("texture.low", "texture.high"):
            folder = self.project(identifier, kind="data", texture_edits="assets/textures.json")
            p.write_json(folder / "assets/textures.json", {"version": 1, "textures": [document]})
            mods.append(p.manifest(folder))
        self.assertEqual(p.conflicts(mods), [{"path": "texture:" + "5" * 32, "winner": "texture.high", "overridden": ["texture.low"]}])
        installed = p.install(p.pack(Path(mods[1]["folder"]), self.root / "textures.zip"), self.root / "mods")
        self.assertEqual(installed["author"], "Creator")
        self.assertFalse(p.profile(self.root / "mods")["enabled"])
        self.assertEqual(p.texture_variants(Path(installed["folder"]), installed), [document])

    def test_texture_package_validation_matches_native(self):
        folder = self.project(kind="data", texture_edits="assets/textures.json")
        document = {"format": "EARS_ITXD", "version": 1, "resource_id": "6" * 32,
                    "source_sha256": "f" * 64}
        cases = [({"texture": "texture:0", "levels": ["000102ff"]}, True),
                 ({"texture": "texture:9999", "levels": ["0000", "ffff"]}, True),
                 ({"texture": "texture:01", "levels": ["00"]}, False),
                 ({"texture": "texture:10000", "levels": ["00"]}, False),
                 ({"texture": "texture:0", "levels": ["0"]}, False),
                 ({"texture": "texture:0", "levels": ["FF"]}, False),
                 ({"texture": "texture:0", "levels": [True]}, False),
                 ({"texture": "texture:0", "levels": []}, False),
                 ({"texture": "texture:0", "levels": ["00"], "offset": 0}, False)]
        for change, accepted in cases:
            document["changes"] = [change]
            p.write_json(folder / "assets/textures.json", {"version": 1, "textures": [document]})
            if accepted:
                self.assertEqual(p.texture_variants(folder, p.manifest(folder)), [document])
            else:
                with self.assertRaises(p.ModError): p.manifest(folder)
            if os.environ.get("MODKIT_CHECK_EXE"):
                result = subprocess.run([os.environ["MODKIT_CHECK_EXE"], "--validate-package", str(folder)], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)
                if accepted: self.assertEqual(result.stdout.strip(), p.fingerprint(folder))

    def test_symlink_archive_and_source(self):
        source = self.project()
        (source / "link").symlink_to(self.root)
        with self.assertRaises(p.ModError):
            p.pack(source, self.root / "bad.zip")
        with zipfile.ZipFile(self.root / "link.zip", "w") as z:
            item = zipfile.ZipInfo("link")
            item.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(item, "../escape")
        with self.assertRaises(p.ModError):
            p.install(self.root / "link.zip", self.root / "mods")

    def test_dependencies_and_profile_order(self):
        a = self.project("test.a", dependencies={"test.b": "^1.0.0"})
        b = self.project("test.b")
        mods = {x["id"]: x for x in (p.manifest(a), p.manifest(b))}
        self.assertEqual([x["id"] for x in p.resolve(mods, ["test.a"])], ["test.b", "test.a"])
        mods["test.b"]["dependencies"] = {"test.a": "*"}
        with self.assertRaises(p.ModError):
            p.resolve(mods, ["test.a"])

    def test_reject_compiled_game_and_case_collisions(self):
        source = self.project()
        (source / "gamedata").mkdir()
        (source / "gamedata/default.xex").write_bytes(b"game")
        with self.assertRaises(p.ModError):
            p.manifest(source)
        (source / "gamedata/default.xex").unlink()
        (source / "gamedata/Texture").write_text("one")
        (source / "gamedata/texture").write_text("two")
        with self.assertRaises(p.ModError):
            p.manifest(source)

    def test_disabled_native_project_does_not_block_other_mods(self):
        lua = self.project("test.lua")
        self.project("test.native", kind="native", binaries={})
        all_mods = p.installed(self.root / "source")
        self.assertEqual(len(p.resolve(all_mods, ["test.lua"])), 1)
        with self.assertRaises(p.ModError):
            p.resolve(all_mods, ["test.native"])

    def test_modded_playthrough_never_writes_vanilla_progress(self):
        original = self.root / "userdata"
        checkpoint = original / "0009000000000001/454108ED/00000001/SIMPSONS_SLOT1"
        checkpoint.parent.mkdir(parents=True)
        checkpoint.write_bytes(b"vanilla progress")
        (original / "saves").mkdir()
        (original / "saves/old-snapshot").write_bytes(b"not copied")
        a = p.manifest(self.project("test.a"))
        b = p.manifest(self.project("test.b"))
        session = p.prepare_playthrough(original, [a, b])
        copy = session / checkpoint.relative_to(original)
        self.assertEqual(copy.read_bytes(), checkpoint.read_bytes())
        copy.write_bytes(b"modded progress")
        self.assertEqual(checkpoint.read_bytes(), b"vanilla progress")
        self.assertEqual(p.prepare_playthrough(original, [a, b]), session)
        self.assertEqual(copy.read_bytes(), b"modded progress")
        self.assertFalse((session / "saves/old-snapshot").exists())
        self.assertNotEqual(p.prepare_playthrough(original, [b, a]), session)
        self.assertEqual(p.prepare_playthrough(original, []), original)

    def test_cpp_and_python_fingerprints_match(self):
        exe = os.environ.get("MODKIT_CHECK_EXE")
        if not exe:
            self.skipTest("Set MODKIT_CHECK_EXE for the cross-language fixture")
        source = self.project()
        (source / "café.txt").write_text("A Unicode path\n", encoding="utf-8")
        (source / ".modkit-build").mkdir()
        (source / ".modkit-build/ignored").write_text("build output")
        cpp = subprocess.check_output([exe, "--fingerprint", str(source)], text=True).strip()
        self.assertEqual(p.fingerprint(source), cpp)


if __name__ == "__main__":
    unittest.main()
