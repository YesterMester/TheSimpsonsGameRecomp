#!/usr/bin/env python3
"""Execute the game's original capsule and rigid-body configuration constructors.

This checks collision definitions and mass properties, not world simulation or
skeletal ragdoll behavior. Extracted routines stay in the chosen output folder.
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
    source = '#include "simpsons_init.h"\n\n'
    identities = {}
    for number, names in {
        73: ["sub_82AFB470"], 70: ["sub_82AB0978"],
        45: ["sub_827A4D28", "sub_827A4A90", "sub_827A4A48", "sub_827A4D50",
             "sub_827A4B38", "sub_827A4B88", "sub_827A4BE8", "sub_827A4C48",
             "sub_827A4CB0"],
        65: ["__savegprlr_28", "__restgprlr_28", "__savegprlr_29", "__restgprlr_29"],
    }.items():
        for name in names:
            body = selected(generated / f"simpsons_recomp.{number}.cpp", [name])
            source += body + "\n\n"
            identities[name] = hashlib.sha256(body.encode()).hexdigest()
    original = output / "original-ragdoll-body-info.cpp"
    original.write_text(source)
    (output / "original-routines.json").write_text(json.dumps(identities, indent=2) + "\n")
    sdk = ROOT / "tools/rexglue-sdk"
    includes = [generated, ROOT / "simpsons/src", sdk / "include"]
    includes += [sdk / "thirdparty" / path for path in
                 ["simde", "fmt/include", "spdlog/include", "sdl3/include", "vulkan-headers/include"]]
    bundled = ROOT / "tools/clang20/bin/clang++"
    compiler = args.cxx or (str(bundled) if bundled.is_file() else shutil.which("clang++"))
    if not compiler:
        parser.error("Clang is required to compile the generated routines")
    command = [compiler, "-std=c++23", "-O2", "-march=x86-64", "-DSPDLOG_FMT_EXTERNAL"]
    command += ["-I" + str(path) for path in includes]
    command += [str(original), str(ROOT / "simpsons/src/native_ragdoll_body.cpp"),
                str(ROOT / "simpsons/src/native_ragdoll_joint.cpp"),
                str(Path(__file__).with_suffix(".cpp")), "-o", str(output / "ragdoll-body-check")]
    subprocess.run(command, check=True)
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(output / "ragdoll-body-check")], capture_output=True, text=True)
    print(result.stdout, end="")
    (output / "report.txt").write_text(result.stdout)
    if result.returncode:
        print(result.stderr, file=sys.stderr, end="")
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
