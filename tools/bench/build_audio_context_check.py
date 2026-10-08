#!/usr/bin/env python3
"""Build the audio buffer check against the current source-built runtime."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    sdk = root / "tools/rexglue-sdk"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=sdk / "build")
    parser.add_argument("--runtime", type=Path, default=sdk / "out/linux-amd64")
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).parent / "out/audio_context_check")
    args = parser.parse_args()
    entries = json.loads((args.sdk_build / "compile_commands.json").read_text())
    entry = next(e for e in entries if Path(e["file"]).name == "xma_context.cpp")
    command = shlex.split(entry["command"])
    flags = []
    skip = False
    for arg in command[1:]:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg not in ("-MD", "-MMD"):
            flags.append(arg)
    output = args.output.resolve()
    runtime = args.runtime.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.with_suffix(".o")
    temporary = output.with_name(output.name + ".next")
    source = Path(__file__).with_name("audio_context_check.cpp")
    subprocess.run([command[0], *flags, "-fno-access-control", "-c", str(source),
                    "-o", str(obj)], cwd=entry["directory"], check=True)
    subprocess.run([command[0], "-o", str(temporary), str(obj),
                    "-L" + str(runtime), "-lrexruntimerd", "-lTracyClientrd",
                    "-Wl,-rpath," + str(runtime), "-Wl,--enable-new-dtags",
                    "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2"], check=True)
    os.replace(temporary, output)
    print(output)


if __name__ == "__main__":
    main()
