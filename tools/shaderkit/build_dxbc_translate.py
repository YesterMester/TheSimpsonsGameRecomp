#!/usr/bin/env python3
"""Build the DX11 shader compiler against a source-built Linux runtime."""

import argparse
from concurrent.futures import ThreadPoolExecutor
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
    parser.add_argument("--output", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--geometry", action="store_true",
                        help="build the shared primitive shader exporter")
    mode.add_argument("--transfer", action="store_true",
                      help="build the shared render target transfer exporter")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    entries = json.loads((args.sdk_build / "compile_commands.json").read_text())
    entry = next(e for e in entries if Path(e["file"]).name == "spirv_translator.cpp")
    command = shlex.split(entry["command"])
    flags = []
    skip = False
    for arg in command[1:]:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
            skip = True
        elif arg not in ("-MD", "-MMD"):
            flags.append(arg)
    name = "dxbc_geometry" if args.geometry else "dxbc_transfer" if args.transfer else "dxbc_translate"
    output = (args.output or Path(__file__).parent / "out" / name).resolve()
    runtime = args.runtime.resolve()
    work = output.with_name(output.name + "-objects")
    work.mkdir(parents=True, exist_ok=True)
    if args.geometry:
        sources = [sdk / "src/graphics/util/dxbc_geometry.cpp",
                   sdk / "thirdparty/dxbc/DXBCChecksum.cpp",
                   Path(__file__).with_name("dxbc_geometry.cpp")]
    elif args.transfer:
        sources = [sdk / "src/graphics/util/dxbc_render_target_transfer.cpp",
                   sdk / "src/graphics/util/dxbc_float.cpp",
                   sdk / "thirdparty/dxbc/DXBCChecksum.cpp",
                   Path(__file__).with_name("dxbc_transfer.cpp")]
    else:
        sources = [sdk / "src/graphics/pipeline/shader" / name for name in (
            "dxbc.cpp", "dxbc_translator.cpp", "dxbc_translator_alu.cpp",
            "dxbc_translator_fetch.cpp", "dxbc_translator_memexport.cpp",
            "dxbc_translator_om.cpp")]
        sources += [sdk / "src/graphics/d3d11/shader_bytecode.cpp",
                    sdk / "thirdparty/dxbc/DXBCChecksum.cpp",
                    Path(__file__).with_name(name + ".cpp")]

    def compile_source(source):
        obj = work / (str(source.relative_to(root)).replace(os.sep, "_") + ".o")
        subprocess.run([command[0], *flags, "-I" + str(sdk), "-c", str(source),
                        "-o", str(obj)], cwd=entry["directory"], check=True)
        return obj

    with ThreadPoolExecutor(max_workers=args.jobs) as workers:
        objects = list(workers.map(compile_source, sources))
    temporary = output.with_name(output.name + ".next")
    subprocess.run([command[0], "-o", str(temporary), *map(str, objects),
                    "-L" + str(runtime), "-lrexruntimerd", "-lTracyClientrd",
                    "-Wl,-rpath," + str(runtime), "-Wl,--enable-new-dtags",
                    "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2"], check=True)
    os.replace(temporary, output)
    print(output)


if __name__ == "__main__":
    main()
