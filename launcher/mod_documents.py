"""Portable creator documents. Progress and native handles stay in save state."""
from __future__ import annotations

import json
import math
import re

if __package__:
    from . import mod_scenes as scenes
else:
    import mod_scenes as scenes

KINDS = ("scene", "mission")
ID = re.compile(r"[a-z0-9_-]{1,80}\Z")


def _check(value, message):
    if not value:
        raise ValueError(message)


def _real(value, low, high):
    _check(type(value) in (int, float) and low <= value <= high and math.isfinite(value),
           "Mission number exceeds its bounds")
    return float(value)


def empty(kind):
    if kind == "scene":
        return scenes.empty()
    _check(kind == "mission", "Choose a scene or mission document")
    return {"schema": 1, "name": "My mission", "description": "", "level_root": "", "revision": 1,
            "time_limit": 0.0, "fail_on_defeat": True, "auto_start": False, "objectives": []}


def validate(kind, document):
    if kind == "scene":
        return scenes.validate(document)
    _check(kind == "mission", "Choose a scene or mission document")
    scenes._keys(document, ("schema", "name", "description", "level_root", "revision", "time_limit",
                            "fail_on_defeat", "auto_start", "objectives"))
    _check(type(document.get("schema")) is int and document["schema"] == 1, "Unsupported mission document version")
    _check(all(k in document for k in ("name", "level_root", "objectives")), "Incomplete mission document")
    root = scenes._text(document["level_root"], 200, True)
    if root:
        root = scenes._stream(root)
        parts = root.split("/")
        _check(len(parts) == 2 and parts[1] == parts[0] + ".str", "Invalid mission episode catalog")
    revision = document.get("revision", 1)
    _check(type(revision) is int and 1 <= revision <= 0xFFFFFFFF, "Invalid mission document revision")
    objects = document["objectives"]
    if objects == {}:
        objects = []
    _check(isinstance(objects, list) and len(objects) <= 32 and (not objects or bool(root)),
           "A mission has up to 32 objectives in one episode")
    for key in ("fail_on_defeat", "auto_start"):
        _check(type(document.get(key, key == "fail_on_defeat")) is bool, "Mission switches must be boolean")
    result = {"schema": 1, "name": scenes._text(document["name"], 160),
        "description": scenes._text(document.get("description", ""), 1024, True), "level_root": root,
        "revision": revision, "time_limit": _real(document.get("time_limit", 0), 0, 86400),
        "fail_on_defeat": document.get("fail_on_defeat", True), "auto_start": document.get("auto_start", False),
        "objectives": []}
    ids = set()
    for source in objects:
        scenes._keys(source, ("id", "title", "kind", "position", "radius", "seconds", "requires", "optional", "checkpoint"))
        _check(all(k in source for k in ("id", "title", "kind")), "Incomplete mission objective")
        ident = scenes._text(source["id"], 80)
        _check(ID.fullmatch(ident) and ident not in ids, "Mission objective ids must be unique and stable")
        ids.add(ident)
        kind = source["kind"]
        _check(kind in ("reach", "stay", "wait"), "Unsupported mission objective kind")
        _check(kind == "wait" or "position" in source, "A reach or stay objective needs its point")
        radius = scenes._number(source.get("radius", 3), 1000)
        _check(radius > 0, "A mission radius cannot be zero or underflow float32")
        deps = source.get("requires", [])
        if deps == {}:
            deps = []
        _check(isinstance(deps, list) and len(deps) <= 32
               and all(isinstance(v, str) and ID.fullmatch(v) and v != ident for v in deps)
               and len(set(deps)) == len(deps), "Invalid mission prerequisites")
        for key in ("optional", "checkpoint"):
            _check(type(source.get(key, False)) is bool, "Objective switches must be boolean")
        result["objectives"].append({"id": ident, "title": scenes._text(source["title"], 240), "kind": kind,
            "position": scenes._vector(source.get("position", {"x": 0, "y": 0, "z": 0})),
            "radius": radius, "seconds": _real(source.get("seconds", 2), .1, 86400),
            "requires": sorted(deps), "optional": source.get("optional", False), "checkpoint": source.get("checkpoint", False)})
    _check(not objects or any(not item["optional"] for item in result["objectives"]),
           "A mission needs at least one required objective")
    graph = {v["id"]: v["requires"] for v in result["objectives"]}
    visited = {}

    def visit(ident):
        _check(ident in graph, "Unknown mission prerequisite")
        _check(visited.get(ident) != 1, "Mission prerequisites contain a cycle")
        if visited.get(ident) == 2:
            return
        visited[ident] = 1
        for parent in graph[ident]:
            visit(parent)
        visited[ident] = 2

    for ident in graph:
        visit(ident)
    _check(len(json.dumps(result, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")) <= 64 * 1024,
           "Mission documents must fit 64 KiB")
    return result


def draft(value, kind):
    _check(kind in KINDS, "Choose a scene or mission draft")
    scenes._keys(value, ("schema", "fingerprint", kind))
    _check(type(value.get("schema")) is int and value["schema"] == 1, "Unsupported project draft version")
    _check(isinstance(value.get("fingerprint"), str) and re.fullmatch(r"[a-f0-9]{64}", value["fingerprint"]),
           "Invalid project draft source fingerprint")
    return {"schema": 1, "fingerprint": value["fingerprint"], kind: validate(kind, value.get(kind))}


def bundle(value):
    scenes._keys(value, ("schema", "fingerprint", "documents"))
    _check(type(value.get("schema")) is int and value["schema"] == 1, "Unsupported saved draft bundle")
    _check(isinstance(value.get("fingerprint"), str) and re.fullmatch(r"[a-f0-9]{64}", value["fingerprint"]),
           "Invalid project draft source fingerprint")
    items = value.get("documents")
    _check(isinstance(items, dict) and 0 < len(items) <= 2 and not items.keys() - set(KINDS),
           "A saved draft bundle needs scene or mission documents")
    return {"schema": 1, "fingerprint": value["fingerprint"],
            "documents": {kind: validate(kind, document) for kind, document in items.items()}}
