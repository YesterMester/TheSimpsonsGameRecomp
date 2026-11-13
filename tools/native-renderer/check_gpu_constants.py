#!/usr/bin/env python3
"""Check the bounded constant writer against the current generated XDK body.

All generated source, compiler objects and executables stay in RAM. Reports
describe the CPU writer and replay ABI only; no game or GPU backend is launched.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
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
        raise RuntimeError(f"Cannot bound reference body {name}")
    return body


def compiler_flags(build):
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(e for e in entries if e["file"].endswith("/src/graphics/graphics_system.cpp"))
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    compiler, flags = arguments[0], []
    i = 1
    while i < len(arguments):
        if arguments[i] in ("-o", "-c", "-MF", "-MT", "-MQ"):
            i += 2
        elif arguments[i] in ("-MD", "-MMD", "-g"):
            i += 1
        else:
            flags.append(arguments[i])
            i += 1
    return compiler, flags, entry["directory"]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    inputs = [root / p for p in (
        "simpsons/src/native_gpu_constants.h", "simpsons/src/native_gpu_constants.cpp",
        "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_constants.h",
        "tools/bench/native_gpu_constants_check.cpp", "tools/native-renderer/check_gpu_constants.py")]
    hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    generated = root / "simpsons/generated/default"
    names = ["__savegprlr_21", "__restgprlr_21", "sub_8245EC68"]
    bodies = {name: extract(generated / ("simpsons_recomp.17.cpp" if name.startswith("sub_")
                                        else "simpsons_recomp.65.cpp"), name) for name in names}
    reference_hashes = {name: hashlib.sha256(body.encode()).hexdigest() for name, body in bodies.items()}
    renames = {name: "reference_" + name.lstrip("_") for name in names}
    pieces = [f"void {renames[name]}(PPCContext&, uint8_t*);" for name in names]
    for name, body in bodies.items():
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name == "sub_8245EC68":
            body = body.replace("REX_FUNC_PROLOGUE();",
                                "REX_FUNC_PROLOGUE();\n  ++reference_writer_calls;", 1)
        for original, replacement in renames.items():
            body = re.sub(r"\b" + re.escape(original) + r"\(ctx, base\)",
                          replacement + "(ctx, base)", body)
        pieces.append(body)

    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-native-constants-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_constants_reference.inc").write_text("\n\n".join(pieces) + "\n")
        common = [compiler, *flags, "-O2", "-g0", "-std=c++23",
                  "-I" + str(generated), "-I" + str(root / "simpsons/src"), "-I" + str(ram)]
        executable = ram / "native_gpu_constants_check"
        command = [*common, str(root / "tools/bench/native_gpu_constants_check.cpp"),
                   "-o", str(executable)]
        linker = root / "tools/clang20/bin/ld.lld"
        if linker.exists():
            command.append("--ld-path=" + str(linker))
        if Path("/lib64/ld-linux-x86-64.so.2").exists():
            command.append("-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2")
        wrapper_command = [*common, "-c", str(root / "simpsons/src/native_gpu_constants.cpp"),
                           "-o", str(ram / "native_gpu_constants.o")]
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run(wrapper_command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"Constant oracle failed: {result.stderr.strip()}")
        report = json.loads(result.stdout)
        if report.get("passed") is not True:
            raise RuntimeError("The generated-body oracle did not pass")
        for path in inputs:
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[str(path.relative_to(root))]:
                raise RuntimeError(f"Source changed during qualification: {path}")
        for name in names:
            path = generated / ("simpsons_recomp.17.cpp" if name.startswith("sub_")
                                else "simpsons_recomp.65.cpp")
            if hashlib.sha256(extract(path, name).encode()).hexdigest() != reference_hashes[name]:
                raise RuntimeError(f"Reference body changed during qualification: {name}")
        report.update(reference_body_hashes=reference_hashes, source_hashes=hashes,
                      wrapper_object_compiled=True, build_storage="RAM temporary directory")
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (output / "build.json").write_text(json.dumps(
            {"command": command, "wrapper_command": wrapper_command, "cwd": cwd}, indent=2) + "\n")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-constant-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
