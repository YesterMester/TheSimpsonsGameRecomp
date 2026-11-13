#!/usr/bin/env python3
"""Bundle commented creator sources and offline help for the desktop editor."""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "launcher/ui/modkit-templates.json"


def bundle():
    api = json.loads((ROOT / "modkit/api.json").read_text(encoding="utf-8"))
    operations = {item["name"] for item in api["operations"]}
    templates = []
    for identifier, title, description in [
        ("script-starter", "Script starter", "Begin with a small translated native menu and one editable action."),
        ("player-actions", "Player actions", "Add a controlled-character action with independent P1/P2 cooldowns and an optional shortcut."),
        ("timed-events", "Timed events", "Schedule and cancel gameplay timers without depending on display FPS."),
        ("health-tools", "Gameplay tools", "A native Options page, health controls and owned camera requests."),
        ("mission-walk", "There and back", "A mission with objectives, a time limit, failure, retry and saved progress."),
        ("mission-editor", "Mission editor", "Place route objectives and props in game, set rules and retry points, then export a credited mission."),
        ("input-observer", "Input observer", "Read P1/P2 buttons and analog controls without changing gameplay."),
        ("owned-objects", "Owned objects", "Spawn, move, rotate, scale and safely remove only this mod's loaded props."),
        ("library-objects", "Library objects", "Inspect named original variants, load a cross-level source and spawn through the native game factory."),
        ("scene-editor", "Scene editor", "Place and edit native props in game, undo edits and save portable scenes for your project."),
        ("combat-workshop", "Combat workshop", "Choose loaded NPC targets in game, with native damage/death events, objectives and checkpoint retry."),
        ("camera-workshop", "Camera workshop", "Capture and preview native camera shots, change lenses and safely release the camera."),
    ]:
        source = ROOT / "modkit/examples" / identifier
        manifest = json.loads((source / "mod.json").read_text(encoding="utf-8"))
        files = {p.relative_to(source).as_posix(): p.read_text(encoding="utf-8")
                 for p in sorted(source.rglob("*")) if p.is_file() and p.name != "mod.json"}
        if identifier == "mission-editor":
            files["scene.lua"] = (ROOT / "modkit/examples/scene-editor/main.lua").read_text(encoding="utf-8")
        lua = "\n".join(content for path, content in files.items() if path.endswith(".lua"))
        required = set(manifest.get("requires_operations", []))
        required |= set(re.findall(r"\bgame\.([a-z_]+\.[a-z_]+)\b", lua)) & operations
        required |= set(re.findall(r'''\bgame\.system\.try_call\(\s*["']([a-z_]+\.[a-z_]+)["']''', lua))
        if required - operations:
            raise ValueError("Unknown required operations in " + identifier + ": " + ", ".join(sorted(required - operations)))
        manifest["requires_operations"] = sorted(required)
        manifest["requires_events"] = sorted(set(re.findall(r'\bgame\.on\("([a-z_.]+)"', lua)))
        templates.append({"id": identifier, "name": title, "description": description,
                          "manifest": manifest, "files": files})
    templates.append({"id": "file-replacements", "name": "File replacements",
                      "description": "Start an empty data mod. Add your edited files beneath gamedata/.",
                      "manifest": {"kind": "data", "save_mode": "stateless"}, "files": {}})
    for template in templates:
        template["files"].setdefault("README.md", (
            "# Your mod project\n\n"
            "This is editable source. Modkit creates it disabled.\n"
            "Fill in your name, description and version in mod.json.\n"
            "Use the source editor, then Validate and Test. Test uses separate progress.\n"
            "Lua changes can be reloaded through the creator tools; file replacements need a restart.\n"
            "Export creates an installable ZIP with your credits and checksums.\n\n"
            "Game operations run from registered callbacks. Native Options pages appear in\n"
            "Options > Extras > Mods. Read the comments in main.lua before changing it.\n"
            "File-replacement mods use gamedata/ paths matching your own installed files.\n"
            "Do not include default.xex or unmodified game archives in a shared package.\n\n"
            "Full guide: https://github.com/YesterMester/TheSimpsonsGameRecomp/blob/main/modkit/README.md\n"
        ))
    guides = {p.name: p.read_text(encoding="utf-8") for p in sorted((ROOT / "modkit").glob("*.md"))}
    return {"schema": 1, "api": 1, "operations": sorted(operations), "events": sorted(api["events"]), "templates": templates,
            "reference": api, "guides": guides,
            "completion": (ROOT / "modkit/generated/game.lua").read_text(encoding="utf-8")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = json.dumps(bundle(), indent=2, ensure_ascii=False) + "\n"
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != text:
            parser.exit(1, "Creator templates are stale. Run tools/modkit/generate_templates.py\n")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != text:
            args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
