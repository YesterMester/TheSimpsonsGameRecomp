#!/usr/bin/env python3
"""Check the joint adapter against unchanged Havok routines in the generated game.

This checks joint data and native validation, not skeleton driving, world
collision, character recovery or a complete playable ragdoll.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from camera_input_check import selected

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cxx", help="Clang C++ compiler (defaults to bundled Clang or PATH)")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This fixture uses Linux sparse guest memory")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = ROOT / "simpsons/generated/default"
    routines = {
        74: ["sub_82B20270", "sub_82B1FF18"],
        77: ["sub_82B5FF80"],
        65: ["__savegprlr_28", "__restgprlr_28"],
    }
    source = '#include "simpsons_init.h"\n\n'
    identities = {}
    for number, names in routines.items():
        for name in names:
            body = selected(generated / f"simpsons_recomp.{number}.cpp", [name])
            source += body + "\n\n"
            identities[name] = hashlib.sha256(body.encode()).hexdigest()
    (output / "original-ragdoll-joint.cpp").write_text(source)
    (output / "original-routines.json").write_text(json.dumps(identities, indent=2) + "\n")
    sdk = ROOT / "tools/rexglue-sdk"
    includes = [generated, ROOT / "simpsons/src", sdk / "include"]
    includes += [sdk / "thirdparty" / path for path in
                 ["simde", "fmt/include", "spdlog/include", "sdl3/include", "vulkan-headers/include"]]
    bundled = ROOT / "tools/clang20/bin/clang++"
    compiler = args.cxx or (str(bundled) if bundled.is_file() else shutil.which("clang++"))
    if not compiler:
        parser.error("Clang is required to compile the generated routines")
    command = [compiler, "-std=c++23", "-O2",
               "-march=x86-64", "-DSPDLOG_FMT_EXTERNAL"]
    command += ["-I" + str(path) for path in includes]
    command += [str(output / "original-ragdoll-joint.cpp"),
                str(ROOT / "simpsons/src/native_ragdoll_joint.cpp"),
                str(Path(__file__).with_suffix(".cpp")), "-o", str(output / "ragdoll-joint-check")]
    subprocess.run(command, check=True)
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(output / "ragdoll-joint-check")],
                            capture_output=True, text=True)
    print(result.stdout, end="")
    (output / "report.txt").write_text(result.stdout)
    if result.returncode:
        print(result.stderr, file=sys.stderr, end="")
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
