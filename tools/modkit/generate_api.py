#!/usr/bin/env python3
"""Generate the embedded catalog, Lua completion and API reference together."""
import argparse
import json
from pathlib import Path


def generate(source, output):
    catalog = json.loads(source.read_text(encoding="utf-8"))
    names = set()
    for op in catalog["operations"]:
        if op["name"] in names:
            raise ValueError("duplicate API operation: " + op["name"])
        names.add(op["name"])
    output.mkdir(parents=True, exist_ok=True)
    text = json.dumps(catalog, separators=(",", ":"), ensure_ascii=False)
    (output / "api_catalog.h").write_text(
        '#pragma once\ninline constexpr char kModkitApiCatalog[] = R"MODKIT(' + text + ')MODKIT";\n', encoding="utf-8")
    lua = ["---@meta", "-- Generated from modkit/api.json.", "game = {}", ""]
    for name, definition in catalog.get("types", {}).items():
        lua += ["---" + definition["description"], "---@class " + name]
        fields = set()
        for field in definition["fields"]:
            if field["name"] in fields:
                raise ValueError("duplicate field in " + name + ": " + field["name"])
            fields.add(field["name"])
            lua.append("---@field " + field["name"] + " " + field["type"]
                       + (" " + field["description"] if field.get("description") else ""))
        lua.append("")
    for group in sorted({op["name"].split(".")[0] for op in catalog["operations"]}):
        lua.append("game." + group + " = {}")
    reference = ["# Game API", "", "Generated from `modkit/api.json`. Calls run in game-thread callbacks.", ""]
    types = {"handle": "string", "object": "table", "vec3": "table", "array": "table", "any": "any"}
    for op in catalog["operations"]:
        params = op["parameters"]
        lua.extend(["", "---" + op["description"]])
        for p in params:
            lua.append("---@param " + p["name"] + ("?" if p.get("optional") else "") + " " + p.get("lua_type", types.get(p["type"], p["type"])))
        if op.get("lua_returns"):
            lua.append("---@return " + op["lua_returns"])
        lua.append("function game." + op["name"] + "(" + ", ".join(p["name"] for p in params) + ") end")
        reference.extend(["## `game." + op["name"] + "(" + ", ".join(p["name"] for p in params) + ")`", "", op["description"], "", "Returns: " + op["returns"] + ".", ""])
    lua += ["", "game.state = {}", "---@param event " + "|".join(repr(name) for name in catalog.get("events", {})), "---@param callback fun(data: table)", "function game.on(event, callback) end", ""]
    for name, definition in catalog.get("types", {}).items():
        reference += ["## `" + name + "`", "", definition["description"], "",
                      "| Field | Type | Meaning |", "| --- | --- | --- |"]
        for field in definition["fields"]:
            reference.append("| `" + field["name"] + "` | `" + field["type"].replace("|", "\\|") + "` | "
                             + field.get("description", "").replace("|", "\\|") + " |")
        reference.append("")
    reference += ["## Events", "", "Register handlers during module initialization with `game.on(name, callback)`.", ""]
    for name, description in catalog.get("events", {}).items():
        reference += ["- `" + name + "`: " + description]
    reference.append("")
    (output / "game.lua").write_text("\n".join(lua), encoding="utf-8")
    (output / "API.md").write_text("\n".join(reference), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2] / "modkit/api.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
