#!/usr/bin/env python3
"""Check native presentation serialization without running a GPU backend."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    paths = [root / p for p in (
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_presentation.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp",
        "tools/rexglue-sdk/src/graphics/command_processor.cpp",
        "tools/rexglue-sdk/src/graphics/packet_disassembler.cpp",
        "tools/bench/native_gpu_presentation_check.cpp",
        "tools/native-renderer/check_gpu_presentation.py")]
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    pins = {str(p.relative_to(root)): digest(p) for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-presentation-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        command = [compiler, *flags, "-g0", str(root / "tools/bench/native_gpu_presentation_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=120)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Presentation check exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed") or report.get("rendered_frames") != 0:
            raise RuntimeError("Presentation serialization contract did not pass")
        if pins != {str(p.relative_to(root)): digest(p) for p in paths}:
            raise RuntimeError("Presentation sources changed during qualification")
        report["source_hashes"] = pins
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=root / "tools/rexglue-sdk/build")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    run(root, args.sdk_build.resolve(), args.output_dir.resolve())


if __name__ == "__main__":
    main()
