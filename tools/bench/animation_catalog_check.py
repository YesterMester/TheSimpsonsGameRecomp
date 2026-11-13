#!/usr/bin/env python3
"""Compare the animation catalog with the game's original hash and bank lookup.

Generated game routines are extracted to the private output directory. This
qualifies catalog traversal and labels, not animation playback or recovery.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys

from camera_input_check import selected

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    matches = re.findall(r"^" + re.escape(signature) + r" \{.*?^\}",
                         path.read_text(), re.M | re.S)
    if len(matches) != 1:
        raise RuntimeError(f"Expected one {signature} in {path}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cxx")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This fixture uses Linux sparse guest memory")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = ROOT / "simpsons/generated/default"
    originals = {}
    for unit, names in ((41, ("sub_827451C0",)),
                        (44, ("sub_8278AD00", "sub_8278B478"))):
        for name in names:
            originals[name] = selected(generated / f"simpsons_recomp.{unit}.cpp", [name])
    original_file = output / "original-animation-lookup.cpp"
    original_file.write_text('#include "simpsons_init.h"\n\n' +
                             "\n\n".join(originals.values()) + "\n")
    catalog = ROOT / "simpsons/src/spawner.cpp"
    game_thread = ROOT / "simpsons/src/game_thread.cpp"
    plausible = function(catalog, "bool Plausible(uint32_t address)")
    listing = function(catalog, "std::vector<AnimClipInfo> ListEntityAnimations(const uint8_t* base, uint32_t entity)")
    hashing = function(game_thread, "uint32_t Hash(std::string_view name)")
    production_file = output / "production-animation-catalog.cpp"
    production_file.write_text(
        '#include "spawner.h"\n#include "game_thread.h"\n#include "guest_memory.h"\n'
        '#include <algorithm>\n#include <set>\n#include <fmt/format.h>\n'
        'namespace scores {\n' + hashing + '\n}\n'
        'namespace spawner {\nusing simpsons::LoadGuestU32;\n' +
        plausible + '\n' + listing + '\n}\n')
    sdk = ROOT / "tools/rexglue-sdk"
    includes = [generated, ROOT / "simpsons/src", sdk / "include"]
    includes += [sdk / "thirdparty" / path for path in
                 ("simde", "fmt/include", "spdlog/include", "sdl3/include", "vulkan-headers/include")]
    fixture = Path(__file__).with_suffix(".cpp")
    bundled = ROOT / "tools/clang20/bin/clang++"
    compiler = args.cxx or (str(bundled) if bundled.is_file() else shutil.which("clang++"))
    if not compiler:
        parser.error("Clang is required to compile the generated functions")
    command = [compiler, "-std=c++23", "-O2",
               "-march=x86-64", "-DSPDLOG_FMT_EXTERNAL", "-DFMT_HEADER_ONLY"]
    command += ["-I" + str(path) for path in includes]
    command += [str(original_file), str(production_file), str(fixture),
                "-o", str(output / "animation-catalog-check")]
    subprocess.run(command, check=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(output / "animation-catalog-check")],
                            capture_output=True, text=True)
    report = {
        "scope": "Animation-bank traversal and authored names; not playback or get-up",
        "command": command, "exit_code": result.returncode,
        "stdout": result.stdout, "stderr": result.stderr,
        "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (catalog, game_thread, fixture, Path(__file__))},
        "original_routines": {name: hashlib.sha256(body.encode()).hexdigest()
                              for name, body in originals.items()},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(result.stderr or result.returncode)


if __name__ == "__main__":
    main()
