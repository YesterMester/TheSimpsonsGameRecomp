#!/usr/bin/env python3
"""Compare native call arguments with executed, current XDK writer bodies.

Uses the existing SDK compiler configuration and a small standalone executable.
It does not rebuild the game, launch it, or treat backend replay as hook proof.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(path, name):
    source = path.read_text()
    match = re.search(r"^DEFINE_REX_FUNC\(" + re.escape(name) + r"\) \{", source, re.M)
    if not match:
        raise RuntimeError(f"No reference body for {name} in {path}")
    end = source.find("\nDEFINE_REX_FUNC(", match.end())
    body = source[match.start():end if end >= 0 else len(source)].rstrip()
    if not body.endswith("}"):
        raise RuntimeError(f"Cannot bound the reference body for {name}")
    return body


def run(root, build, output):
    output.mkdir(parents=True, exist_ok=True)
    generated = root / "simpsons/generated/default"
    draws = ["sub_824626B8", "sub_82462810", "sub_82464590", "sub_824646E8"]
    helpers = [f"__{kind}gprlr_{number}" for kind in ("save", "rest") for number in (20, 21)]
    names = helpers + draws
    renames = {name: "reference_" + name.lstrip("_") for name in names}
    pieces = [f"void {renames[name]}(PPCContext&, uint8_t*);" for name in names]
    sources = {}
    for name in names:
        path = generated / ("simpsons_recomp.17.cpp" if name in draws else "simpsons_recomp.65.cpp")
        body = extract(path, name)
        sources[name] = hashlib.sha256(body.encode()).hexdigest()
        body = body.replace(f"DEFINE_REX_FUNC({name})", f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name in draws:
            body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n  ++reference_writer_calls;", 1)
        for original, reference in renames.items():
            body = re.sub(r"\b" + re.escape(original) + r"\(ctx, base\)", reference + "(ctx, base)", body)
        pieces.append(body)
    (output / "native_gpu_reference.inc").write_text("\n\n".join(pieces) + "\n")
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(e for e in entries if e["file"].endswith("/src/graphics/graphics_system.cpp"))
    args = shlex.split(entry["command"])
    compiler, args = args[0], args[1:]
    flags = []
    i = 0
    while i < len(args):
        if args[i] in ("-o", "-c", "-MF", "-MT", "-MQ"):
            i += 2
        elif args[i] in ("-MD", "-MMD", "-g"):
            i += 1
        else:
            flags.append(args[i])
            i += 1
    executable = output / "native_gpu_calls_check"
    command = [compiler, *flags, "-O2", "-g0", "-std=c++23",
               "-I" + str(generated), "-I" + str(root / "simpsons/src"), "-I" + str(output),
               str(root / "tools/bench/native_gpu_calls_check.cpp"), "-o", str(executable)]
    linker = root / "tools/clang20/bin/ld.lld"
    if linker.exists():
        command.append("--ld-path=" + str(linker))
    if Path("/lib64/ld-linux-x86-64.so.2").exists():
        command.append("-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2")
    inputs = [root / "simpsons/src/native_gpu_calls.h",
              root / "simpsons/src/guest_memory.h",
              root / "simpsons/src/native_gpu_memory.h",
              root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
              root / "tools/bench/native_gpu_calls_check.cpp"]
    hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    with (output / "build.log").open("w") as log:
        subprocess.run(command, cwd=entry["directory"], stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    report = json.loads(result.stdout)
    if report.get("passed") is not True:
        raise RuntimeError("The native call oracle did not pass")
    for path in inputs:
        if hashes[str(path.relative_to(root))] != hashlib.sha256(path.read_bytes()).hexdigest():
            raise RuntimeError(f"Source changed during qualification: {path}")
    for name in names:
        path = generated / ("simpsons_recomp.17.cpp" if name in draws else "simpsons_recomp.65.cpp")
        if hashlib.sha256(extract(path, name).encode()).hexdigest() != sources[name]:
            raise RuntimeError(f"Reference body changed during qualification: {name}")
    report.update(reference_body_hashes=sources, source_hashes=hashes)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "build.json").write_text(json.dumps({"command": command, "cwd": entry["directory"]}, indent=2) + "\n")
    print(json.dumps(report, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=root / "tools/rexglue-sdk/build")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.output_dir:
        run(root, args.sdk_build.resolve(), args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="simpsons-native-calls-") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
