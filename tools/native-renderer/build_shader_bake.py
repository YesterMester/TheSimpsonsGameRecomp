#!/usr/bin/env python3
"""Build the storage-only shader exporter with the SDK's actual compiler flags."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    sdk = root / "tools/rexglue-sdk"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("runtime", type=Path)
    args = parser.parse_args()
    entry = next(row for row in json.loads((sdk / "build/compile_commands.json").read_text())
                 if row["file"].endswith("src/graphics/graphics_system.cpp"))
    original = entry.get("arguments") or shlex.split(entry["command"])
    flags = []
    skip = False
    for word in original[1:]:
        if skip:
            skip = False
        elif word in ("-o", "-c", "-MF", "-MT"):
            skip = True
        elif word != "-MD":
            flags.append(word)
    linker = Path(original[0]).parent / "ld.lld"
    if not linker.is_file():
        linker = root / "tools/clang20/bin/ld.lld"
    output, runtime = args.output.resolve(), args.runtime.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [original[0], *flags, "-g0", str(root / "tools/native-renderer/shader_bake.cpp"),
               "--ld-path=" + str(linker), "-L" + str(runtime), "-lrexruntimerd",
               "-Wl,-rpath," + str(runtime), "-Wl,--enable-new-dtags",
               "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(output)]
    subprocess.run(command, cwd=entry["directory"], check=True)
    print("Built", output)


if __name__ == "__main__":
    main()
