"""Native mesh projects with source-checked OBJ attribute editing.

Keep vertex order and triangulated faces when editing. Positions and optional
reviewed normals can change on static geometry; reviewed UVs can also change on
skinned geometry. Materials, weights and unknown data stay intact. The native
writer checks declarations, normal directions and original bounds.
"""
from collections import Counter
import json
import math
import struct
from pathlib import Path
import tempfile

import commands
import packages
from translations import stream_tool, tool_run


def document(path):
    value = packages.read_json(Path(path), 16 * 1024**2)
    if not isinstance(value, dict) or value.get("format") != "EARS_MESH" or \
            type(value.get("version")) is not int or value["version"] != 1:
        raise packages.ModError("Use a document from mesh inspect")
    return value


def fresh(path):
    path = Path(path).resolve()
    if path.exists():
        raise packages.ModError("Choose a new mesh output file")
    return path


def inspect(args, gamedata, mods):
    if not gamedata or not gamedata.is_dir():
        raise packages.ModError("Choose your installed game files with --gamedata")
    source = packages.inside(gamedata, args.stream)
    output = fresh(args.output)
    tool_run(stream_tool(args.stream_tool), "mesh-export", source, args.entry, args.resource, output)
    value = document(output)
    value["source"]["stream"] = args.stream
    packages.write_json(output, value)
    print("Exported", value["name"], "to", output)
    for field, label in (("editable", "positions"), ("normals_editable", "normals"), ("uvs_editable", "UVs")):
        editable = [g["id"] for g in value["geometries"] if g.get(field)]
        print("Editable " + label + ":", ", ".join(editable) or "none; see the inspection reasons")


def geometry_eligible(geometry, normals, uvs):
    return (geometry["editable"] or uvs and geometry.get("uvs_editable")) and \
        (not normals or geometry.get("normals_editable")) and \
        (not uvs or geometry.get("uvs_editable"))


def export_obj(value, selected=None, normals=False, uvs=False):
    geometries = [g for g in value["geometries"] if geometry_eligible(g, normals, uvs)
                  and (not selected or g["id"] in selected)]
    if not geometries or selected and set(selected) != {g["id"] for g in geometries}:
        raise packages.ModError("Choose geometries with the requested editable attributes")
    lines = ["# Simpsons Modkit vertex project", "# Keep vertex order and triangulated faces unchanged.",
             "# Resource " + value["resource_id"], "# Source " + value["source_sha256"]]
    first = 1
    edits = {change["geometry"]: change for change in value.get("changes", [])}
    for g in geometries:
        edit = edits.get(g["id"], {})
        positions = edit.get("positions", g["positions"])
        if len(positions) != g["vertex_count"] or len(g["triangles"]) % 3:
            raise packages.ModError("The inspection document has incomplete geometry")
        lines.append("o " + g["id"].replace(":", "_"))
        for position in positions:
            if len(position) != 3 or any(type(v) not in (int, float) or abs(v) > 3.4028234663852886e38 or not math.isfinite(v) for v in position):
                raise packages.ModError("Mesh coordinates must be finite numbers")
            lines.append("v " + " ".join(format(v, ".9g") for v in position))
        for field, enabled, kind, validate in (("normals", normals, "vn", normal_value),
                                                ("uvs", uvs, "vt", packages.validate_mesh_uv)):
            if not enabled:
                continue
            values = edit.get(field, g.get(field, []))
            if not g.get(field + "_editable") or len(values) != g["vertex_count"]:
                raise packages.ModError("Choose geometries with reviewed " + field)
            for point in values:
                validate(point)
                lines.append(kind + " " + " ".join(format(v, ".9g") for v in point))
        for i in range(0, len(g["triangles"]), 3):
            face = g["triangles"][i:i + 3]
            if any(type(v) is not int or not 0 <= v < g["vertex_count"] for v in face):
                raise packages.ModError("Mesh triangle exceeds its vertex allocation")
            tokens = []
            for v in face:
                token = str(v + first)
                if uvs or normals:
                    token += "/" + (str(v + first) if uvs else "")
                if normals:
                    token += "/" + str(v + first)
                tokens.append(token)
            lines.append("f " + " ".join(tokens))
        first += g["vertex_count"]
    return "\n".join(lines) + "\n"


def export(args, gamedata, mods):
    output = fresh(args.output)
    text = export_obj(document(args.document), args.geometry, args.normals, args.uvs)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        stream.write(text)
    print("Exported", output)
    print("Edit the selected attributes; preserve vertex order, names and triangle winding.")


def normal_value(normal):
    return packages.validate_mesh_normal(normal)


def obj_index(text, count, label):
    index = int(text)
    if not index:
        raise packages.ModError("OBJ " + label + " indices start at 1")
    index = index - 1 if index > 0 else count + index
    if not 0 <= index < count:
        raise packages.ModError("OBJ face exceeds its " + label + " list")
    return index


def import_obj(value, text, normals=False, uvs=False):
    expected = {g["id"].replace(":", "_"): g for g in value["geometries"]
                if geometry_eligible(g, normals, uvs)}
    vertices, faces = [], {}
    attributes = {"normals": {"enabled": normals, "kind": "vn", "slot": 2, "dimensions": 3,
                              "validate": normal_value, "values": [], "assignments": {}},
                  "uvs": {"enabled": uvs, "kind": "vt", "slot": 1, "dimensions": 2,
                          "validate": packages.validate_mesh_uv, "values": [], "assignments": {}}}
    records = {spec["kind"]: spec for spec in attributes.values() if spec["enabled"]}
    current = None
    for line in text.splitlines():
        fields = line.split("#", 1)[0].split()
        if not fields:
            continue
        kind, arguments = fields[0], fields[1:]
        if kind == "v":
            if len(arguments) != 3 or len(vertices) >= 1000000:
                raise packages.ModError("OBJ vertices need exactly three finite coordinates")
            position = [float(n) for n in arguments]
            if any(not math.isfinite(n) or abs(n) > 3.4028234663852886e38 for n in position):
                raise packages.ModError("OBJ coordinates exceed finite float range")
            vertices.append(position)
        elif kind in records:
            spec = records[kind]
            if len(arguments) != spec["dimensions"] or len(spec["values"]) >= 1000000:
                raise packages.ModError("OBJ " + kind + " has the wrong component count or exceeds its budget")
            spec["values"].append(spec["validate"]([float(n) for n in arguments]))
        elif kind in {"o", "g"}:
            if len(arguments) != 1:
                raise packages.ModError("Keep the exported geometry names")
            if arguments[0] in expected:
                current = arguments[0]
                faces.setdefault(current, [])
            elif kind == "o" or current is None:
                raise packages.ModError("Unknown OBJ geometry: " + arguments[0])
        elif kind == "f":
            if current is None and len(expected) == 1:
                current = next(iter(expected))
            if current is None or len(arguments) != 3:
                raise packages.ModError("Keep geometry names and export triangulated faces")
            face = []
            for token in arguments:
                components = token.split("/")
                index = obj_index(components[0], len(vertices), "vertex")
                face.append(index)
                for name, spec in attributes.items():
                    if not spec["enabled"]:
                        continue
                    if len(components) > 3 or len(components) <= spec["slot"] or not components[spec["slot"]]:
                        raise packages.ModError("Every face corner needs an OBJ " + name + " reference")
                    point = spec["values"][obj_index(components[spec["slot"]], len(spec["values"]), name)]
                    previous = spec["assignments"].get(index)
                    if previous is not None:
                        if name == "uvs":
                            different = struct.unpack("=2f", struct.pack("=2f", *previous)) != struct.unpack("=2f", struct.pack("=2f", *point))
                        else:
                            different = any(abs(a - b) > .000001 for a, b in zip(previous, point))
                        if different:
                            raise packages.ModError("One preserved vertex cannot use different face " + name + "; keep the original seam and hard-edge splits")
                    spec["assignments"][index] = point
            faces.setdefault(current, []).append(face)
        elif kind not in {"vt", "vn", "s", "usemtl", "mtllib"}:
            raise packages.ModError("Unsupported OBJ record: " + kind)
    selected = [g for name, g in expected.items() if name in faces]
    if not selected or sum(g["vertex_count"] for g in selected) != len(vertices):
        raise packages.ModError("Preserve the original vertex counts and order")
    canonical = lambda face: min(tuple(face), tuple(face[1:] + face[:1]), tuple(face[2:] + face[:2]))
    changes, first = [], 0
    for g in selected:
        original = Counter(canonical([v + first for v in g["triangles"][i:i + 3]]) for i in range(0, len(g["triangles"]), 3))
        actual = Counter(canonical(face) for face in faces[g["id"].replace(":", "_")])
        if original != actual:
            raise packages.ModError("Keep vertex order, triangle winding and topology unchanged")
        previous = next((c for c in value.get("changes", []) if c["geometry"] == g["id"]), {})
        change = dict(previous, geometry=g["id"])
        points = vertices[first:first + g["vertex_count"]]
        if g["editable"]:
            unchanged = len(points) == len(g["positions"]) and all(
                struct.pack("=3f", *p) == struct.pack("=3f", *q)
                for p, q in zip(points, g["positions"]))
            if (normals or uvs) and unchanged:
                change.pop("positions", None)
            else:
                change["positions"] = points
        else:
            for proposed in (points, previous.get("positions", g["positions"])):
                if len(proposed) != g["vertex_count"] or any(struct.pack("=3f", *p) != struct.pack("=3f", *q) for p, q in zip(proposed, g["positions"])):
                    raise packages.ModError("Only UVs are editable for " + g["id"] + "; keep its vertex positions unchanged")
            change.pop("positions", None)
        for name, spec in attributes.items():
            if not spec["enabled"]:
                continue
            baseline = previous.get(name, g.get(name, []))
            if not g.get(name + "_editable") or len(baseline) != g["vertex_count"]:
                raise packages.ModError("Choose geometries with reviewed " + name)
            # Unreferenced vertices keep their prior attributes. Changes to other
            # streams are retained when importing only normals or UVs.
            change[name] = [spec["assignments"].get(first + i, baseline[i]) for i in range(g["vertex_count"])]
        changes.append(change)
        first += g["vertex_count"]
    result = dict(value)
    old = {change["geometry"]: change for change in value.get("changes", [])}
    old.update({change["geometry"]: change for change in changes})
    result["changes"] = list(old.values())
    return result


def import_(args, gamedata, mods):
    source, output = Path(args.obj), fresh(args.output)
    if source.stat().st_size > 64 * 1024**2:
        raise packages.ModError("OBJ projects are limited to 64 MiB")
    result = import_obj(document(args.document), source.read_text(encoding="utf-8"), args.normals, args.uvs)
    packages.write_json(output, result)
    print("Imported mesh attributes into", output)
    print("Run mesh build to validate the source and native attribute layout.")


def build(args, gamedata, mods):
    if not gamedata or not gamedata.is_dir():
        raise packages.ModError("Choose your installed game files with --gamedata")
    folder = commands.project(args.name, mods)
    manifest = packages.manifest(folder, verify=False)
    path = Path(args.document).resolve()
    value = document(path)
    source = value.get("source", {})
    if not isinstance(source, dict) or type(source.get("entry")) is not int or source["entry"] < 0:
        raise packages.ModError("Use a document from mesh inspect")
    stream = packages.inside(gamedata, source.get("stream"))
    with tempfile.TemporaryDirectory(prefix="simpsons-mesh-") as temporary:
        output = Path(temporary) / "mesh.json"
        tool_run(stream_tool(args.stream_tool), "mesh-compile", stream, source["entry"], path, output)
        compiled = packages.read_json(output, 16 * 1024**2)["meshes"][0]
    relative = manifest.get("mesh_edits", "assets/mesh-edits.json")
    destination = packages.inside(folder, relative)
    if "mesh_edits" not in manifest and destination.exists():
        raise packages.ModError("The default mesh output exists; choose its catalog path in mod.json")
    existing = packages.mesh_variants(folder, manifest)
    catalog = {"version": 1, "meshes": [m for m in existing if m["resource_id"] != compiled["resource_id"]] + [compiled]}
    if len(catalog["meshes"]) > 64 or len(json.dumps(catalog, allow_nan=False).encode("utf-8")) > 16 * 1024**2:
        raise packages.ModError("Mesh catalog exceeds its budget")
    original = packages.read_json(folder / "mod.json")
    original["mesh_edits"] = relative
    packages.write_json(destination, catalog)
    packages.write_json(folder / "mod.json", original)
    (folder / "checksums.json").unlink(missing_ok=True)
    print("Built mesh edits in", destination)
    print("Restart the test session to load edited meshes.")


def register(sub):
    p = sub.add_parser("mesh", help="inspect and edit supported native mesh attributes")
    actions = p.add_subparsers(dest="mesh_command", required=True)
    inspect_ = actions.add_parser("inspect", help="export a readable mesh document")
    inspect_.add_argument("stream"); inspect_.add_argument("entry", type=int)
    inspect_.add_argument("resource"); inspect_.add_argument("output")
    inspect_.add_argument("--stream-tool"); inspect_.set_defaults(func=inspect)
    export_ = actions.add_parser("export", help="export editable geometries as OBJ")
    export_.add_argument("document"); export_.add_argument("output")
    export_.add_argument("--geometry", action="append")
    export_.add_argument("--normals", action="store_true", help="include the reviewed static normal stream")
    export_.add_argument("--uvs", action="store_true", help="include the reviewed texture-coordinate stream")
    export_.set_defaults(func=export)
    import__ = actions.add_parser("import", help="import edited OBJ mesh attributes")
    import__.add_argument("document"); import__.add_argument("obj"); import__.add_argument("output")
    import__.add_argument("--normals", action="store_true", help="import face normals without changing vertex splits")
    import__.add_argument("--uvs", action="store_true", help="import texture coordinates without changing vertex seams")
    import__.set_defaults(func=import_)
    build_ = actions.add_parser("build", help="verify and build a mesh project")
    build_.add_argument("name"); build_.add_argument("document")
    build_.add_argument("--stream-tool"); build_.set_defaults(func=build)
