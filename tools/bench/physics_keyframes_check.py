#!/usr/bin/env python3
"""Check native hard-keyframe feedback against the original generated calculation."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

from camera_input_check import selected

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This check reserves guest memory with Linux mmap")
    import resource

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = ROOT / "simpsons/generated/default"
    source = '#include "simpsons_init.h"\n\n'
    source += selected(generated / "simpsons_recomp.69.cpp", ["sub_82A93FE0"])
    source += "\n\n" + selected(generated / "simpsons_recomp.65.cpp",
                                ["__savegprlr_29", "__restgprlr_29"])
    source += "\n\n" + selected(generated / "simpsons_recomp.64.cpp", ["sub_82A1BD48"])
    (output / "original-keyframe.cpp").write_text(source)
    bundled = ROOT / "tools/clang20/bin/clang++"
    compiler = args.cxx or (str(bundled) if bundled.is_file() else shutil.which("clang++"))
    if not compiler:
        parser.error("Clang is required to compile the generated calculation")
    sdk = ROOT / "tools/rexglue-sdk"
    includes = [generated, ROOT / "simpsons/src", sdk / "include"]
    includes += [sdk / "thirdparty" / name for name in
                 ("simde", "fmt/include", "spdlog/include", "sdl3/include", "vulkan-headers/include")]
    command = [compiler, "-std=c++23", "-O2", "-march=x86-64", "-DSPDLOG_FMT_EXTERNAL"]
    command += ["-I" + str(path) for path in includes]
    command += [str(output / "original-keyframe.cpp"), str(ROOT / "simpsons/src/physics_keyframes.cpp"),
                str(Path(__file__).with_suffix(".cpp")), "-o", str(output / "physics-keyframes-check")]
    subprocess.run(command, check=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    subprocess.run([str(output / "physics-keyframes-check")], check=True)


if __name__ == "__main__":
    main()
