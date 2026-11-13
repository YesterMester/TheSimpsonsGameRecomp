"""Exercise artist interchange without game data or a running game."""
import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/modkit"))
import meshes
import modkit as creator
import packages


def source():
    geometry = {"id": "geometry:0", "editable": True, "vertex_count": 3,
                "positions": [[-1, -1, 0], [1, -1, 0], [0, 1, 0]], "triangles": [0, 1, 2]}
    return {"format": "EARS_MESH", "version": 1, "resource_id": "1" * 32,
            "source_sha256": "a" * 64, "geometries": [geometry], "changes": []}


class Meshes(unittest.TestCase):
    def uv_source(self, editable=True):
        value = source()
        value["geometries"][0].update(editable=editable, uvs_editable=True,
                                       uvs=[[-.5, .25], [1.5, 2], [0, -.25]])
        return value

    def normal_source(self):
        value = source()
        value["geometries"][0].update(normals_editable=True, normals=[[0, 0, 1]] * 3)
        return value

    def test_obj_round_trip_and_edit(self):
        original = source()
        text = meshes.export_obj(original)
        self.assertEqual(meshes.import_obj(original, text)["changes"][0]["positions"], original["geometries"][0]["positions"])
        text = text.replace("v -1 -1 0", "v -0.5 -0.5 0")
        self.assertEqual(meshes.import_obj(original, text)["changes"][0]["positions"][0], [-.5, -.5, 0])
        self.assertEqual(original, source())

    def test_face_order_and_relative_indices(self):
        text = meshes.export_obj(source()).replace("f 1 2 3", "f -2 -1 -3")
        self.assertEqual(meshes.import_obj(source(), text)["changes"][0]["geometry"], "geometry:0")

    def test_reexport_keeps_current_edits(self):
        original = source()
        original["changes"] = [{"geometry": "geometry:0", "positions": [[-.5, -.5, 0], [.5, -.5, 0], [0, .5, 0]]}]
        result = meshes.import_obj(original, meshes.export_obj(original))
        self.assertEqual(result["changes"], original["changes"])

    def test_authored_normals_round_trip_and_edit(self):
        original = self.normal_source()
        text = meshes.export_obj(original, normals=True)
        result = meshes.import_obj(original, text, normals=True)
        self.assertEqual(result["changes"][0]["normals"], [[0, 0, 1]] * 3)
        text = text.replace("vn 0 0 1", "vn 1 0 0", 1)
        result = meshes.import_obj(original, text, normals=True)
        self.assertEqual(result["changes"][0]["normals"], [[1, 0, 0], [0, 0, 1], [0, 0, 1]])
        self.assertEqual(meshes.import_obj(result, meshes.export_obj(result, normals=True), normals=True)["changes"], result["changes"])

    def test_normal_import_preserves_original_hard_edge_splits(self):
        original = self.normal_source()
        original["geometries"][0]["triangles"] *= 2
        text = meshes.export_obj(original, normals=True)
        text = text.rsplit("f ", 1)[0] + "vn 1 0 0\nf 1//4 2//2 3//3\n"
        with self.assertRaisesRegex(packages.ModError, "different face normals"):
            meshes.import_obj(original, text, normals=True)

    def test_normal_import_rejects_absent_stream_and_invalid_references(self):
        with self.assertRaises(packages.ModError):
            meshes.export_obj(source(), normals=True)
        original = self.normal_source()
        text = meshes.export_obj(original, normals=True)
        for bad in (text.replace("1//1", "1//0"), text.replace("1//1", "1//4"),
                    text.replace("1//1", "1"), text.replace("vn 0 0 1", "vn 0 0 0"),
                    text.replace("vn 0 0 1", "vn nan 0 0")):
            with self.assertRaises(packages.ModError):
                meshes.import_obj(original, bad, normals=True)

    def test_unreferenced_normals_and_position_only_reimports_are_preserved(self):
        original = self.normal_source()
        g = original["geometries"][0]
        g["positions"].append([.1, .1, 0]);g["vertex_count"] = 4
        g["normals"] = [[0, 0, 1]] * 4
        imported = meshes.import_obj(original, meshes.export_obj(original, normals=True), normals=True)
        self.assertEqual(imported["changes"][0]["normals"], g["normals"])
        imported["changes"][0]["normals"][0] = [1, 0, 0]
        edited = meshes.import_obj(imported, meshes.export_obj(imported))
        self.assertEqual(edited["changes"][0]["normals"], imported["changes"][0]["normals"])

    def test_uv_round_trip_repeats_and_combined_face_attributes(self):
        original = self.uv_source()
        original["geometries"][0].update(normals_editable=True, normals=[[0, 0, 1]] * 3)
        text = meshes.export_obj(original, normals=True, uvs=True)
        self.assertIn("f 1/1/1 2/2/2 3/3/3", text)
        imported = meshes.import_obj(original, text, normals=True, uvs=True)
        self.assertEqual(imported["changes"][0]["uvs"], original["geometries"][0]["uvs"])
        text = text.replace("vt -0.5 0.25", "vt -1.25 2.5")
        self.assertEqual(meshes.import_obj(original, text, normals=True, uvs=True)["changes"][0]["uvs"][0], [-1.25, 2.5])

    def test_uv_only_geometry_keeps_positions_read_only(self):
        original = self.uv_source(editable=False)
        text = meshes.export_obj(original, uvs=True)
        imported = meshes.import_obj(original, text, uvs=True)
        self.assertEqual(set(imported["changes"][0]), {"geometry", "uvs"})
        with self.assertRaisesRegex(packages.ModError, "Only UVs"):
            meshes.import_obj(original, text.replace("v -1 -1 0", "v -.5 -1 0"), uvs=True)

    def test_uv_import_preserves_seams_and_validates_references(self):
        original = self.uv_source()
        text = meshes.export_obj(original, uvs=True)
        for bad in (text.replace("1/1", "1/0"), text.replace("1/1", "1/4"),
                    text.replace("1/1", "1"), text.replace("vt -0.5 0.25", "vt nan 0"),
                    text.replace("vt -0.5 0.25", "vt 1e400 0")):
            with self.assertRaises(packages.ModError):
                meshes.import_obj(original, bad, uvs=True)
        original["geometries"][0]["triangles"] *= 2
        text = meshes.export_obj(original, uvs=True).rsplit("f ", 1)[0]
        with self.assertRaisesRegex(packages.ModError, "different face uvs"):
            meshes.import_obj(original, text + "vt -.4 .25\nf 1/4 2/2 3/3\n", uvs=True)

    def test_uv_relative_indices_and_unreferenced_vertices(self):
        original = self.uv_source()
        g = original["geometries"][0]
        g["positions"].append([.1, .1, 0]);g["vertex_count"] = 4
        g["uvs"].append([1e-30, -0.0])
        text = meshes.export_obj(original, uvs=True).replace("f 1/1 2/2 3/3", "f -4/-4 -3/-3 -2/-2")
        imported = meshes.import_obj(original, text, uvs=True)
        self.assertEqual(imported["changes"][0]["uvs"], g["uvs"])
        imported["changes"][0]["uvs"][0] = [.125, .75]
        edited = meshes.import_obj(imported, meshes.export_obj(imported))
        self.assertEqual(edited["changes"][0]["uvs"], imported["changes"][0]["uvs"])

    def test_uv_stream_selection_and_float_limits(self):
        with self.assertRaises(packages.ModError):
            meshes.export_obj(source(), uvs=True)
        for point in ([True, 0], [10**1000, 0], [0], [0, float("inf")]):
            with self.assertRaises(packages.ModError):
                packages.validate_mesh_uv(point)

    def test_topology_names_counts_and_nonfinite_reject(self):
        text = meshes.export_obj(source())
        for bad in (text.replace("f 1 2 3", "f 3 2 1"), text.replace("geometry_0", "foreign"),
                    text.replace("v -1 -1 0", "v nan 0 0"), text.replace("v -1 -1 0", "v 1e400 0 0"),
                    text + "v 0 0 0\n", text.replace("f 1 2 3", "f 1 2 4"), text.replace("f 1 2 3", "f 0 1 2")):
            with self.assertRaises((packages.ModError, ValueError)):
                meshes.import_obj(source(), bad)

    def test_read_only_geometry_cannot_export(self):
        original = source(); original["geometries"][0]["editable"] = False
        with self.assertRaises(packages.ModError):
            meshes.export_obj(original)

    def test_source_coordinates_reject_before_float_overflow(self):
        for value in (True, float("nan"), float("inf"), 10**1000):
            original = source()
            original["geometries"][0]["positions"][0][0] = value
            with self.assertRaises(packages.ModError):
                meshes.export_obj(original)

    def test_offline_obj_commands_preserve_profiles_and_existing_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            document, obj, imported, mods = (root / name for name in ("mesh.json", "mesh.obj", "imported.json", "mods"))
            document.write_text(json.dumps(source()), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                creator.main(["--mods", str(mods), "mesh", "export", str(document), str(obj)])
                creator.main(["--mods", str(mods), "mesh", "import", str(document), str(obj), str(imported)])
            self.assertFalse(mods.exists())
            self.assertEqual(json.loads(imported.read_text())["changes"][0]["positions"], source()["geometries"][0]["positions"])
            before = obj.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                creator.main(["mesh", "export", str(document), str(obj)])
            self.assertEqual(obj.read_bytes(), before)

    def test_subset_import_preserves_other_edits(self):
        original = source(); other = copy.deepcopy(original["geometries"][0]); other["id"] = "geometry:1"
        original["geometries"].append(other)
        old = {"geometry": "geometry:0", "positions": [[0, 0, 0]] * 3}
        original["changes"] = [old]
        imported = meshes.import_obj(original, meshes.export_obj(original, ["geometry:1"]))
        self.assertEqual(imported["changes"][0], old)
        self.assertEqual(imported["changes"][1]["geometry"], "geometry:1")


if __name__ == "__main__":
    unittest.main()
