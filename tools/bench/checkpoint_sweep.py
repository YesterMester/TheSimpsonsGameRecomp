#!/usr/bin/env python3
"""Catalog map entry checkpoints and prepare isolated renderer test starts.

Reads the player's own gameflow script. Each prepared game uses the original
map entry, including its character overrides and extension flag. Save files
and game data at the source are never changed. The catalog describes map entry
points, not every checkpoint inside a level.
"""

import argparse
import ast
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import uuid

# The shipped editor and renderer sweep use the same gameflow parser.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher"))
from modkit_levels import (EPISODE, MODE, DEFAULT, GUID, read_script, call_arguments,
                           catalog, select_episode_start, select_entry, public_entry)


def checkpoint_guid(words):
    # Guest GUIDs contain little-endian GUID fields in big-endian dwords.
    return "{" + str(uuid.UUID(bytes_le=struct.pack("<4I", *words))).upper() + "}"


def read_live_checkpoints(pid):
    """Read the supported game's checkpoint registry; never write guest memory.

    The creator (823BD350 / 829539B8) registers 180-byte Checkpoint objects
    in a 16-slot array. The fallback finder (82953748) compares GUID words
    1..3: word 0 is replaced with the loaded stream's identity.
    """
    mappings = Path(f"/proc/{pid}/maps").read_text().splitlines()
    image = next((line for line in mappings if "xenia_memory_" in line and
                  int(line.split()[2], 16) == 0x82000000), None)
    if image is None:
        raise ValueError("Could not identify the guest image mapping")
    base = int(image.split("-")[0], 16) - 0x82000000
    with open(f"/proc/{pid}/mem", "rb", buffering=0) as memory:
        def read(address, size):
            memory.seek(base + address)
            data = memory.read(size)
            if len(data) != size:
                raise ValueError("Incomplete checkpoint memory read")
            return data

        if read(0x82005F44, 11) != b"Checkpoint\0":
            raise ValueError("Checkpoint schema does not match this game executable")
        table = struct.unpack(">17I", read(0x82E07DB0, 68))
        if table[-1] > 16:
            raise ValueError("Invalid checkpoint registry count")
        entries = []
        for pointer in table[:table[-1]]:
            words = struct.unpack(">45I", read(pointer, 180))
            if words[0] != 0x8218AA60:
                raise ValueError("Checkpoint registry contains an unexpected object type")
            entries.append(dict(guid=checkpoint_guid(words[7:11]),
                                default=bool(words[22] & 1), pointer=f"{pointer:08X}"))
        active = struct.unpack(">4I", read(0x82E07D94, 16))
        flow = struct.unpack(">I", read(0x82D08BA8, 4))[0]
        if not flow:
            raise ValueError("Gameflow manager is not loaded")
        episode_handle = struct.unpack(">I", read(flow + 160, 4))[0]
        # Unlike the Lua builder's handle, the manager owns package data
        # directly. Kind 5 identifies an episode; its name starts at +12.
        package = struct.unpack(">6I", read(episode_handle, 24))
        length = package[4] >> 16
        if package[2] != 5 or not 0 < length < 64:
            raise ValueError("Current episode package has an unexpected layout: " +
                             json.dumps(dict(pointer=f"{episode_handle:08X}",
                                             package=[f"{word:08X}" for word in package])))
        name = read(package[3], length + 1)
        if name[-1] != 0:
            raise ValueError("Current episode name is not terminated")
        episode = name[:-1].decode("ascii")
        if struct.unpack(">I", read(flow + 160, 4))[0] != episode_handle:
            raise ValueError("Current episode changed during the snapshot")
        if struct.unpack(">I", read(0x82E07DF0, 4))[0] != table[-1]:
            raise ValueError("Checkpoint registry changed during the snapshot")
    return dict(schema="45410809-checkpoint-180", checkpoints=entries,
                active_guid=checkpoint_guid(active), episode=episode,
                scope="Checkpoint objects loaded in this map; further streaming may add objects")


def link_file(source, destination):
    try:
        os.link(source, destination)
    except OSError as error:
        if error.errno != errno.EXDEV:
            raise
        raise ValueError("Put the test directory on the game data's filesystem to avoid copying the whole game") from error


def write_private(path, data):
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as file:
            file.write(data)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def skip_test_logos(data):
    # Reuse the launcher's existing list; rename only entries in the private
    # directory. The hard-linked video data at the source stays untouched.
    launcher = Path(__file__).resolve().parents[2] / "launcher/launcher.py"
    names = next(ast.literal_eval(node.value) for node in ast.parse(launcher.read_text()).body
                 if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and
                     target.id == "LOGO_MOVIES" for target in node.targets))
    names = {name + ".vp6" for name in names}
    movies = next((path for path in data.iterdir() if path.name.lower() == "movies"), None)
    renamed = []
    if movies:
        for path in movies.rglob("*"):
            if not path.is_file() or path.name.lower() not in names:
                continue
            if not path.parent.resolve().is_relative_to(data.resolve()):
                raise ValueError("Logo path leaves the private game directory")
            disabled = path.with_name(path.name + ".disabled")
            if disabled.exists():
                raise ValueError("Private game has both enabled and disabled copies of a logo")
            path.rename(disabled)
            renamed.append(str(path.relative_to(data)))
    return renamed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-data", type=Path, required=True)
    sub = parser.add_subparsers(dest="command", required=True)
    inventory = sub.add_parser("inventory")
    inventory.add_argument("--output", type=Path)
    prepare = sub.add_parser("prepare")
    prepare.add_argument("entry")
    prepare.add_argument("--checkpoint", help="Checkpoint GUID observed in this map")
    prepare.add_argument("--episode-start", action="store_true",
                         help="Keep the original full episode flow, including its intro movies")
    prepare.add_argument("--profile", type=Path,
                         help="Copy a saved test profile and leave the original gameflow intact")
    prepare.add_argument("--keep-logos", action="store_true")
    prepare.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.game_data.resolve(strict=True)
    script = source / "simpsons_gameflow.lua.original"
    if not script.is_file():
        script = source / "simpsons_gameflow.lua"
    text = read_script(script)
    entries = catalog(text)
    report = dict(source_sha256=hashlib.sha256(script.read_bytes()).hexdigest(),
                  scope="Map entry points declared by the gameflow; not every in-level checkpoint",
                  entries=[public_entry(e) for e in entries])
    if args.command == "inventory":
        serialized = json.dumps(report, indent=2) + "\n"
        if args.output:
            args.output.write_text(serialized)
        else:
            print(serialized, end="")
        return
    entry = next((e for e in entries if e["id"] == args.entry), None)
    if entry is None:
        raise ValueError("Unknown entry; run inventory first")
    if args.episode_start and (args.profile or args.checkpoint):
        raise ValueError("Episode start cannot override a checkpoint or saved profile")
    if args.profile:
        prepared = text.encode("utf-8", errors="surrogateescape")
    elif args.episode_start:
        prepared = select_episode_start(text, entry).encode("utf-8", errors="surrogateescape")
    else:
        prepared = select_entry(text, entry, args.checkpoint).encode("utf-8", errors="surrogateescape")
    destination = args.output.resolve()
    repo = Path(__file__).resolve().parents[2]
    if (destination.exists() or destination == source or destination in source.parents or
            source in destination.parents or destination == repo or repo in destination.parents):
        raise ValueError("Use a new test directory outside the repository and game data")
    destination.mkdir(parents=True)
    data = destination / "gamedata"
    shutil.copytree(source, data, copy_function=link_file, symlinks=True)
    target = data / "simpsons_gameflow.lua"
    if target.is_symlink():
        raise ValueError("Gameflow script must be a regular file")
    write_private(target, prepared)
    backup = data / "simpsons_gameflow.lua.original"
    if backup.exists():
        backup.unlink()
    report["skipped_logos"] = [] if args.keep_logos else skip_test_logos(data)
    if args.profile:
        profile = args.profile.resolve(strict=True)
        if destination == profile or destination in profile.parents or profile in destination.parents:
            raise ValueError("Saved test profile must be separate from the destination")
        # Saves must be copied: the game can overwrite them during the test.
        shutil.copytree(profile, destination / "userdata", symlinks=False,
                        ignore=shutil.ignore_patterns("cache", "logs"))
    else:
        (destination / "userdata").mkdir()
    report["start_method"] = ("saved_profile" if args.profile else
                              "episode_start" if args.episode_start else "map_entry")
    if args.episode_start:
        report["scope"] = "Original episode flow from its first standard entry, including movies"
    report["selected"] = public_entry(entry)
    if args.checkpoint:
        report["selected"]["entry_checkpoint"] = entry["checkpoint"]
        report["selected"]["checkpoint"] = args.checkpoint.upper()
    report["prepared_sha256"] = hashlib.sha256(prepared).hexdigest()
    report["game_data"] = str(data)
    report["user_data"] = str(destination / "userdata")
    (destination / "checkpoint.json").write_text(json.dumps(report, indent=2) + "\n")
    if hashlib.sha256(script.read_bytes()).hexdigest() != report["source_sha256"]:
        raise RuntimeError("Source script changed while preparing the test")
    print(json.dumps(report["selected"]))
    print("Prepared:", destination)


if __name__ == "__main__":
    main()
