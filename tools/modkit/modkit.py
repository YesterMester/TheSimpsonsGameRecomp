#!/usr/bin/env python3
"""Create, build, package and run mods for The Simpsons Game: Recompiled.

Lua scripts, native C++ plugins and file replacements share the launcher package
format. Start with `new`, edit the example, then `validate`, `run` and `pack`.
`find` and `add` work with your own installed game files. Use --gamedata and
--mods to select paths; run a command with -h for its options.
"""

import argparse
import json
import os
import shutil
import sys
import zipfile
from pathlib import Path
import commands
import packages
import translations
import developer
import code_navigation
import models
import meshes
import textures


# ---------------------------------------------------------------- locating

def find_root(start=None):
    """Walk up from `start` looking for the install layout. Works both in
    the dev repo (gamedata/ and launcher/mods/ under the repo root) and in a
    packaged release (gamedata/ and mods/ sitting next to each other)."""
    d = Path(start or Path.cwd()).resolve()
    for candidate in [d, *d.parents]:
        if (candidate / "gamedata").is_dir():
            return candidate
    return None


def resolve_paths(args):
    root = find_root()
    gamedata = Path(args.gamedata).resolve() if args.gamedata else (
        (root / "gamedata") if root else None)
    if args.mods:
        mods = Path(args.mods).resolve()
    elif root and (root / "launcher" / "mods").is_dir():
        mods = root / "launcher" / "mods"       # dev repo layout
    elif root and (root / "launcher").is_dir():
        mods = root / "launcher" / "mods"       # dev repo, mods/ not made yet
    elif root:
        mods = root / "mods"                    # release layout
    else:
        mods = Path.cwd() / "mods"
    return gamedata, mods


def die(msg):
    print(f"modkit: {msg}", file=sys.stderr)
    sys.exit(1)


# ------------------------------------------------------------------- new

def cmd_new(args, gamedata, mods):
    return commands.new(args, gamedata, mods)


# ------------------------------------------------------------------ find

def cmd_find(args, gamedata, mods):
    if not gamedata or not gamedata.is_dir():
        die("can't find the installed game data - install the game first, "
            "or point me at it with --gamedata /path/to/gamedata")
    needle = args.text.lower()
    hits = []
    for f in gamedata.rglob("*"):
        if f.is_file() and needle in f.name.lower():
            hits.append(f)
    hits.sort()
    if not hits:
        print(f"Nothing in the game data matches '{args.text}'.")
        print("Try a broader term - part of a filename or extension "
              "(igc, .vp6, gameflow, localetable).")
        return
    print(f"{len(hits)} file(s) matching '{args.text}':\n")
    for f in hits[:200]:
        rel = f.relative_to(gamedata)
        size = f.stat().st_size
        print(f"  {_human_size(size):>8}  {rel.as_posix()}")
    if len(hits) > 200:
        print(f"\n  ...and {len(hits) - 200} more. Narrow the search.")
    print("\nThe path shown (after the size) is what you pass to `modkit add`.")


# ------------------------------------------------------------------- add

def cmd_add(args, gamedata, mods):
    mod_dir = commands.project(args.name, mods)
    if not gamedata or not gamedata.is_dir():
        die("can't find the installed game data - install the game first, "
            "or point me at it with --gamedata /path/to/gamedata")

    rel = Path(args.path.replace("\\", "/"))
    src = packages.inside(gamedata, rel.as_posix())
    if not src.is_file():
        # be forgiving: they may have pasted a search-result line or a
        # slightly-off path. Point them at find.
        die(f"'{rel.as_posix()}' isn't a file in the game data. "
            f"Run `modkit find {rel.name}` to get the exact path.")

    dst = packages.inside(mod_dir / "gamedata", rel.as_posix())
    if dst.exists() and not args.force:
        die(f"{dst} already exists in your mod - pass --force to overwrite your copy")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    editable = rel.suffix.lower() in (".lua", ".txt", ".xml", ".json", ".ini", ".cfg")
    print(f"Copied {rel.as_posix()} into '{args.name}'.")
    print(f"Edit your copy here:\n  {dst}")
    if editable:
        print("It's a text file - open it in any editor and change what you like.")
    else:
        print("It's a binary file - replace it with your own version "
              "(same format, same filename).")


# ------------------------------------------------------------------ list

def cmd_list(args, gamedata, mods):
    installed = packages.installed(mods)
    if not installed:
        print(f"No mods yet. They live in {mods}")
        print("Start one with: modkit new MyMod")
        return
    selected = packages.profile(mods)["enabled"]
    active = {m["id"] for m in packages.resolve(installed, selected, verify_contents=False)}
    for identifier, manifest in installed.items():
        status = "enabled" if identifier in active else "disabled"
        title = f"{manifest['name']}  v{manifest['version']}  ({manifest['kind']}, {status})"
        if manifest.get("author"):
            title += f"  by {manifest['author']}"
        print(title)
        print("  id:", identifier)
        folder = Path(manifest["folder"]) / "gamedata"
        if folder.is_dir():
            for name, _ in packages.files(folder):
                print("  overrides", name)
        if manifest.get("entry"):
            print("  script:", manifest["entry"])
        for target, binary in manifest.get("binaries", {}).items():
            print(f"  {target}: {binary}")
        print()


# ------------------------------------------------------------------ pack

def cmd_pack(args, gamedata, mods):
    return commands.pack(args, gamedata, mods)


# --------------------------------------------------------------- helpers

def _human_size(n):
    size = float(n)
    for unit in ("B", "K", "M", "G"):
        if size < 1024 or unit == "G":
            return f"{size:.0f}{unit}"
        size /= 1024
    return f"{size:.0f}G"


# ---------------------------------------------------------------- argparse

def build_parser():
    p = argparse.ArgumentParser(
        prog="modkit",
        description="Make mods for The Simpsons Game: Recompiled.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Mods use named C++ or Lua game operations, native menu pages, "
               "and optional gamedata/ replacements. See modkit/README.md.")
    p.add_argument("--gamedata", help="path to the installed gamedata/ folder "
                                       "(found automatically by default)")
    p.add_argument("--mods", help="path to your mods folder "
                                   "(found automatically by default)")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("new", help="start a new mod folder")
    s.add_argument("name")
    s.add_argument("--author", default="")
    s.add_argument("--description", default="")
    s.add_argument("--kind", choices=["lua", "native", "data"], default="lua")
    s.add_argument("--template", choices=["health-tools", "mission-editor", "mission-walk", "input-observer", "owned-objects", "library-objects", "scene-editor", "combat-workshop", "camera-workshop"], default="health-tools",
                   help="commented Lua project to start from")
    s.add_argument("--id", help="stable id, for example creator.mod-name")
    s.set_defaults(func=commands.new)

    s = sub.add_parser("find", help="search the installed game for files to edit")
    s.add_argument("text", help="part of a filename, e.g. 'gameflow' or '.vp6'")
    s.set_defaults(func=cmd_find)

    s = sub.add_parser("add", help="copy a real game file into your mod to edit")
    s.add_argument("name", help="which mod")
    s.add_argument("path", help="game file path, as shown by `modkit find`")
    s.add_argument("--force", action="store_true", help="overwrite your existing copy")
    s.set_defaults(func=cmd_add)

    s = sub.add_parser("list", help="show your mods and what they override")
    s.set_defaults(func=cmd_list)

    s = sub.add_parser("pack", help="zip a mod up for sharing / installing")
    s.add_argument("name")
    s.add_argument("--out", help="output zip path (default: <name>.zip next to the mod)")
    s.set_defaults(func=commands.pack)

    commands.register(p, sub)
    translations.register(sub)
    developer.register(sub)
    code_navigation.register(sub)
    models.register(sub)
    meshes.register(sub)
    textures.register(sub)

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    # Session attachment, code navigation and OBJ interchange need no installed
    # game or project folder. Keep those operations independent of mod profiles.
    offline = args.command in {"dev", "code"} or \
        args.command == "mesh" and args.mesh_command in {"export", "import"}
    gamedata, mods = (None, None) if offline else resolve_paths(args)
    if mods is not None:
        mods.mkdir(parents=True, exist_ok=True)
    try:
        args.func(args, gamedata, mods)
    except (packages.ModError, ValueError, OSError) as error:
        die(str(error))


if __name__ == "__main__":
    main()
