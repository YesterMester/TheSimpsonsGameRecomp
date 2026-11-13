#!/usr/bin/env python3
"""Check both production camera hooks against the actual generated handlers on Linux."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def selected(path, names):
    source = path.read_text()
    result = []
    for name in names:
        matches = re.findall(r"^DEFINE_REX_FUNC\(" + re.escape(name) + r"\) \{.*?^\}",
                             source, re.M | re.S)
        if len(matches) != 1:
            raise RuntimeError(f"Expected exactly one generated {name} in {path}")
        result.append(matches[0])
    return "\n\n".join(result)


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
    source += selected(generated / "simpsons_recomp.58.cpp", ["sub_8295C9C0", "sub_82960870"])
    source += "\n\n" + selected(generated / "simpsons_recomp.65.cpp",
                                ["__savegprlr_29", "__restgprlr_29", "__savefpr_28", "__restfpr_28"])
    (output / "original-camera.cpp").write_text(source)
    bundled = ROOT / "tools/clang20/bin/clang++"
    compiler = args.cxx or (str(bundled) if bundled.is_file() else shutil.which("clang++"))
    if not compiler:
        parser.error("Clang is required to compile the generated functions")
    sdk = ROOT / "tools/rexglue-sdk"
    includes = [generated, ROOT / "simpsons/src", sdk / "include"]
    includes += [sdk / "thirdparty" / name for name in
                 ("simde", "fmt/include", "spdlog/include", "sdl3/include", "vulkan-headers/include")]
    command = [compiler, "-std=c++23", "-O2", "-march=x86-64", "-DSPDLOG_FMT_EXTERNAL"]
    command += ["-I" + str(path) for path in includes]
    command += [str(output / "original-camera.cpp"), str(ROOT / "simpsons/src/camera_input.cpp"),
                str(Path(__file__).with_suffix(".cpp")), "-o", str(output / "camera-input-check")]
    subprocess.run(command, check=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    subprocess.run([str(output / "camera-input-check")], check=True)


if __name__ == "__main__":
    main()
