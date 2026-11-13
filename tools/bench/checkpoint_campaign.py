#!/usr/bin/env python3
"""Sweep standard map entry points and optionally discover further checkpoints.

This samples loaded checkpoints and camera views. It does not claim that every
scripted event, boss phase or resource transition in the campaign was executed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from checkpoint_sweep import catalog, public_entry, read_script, write_private
from checkpoint_run import guid_tail
from native_coverage import validate


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def state_key(entry, checkpoint):
    return entry["id"], guid_tail(checkpoint) if checkpoint else None


def load_qualified(previous, roots, manifest):
    completed = {}
    for row in previous["states"]:
        path = Path(row["report"]).resolve()
        if not any(path.is_relative_to(root) for root in roots):
            raise ValueError("Saved case report leaves the declared sweep directories")
        if not row.get("passed"):
            continue
        saved = json.loads(path.read_text())
        registry = saved.get("checkpoints") or {}
        selected = saved.get("selected") or {}
        active = registry.get("active_guid")
        target = selected.get("checkpoint")
        matches = active and any(guid_tail(point["guid"]) == guid_tail(active)
                                 for point in registry.get("checkpoints", []))
        if target:
            matches = matches and guid_tail(active) == guid_tail(target)
        if (not saved.get("passed") or not saved.get("target_verified") or not matches or
                saved.get("rc") != 0 or saved.get("errors") or
                saved.get("runtime_sha256") != manifest["runtime_sha256"] or
                registry.get("episode") != selected.get("episode") or
                validate(saved.get("coverage", {}), "vulkan")):
            raise ValueError("Previously successful case no longer has a qualified report")
        completed[state_key(selected, target)] = dict(saved, report=str(path),
                                                      discovered=row.get("discovered", False))
    return completed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-data", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--lib-dir", type=Path, required=True)
    parser.add_argument("--aot-shaders", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resume", action="store_true",
                        help="Reuse qualified states from this sweep without overwriting earlier attempts")
    parser.add_argument("--reuse-sweep", type=Path,
                        help="Reuse matching qualified cases from another sweep; failed cases are never reused")
    parser.add_argument("--episodes", nargs="+")
    parser.add_argument("--discover", action="store_true",
                        help="Also try loaded checkpoint IDs not listed as timed challenges")
    parser.add_argument("--scale", type=int, choices=(1, 2), default=1)
    parser.add_argument("--startup-wait", type=float, default=35)
    args = parser.parse_args()
    source = args.game_data.resolve(strict=True)
    script = source / "simpsons_gameflow.lua.original"
    if not script.is_file():
        script = source / "simpsons_gameflow.lua"
    entries = catalog(read_script(script))
    episodes = {entry["episode"] for entry in entries}
    if args.episodes and not set(args.episodes) <= episodes:
        parser.error("Unknown episode in --episodes")
    selected = [entry for entry in entries if entry["mode"] == "MODE_STANDARD" and
                (not args.episodes or entry["episode"] in args.episodes)]
    root = args.output.resolve()
    manifest = dict(schema="episode-verified-1", gameflow_sha256=digest(script),
                    executable_sha256=digest(args.exe.resolve(strict=True)),
                    runtime_sha256=digest(args.lib_dir.resolve(strict=True) / "librexruntimerd.so"),
                    scale=args.scale, entries=[entry["id"] for entry in selected],
                    discover=args.discover,
                    reused_sweep=str(args.reuse_sweep.resolve(strict=True)) if args.reuse_sweep else None,
                    aot_shaders=str(args.aot_shaders.resolve(strict=True)) if args.aot_shaders else None)
    completed = {}
    history = []
    allowed_roots = [root]
    if args.reuse_sweep:
        previous_root = args.reuse_sweep.resolve(strict=True)
        previous = json.loads((previous_root / "report.json").read_text())
        required = ("schema", "gameflow_sha256", "executable_sha256", "runtime_sha256", "scale", "aot_shaders")
        if any(previous.get("manifest", {}).get(key) != manifest[key] for key in required):
            parser.error("Reusing cases requires matching gameflow, executable, runtime, scale and shaders")
        allowed_roots.append(previous_root)
        completed.update(load_qualified(previous, allowed_roots, manifest))
    if args.resume:
        previous = json.loads((root / "report.json").read_text())
        if previous.get("manifest") != manifest:
            parser.error("Resume requires the same gameflow, executable, runtime and sweep settings")
        history = previous.get("previous_reports", [])
        snapshot = root / f"report-before-resume-{len(history) + 1:03d}.json"
        with snapshot.open("xb") as file:
            file.write((root / "report.json").read_bytes())
        history = history + [snapshot.name]
        completed.update(load_qualified(previous, allowed_roots, manifest))
    else:
        root.mkdir(parents=True, exist_ok=False)
    tools = Path(__file__).resolve().parent
    queue = [(entry, entry["checkpoint"], False) for entry in selected]
    seen = {(entry["episode"], guid_tail(checkpoint) if checkpoint else None)
            for entry, checkpoint, _ in queue}
    timed = {(entry["episode"], guid_tail(entry["checkpoint"])) for entry in entries
             if entry["mode"] == "MODE_TIMED" and entry["checkpoint"]}
    report = dict(complete=False, passed=False, manifest=manifest, previous_reports=history,
                  states=[], excluded=[],
                  scope="Standard map starts and loaded checkpoint candidates; not every scripted event")

    def save():
        write_private(root / "report.json", (json.dumps(report, indent=2) + "\n").encode())

    save()
    index = 0
    while index < len(queue):
        entry, checkpoint, discovered = queue[index]
        case = root / (f"{index + 1:03d}-" + entry["episode"])
        key = state_key(entry, checkpoint)
        if key in completed:
            row = dict(completed[key], reused=True)
            case = Path(row["report"]).parent.parent
            result_code = 0
        else:
            attempt = 0
            original = case
            while case.exists():
                attempt += 1
                case = original.with_name(original.name + f"-retry{attempt}")
            row, result_code = run_case(args, tools, source, root, entry, checkpoint, case, manifest)
        if row["passed"] and row.get("runtime_sha256") != manifest["runtime_sha256"]:
            row.update(passed=False, failure="Runtime changed during the sweep")
        row["discovered"] = discovered
        report["states"].append(row)
        save()
        print(json.dumps(dict(case=case.name, passed=row["passed"], checkpoint=checkpoint,
                              discovered=discovered, reused=key in completed)), flush=True)
        if result_code or not row["passed"]:
            return 1
        if args.discover:
            seen.add((entry["episode"], guid_tail(row["checkpoints"]["active_guid"])))
            for candidate in row["checkpoints"]["checkpoints"]:
                identity = (entry["episode"], guid_tail(candidate["guid"]))
                if candidate["default"] or identity in seen:
                    continue
                seen.add(identity)
                if identity in timed:
                    report["excluded"].append(dict(episode=entry["episode"],
                                                   checkpoint=candidate["guid"],
                                                   reason="Original timed mode is outside this standard-mode sweep"))
                else:
                    queue.append((entry, candidate["guid"], True))
        index += 1
    report["complete"] = True
    report["passed"] = bool(report["states"]) and all(row["passed"] for row in report["states"])
    save()
    return 0 if report["passed"] else 1


def run_case(args, tools, source, root, entry, checkpoint, case, manifest):
    prepare = [sys.executable, str(tools / "checkpoint_sweep.py"), "--game-data", str(source),
               "prepare", entry["id"], "--output", str(case)]
    if checkpoint:
        prepare += ["--checkpoint", checkpoint]
    with (root / (case.name + ".log")).open("w") as log:
        prepared = subprocess.run(prepare, stdout=log, stderr=subprocess.STDOUT)
        if prepared.returncode:
            selected = dict(public_entry(entry), checkpoint=checkpoint)
            return dict(selected=selected, passed=False, report=str(case / "run/report.json"),
                        failure="Preparation failed", rc=prepared.returncode), prepared.returncode
        descriptor = json.loads((case / "checkpoint.json").read_text())
        if descriptor["source_sha256"] != manifest["gameflow_sha256"]:
            return dict(selected=descriptor["selected"], passed=False,
                        report=str(case / "run/report.json"), failure="Gameflow changed during the sweep"), 1
        command = [sys.executable, str(tools / "checkpoint_run.py"), str(case),
                   "--exe", str(args.exe.resolve()), "--lib-dir", str(args.lib_dir.resolve()),
                   "--cache", str(root / "cache"), "--scale", str(args.scale),
                   "--startup-wait", str(args.startup_wait)]
        if args.aot_shaders:
            command += ["--aot-shaders", str(args.aot_shaders.resolve())]
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    result_path = case / "run/report.json"
    if result_path.exists():
        row = json.loads(result_path.read_text())
    else:
        row = dict(passed=False, failure="Runner did not produce a report")
    row["report"] = str(result_path)
    return row, result.returncode


if __name__ == "__main__":
    sys.exit(main())
