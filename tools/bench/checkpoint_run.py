#!/usr/bin/env python3
"""Run one prepared checkpoint start with native GPU checks on Linux.

Uses the isolated game data and user data made by checkpoint_sweep.py. It
records the loaded checkpoint IDs, verifies an explicit target, moves the
camera, captures a GPU frame and requires a complete strict native report.
"""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import sys
import time
import uuid

import autorun
from checkpoint_sweep import read_live_checkpoints
from native_coverage import validate


def guid_tail(value):
    return uuid.UUID(value.strip("{}")).bytes[4:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prepared", type=Path)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--lib-dir", type=Path, required=True)
    parser.add_argument("--aot-shaders", type=Path)
    parser.add_argument("--cache", type=Path, help="Shared cache dedicated to these serial test runs")
    parser.add_argument("--scale", type=int, choices=(1, 2), default=1)
    parser.add_argument("--startup-wait", type=float, default=35)
    parser.add_argument("--load-timeout", type=float, default=180)
    parser.add_argument("--settle", type=float, default=8)
    parser.add_argument("--no-trace", action="store_true")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This runner uses the Linux game; captured frames can be replayed on other APIs")
    root = args.prepared.resolve(strict=True)
    descriptor = json.loads((root / "checkpoint.json").read_text())
    game = root / "gamedata"
    user = root / "userdata"
    if game.resolve() != Path(descriptor["game_data"]) or user.resolve() != Path(descriptor["user_data"]):
        parser.error("Prepared game/user data paths do not match their manifest")
    if user.is_symlink() or game.is_symlink():
        parser.error("Game and user data must belong to this prepared directory")
    if hashlib.sha256((game / "simpsons_gameflow.lua").read_bytes()).hexdigest() != descriptor["prepared_sha256"]:
        parser.error("Prepared gameflow changed after its manifest was written")
    executable = args.exe.resolve(strict=True)
    libraries = args.lib_dir.resolve(strict=True)
    runtime = libraries / "librexruntimerd.so"
    if not runtime.is_file():
        parser.error("The runtime directory does not contain librexruntimerd.so")
    run = root / "run"
    run.mkdir(exist_ok=False)
    if args.cache:
        cache = args.cache.resolve()
        if cache == user or cache in user.parents or user in cache.parents:
            parser.error("Shared cache must be separate from this test profile")
        cache.mkdir(parents=True, exist_ok=True)
        (user / "cache").symlink_to(cache, target_is_directory=True)
    exe = run / "simpsons"
    try:
        os.link(executable, exe)
    except OSError as error:
        if error.errno != errno.EXDEV:
            raise
        shutil.copy2(executable, exe)
    autorun.BUILD_DIR = str(executable.parent)
    autorun.GAMEDATA = str(game)
    autorun.LIB_OVERRIDE = str(libraries)
    autorun.LIB_DIR = str(executable.parent)
    settings = dict(autorun.BASE_OVERRIDES,
                    resolution_scale=str(args.scale), frame_rate="30", menu_frame_rate="30",
                    native_gpu_diagnostics="true", native_gpu_strict="true",
                    native_gpu_report_path=json.dumps(str(run / "native.json")),
                    shader_inventory_csv=json.dumps(str(run / "inventory.csv")),
                    pipeline_inventory_json=json.dumps(str(run / "pipelines.json")),
                    user_data_root=json.dumps(str(user)),
                    input_inject_file=json.dumps(str(run / "input.cmd")),
                    trace_gpu_prefix=json.dumps(str(run / "trace")))
    if args.aot_shaders:
        settings["aot_shader_path"] = json.dumps(str(args.aot_shaders.resolve(strict=True)))
    autorun.write_config(str(run), settings)
    runner = autorun.Run(str(run), str(exe), str(user))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    # Keep the child inspectable, without dumping the large guest/GPU mappings
    # if a strict native check aborts. This setting is inherited by the child.
    Path("/proc/self/coredump_filter").write_text("0\n")
    reached = False
    target_verified = False
    registry = None
    failure = None
    try:
        runner.start()
        (run / "pid.txt").write_text(str(runner.proc.pid))
        if str(runtime) not in Path(f"/proc/{runner.proc.pid}/maps").read_text():
            raise RuntimeError("Game loaded a different runtime")
        if not runner.waitlog(r"\[native\]", 120):
            raise RuntimeError("Startup did not render")
        time.sleep(args.startup_wait)
        runner.shot("title")
        runner.pad("START", 0.3)
        time.sleep(2)
        for _ in range(3):
            runner.pad("A", 0.3)
            time.sleep(2)
        runner.shot("after-start")
        deadline = time.monotonic() + args.load_timeout
        next_press = time.monotonic() + 1
        stable = None
        while runner.alive() and time.monotonic() < deadline:
            text = Path(runner.log_path).read_text(errors="replace")
            states = re.findall(r"Frame pacing: [^\n]+", text)
            running = bool(states and "fps, level running" in states[-1])
            if running:
                if stable is None:
                    stable = time.monotonic()
                if time.monotonic() - stable >= args.settle:
                    reached = True
                    break
            else:
                stable = None
                if time.monotonic() >= next_press:
                    runner.pad("A", 0.15)
                    next_press = time.monotonic() + 1
            time.sleep(0.25)
        if not reached:
            runner.shot("unreached")
            raise RuntimeError("Did not reach stable gameplay")
        registry = read_live_checkpoints(runner.proc.pid)
        (run / "checkpoints.json").write_text(json.dumps(registry, indent=2) + "\n")
        if registry["episode"] != descriptor["selected"]["episode"]:
            raise RuntimeError("Loaded a different episode: " + registry["episode"])
        target = descriptor["selected"]["checkpoint"]
        if target:
            present = any(guid_tail(c["guid"]) == guid_tail(target) for c in registry["checkpoints"])
            target_verified = present and guid_tail(registry["active_guid"]) == guid_tail(target)
            if not target_verified:
                raise RuntimeError("Requested checkpoint did not become active: " + json.dumps(registry))
        else:
            target_verified = any(guid_tail(c["guid"]) == guid_tail(registry["active_guid"])
                                  for c in registry["checkpoints"])
            if not target_verified:
                raise RuntimeError("No loaded checkpoint matches the active map start")
        runner.shot("level")
        runner.pad("R_RIGHT", 1)
        runner.shot("camera-right")
        runner.pad("R_LEFT", 2)
        runner.shot("camera-left")
        if not args.no_trace:
            runner.trace("checkpoint-frame")
            if not list((run / "trace").glob("*.xtr")):
                raise RuntimeError("Checkpoint GPU frame capture was not produced")
    except Exception as error:
        failure = str(error)
    finally:
        if runner.proc:
            runner.stop()
        (run / "summary.txt").write_text("\n".join(runner.notes) + "\n")
    text = Path(runner.log_path).read_text(errors="replace") if runner.log_path else ""
    coverage = json.loads((run / "native.json").read_text()) if (run / "native.json").exists() else {}
    errors = validate(coverage, "vulkan")
    errors += [line for line in text.splitlines() if "[error]" in line or "[critical]" in line or
               "Shutdown did not finish" in line or "shutdown watchdog" in line.lower()]
    result = dict(passed=reached and target_verified and failure is None and not errors and
                  runner.proc is not None and runner.proc.returncode == 0,
                  selected=descriptor["selected"], reached_gameplay=reached,
                  target_verified=target_verified, failure=failure, errors=errors,
                  rc=runner.proc.returncode if runner.proc else None, checkpoints=registry,
                  coverage=coverage, runtime_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
                  executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  source_sha256=descriptor["source_sha256"],
                  scope="One loaded checkpoint and camera sweep; not every event reachable from it")
    (run / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key not in ("coverage", "checkpoints")}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
