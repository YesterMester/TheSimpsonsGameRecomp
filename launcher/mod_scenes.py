"""Portable scene documents shared by creator packaging and the command line."""
from __future__ import annotations
import json
import math
import re
import struct

TAG = re.compile(r"[a-z0-9_.-]{1,64}\Z")
GUID = re.compile(r"[a-fA-F0-9]{32}\Z")
LOWER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def _check(value, message):
    if not value:
        raise ValueError(message)


def _keys(value, allowed):
    _check(isinstance(value, dict) and not value.keys() - set(allowed), "Unsupported scene record or field")


def _text(value, limit, empty=False):
    _check(isinstance(value, str) and (empty or bool(value)) and len(value.encode("utf-8")) <= limit
           and not any(ord(c) < 32 or ord(c) == 127 for c in value), "Invalid scene text")
    return value


def _stream(value):
    text = _text(value, 200)
    _check(not text.startswith("/") and not any(c in text for c in "\\:")
           and all(part not in ("", ".", "..") for part in text.split("/")), "Invalid scene stream path")
    text = text.translate(LOWER)
    _check(text.endswith(".str"), "Scene streams must be .str files")
    return text


def _number(value, limit):
    _check(type(value) in (int, float) and abs(value) <= limit and math.isfinite(value), "Invalid scene number")
    return struct.unpack("f", struct.pack("f", value))[0]


def _vector(value):
    _keys(value, ("x", "y", "z"))
    _check(value.keys() == {"x", "y", "z"}, "A scene vector needs x, y and z")
    return {k: _number(value[k], 1000000) for k in ("x", "y", "z")}


def _pose(value):
    _keys(value, ("position", "right", "up", "forward"))
    _check(value.keys() == {"position", "right", "up", "forward"}, "Incomplete scene pose")
    pose = {key: _vector(value[key]) for key in ("position", "right", "up", "forward")}
    axes = [[pose[key][k] for k in ("x", "y", "z")] for key in ("right", "up", "forward")]
    squared = [sum(c*c for c in axis) for axis in axes]
    _check(all(1.1754943508222875e-38 * 4 <= v <= 3.4028234663852886e38 / 4 for v in squared), "Invalid scene basis length")
    r, u, f = axes
    determinant = r[0]*(u[1]*f[2]-u[2]*f[1]) - r[1]*(u[0]*f[2]-u[2]*f[0]) + r[2]*(u[0]*f[1]-u[1]*f[0])
    _check(abs(determinant) > math.prod(math.sqrt(v) for v in squared) * 1e-8, "Singular scene pose")
    return pose


def empty():
    return {"schema": 1, "name": "My scene", "level_root": "", "autoload": False, "objects": []}


def validate(document):
    _keys(document, ("schema", "name", "level_root", "autoload", "objects"))
    _check(type(document.get("schema")) is int and document["schema"] == 1, "Unsupported scene version")
    _check(all(k in document for k in ("name", "level_root", "objects")), "Incomplete scene document")
    root = _text(document["level_root"], 200, True)
    if root:
        root = _stream(root)
        parts = root.split("/")
        _check(len(parts) == 2 and parts[1] == parts[0] + ".str", "Invalid scene episode catalog")
    objects = document["objects"]
    if objects == {}:
        objects = []  # Empty Lua table.
    _check(isinstance(objects, list) and len(objects) <= 64 and (not objects or bool(root)), "Scene supports up to 64 objects in one episode")
    _check(type(document.get("autoload", False)) is bool, "Scene autoload must be boolean")
    result = {"schema": 1, "name": _text(document["name"], 160), "level_root": root,
              "autoload": document.get("autoload", False), "objects": []}
    ids = set()
    for item in objects:
        _keys(item, ("id", "name", "source", "transform", "radius"))
        _check(all(k in item for k in ("id", "name", "source", "transform")), "Incomplete scene object")
        ident = _text(item["id"], 64)
        _check(TAG.fullmatch(ident) and ident not in ids, "Scene object tags must be unique")
        ids.add(ident)
        source = item["source"]
        _keys(source, ("guid", "class", "stream"))
        _check(source.keys() == {"guid", "class", "stream"}, "Incomplete scene source")
        guid = _text(source["guid"], 32)
        _check(GUID.fullmatch(guid) and int(guid, 16), "Invalid scene GUID")
        radius = _number(item.get("radius", 1), 1000)
        _check(radius > 0, "Invalid scene radius")
        result["objects"].append({"id": ident, "name": _text(item["name"], 160),
            "source": {"guid": guid.upper(), "class": _text(source["class"], 160), "stream": _stream(source["stream"])},
            "transform": _pose(item["transform"]), "radius": radius})
    _check(len(json.dumps(result, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")) <= 128 * 1024, "Scene document exceeds 128 KiB")
    return result


def draft(value):
    _keys(value, ("schema", "fingerprint", "scene"))
    _check(type(value.get("schema")) is int and value["schema"] == 1, "Unsupported scene draft")
    _check(isinstance(value.get("fingerprint"), str) and re.fullmatch(r"[0-9a-f]{64}", value["fingerprint"]), "Invalid scene draft source revision")
    return {"schema": 1, "fingerprint": value["fingerprint"], "scene": validate(value.get("scene"))}
