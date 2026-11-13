#!/usr/bin/env python3
"""Compare the native utility rectangle writer with generated code and its allocator."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract
from check_gpu_vertices import reference_source as vertices_reference


def reference_source(root):
    source, origins, hashes = vertices_reference(root)
    name = "sub_824529C0"
    body = extract(root / "simpsons/generated/default/simpsons_recomp.16.cpp", name)
    hashes[name] = hashlib.sha256(body.encode()).hexdigest()
    origins[name] = "simpsons_recomp.16.cpp"
    body = body.replace(f"DEFINE_REX_FUNC({name})", "void reference_sub_824529C0(PPCContext& ctx, uint8_t* base)")
    body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++reference_begin_calls;", 1)
    for n in ("sub_82456D60", "__savegprlr_22", "__restgprlr_22"):
        body = re.sub(r"\b" + re.escape(n) + r"\(ctx, base\)", "reference_" + n.lstrip("_") + "(ctx, base)", body)
    return source + "\n" + body + "\n", origins, hashes


def run(root, build, output):
    output.mkdir(parents=True, exist_ok=True)
    reference, origins, hashes = reference_source(root)
    paths = [root / p for p in (
        "simpsons/src/native_gpu_clear.h", "simpsons/src/native_gpu_calls.cpp",
        "simpsons/src/native_gpu_vertices.h", "simpsons/src/native_gpu_memory.h",
        "simpsons/src/native_gpu_recorded_allocation.h", "simpsons/src/native_gpu_direct_draws.h",
        "simpsons/src/guest_memory.h", "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "tools/bench/native_gpu_clear_check.cpp", "tools/native-renderer/check_gpu_clear.py")]
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-clear-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_clear_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_clear_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"clear oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Clear oracle did not pass")
        for p in paths:
            if hashlib.sha256(p.read_bytes()).hexdigest() != sources[str(p.relative_to(root))]:
                raise RuntimeError(f"Source changed during qualification: {p}")
        if reference_source(root)[2] != hashes:
            raise RuntimeError("Reference changed during qualification")
        report.update(source_hashes=sources, reference_body_hashes=hashes)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-clear-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
