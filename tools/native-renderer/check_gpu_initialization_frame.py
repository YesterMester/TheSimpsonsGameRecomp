#!/usr/bin/env python3
"""Compare initialization around its real heap-call boundary without launching a game.

The prefix stops at the allocator call. Continuations receive identical supplied
results; neither test executes or simulates the heap allocator itself.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract
from check_gpu_initialization import reference_source as leaf_reference, inputs as leaf_inputs


def reference_source(root):
    leaves, origins, hashes = leaf_reference(root)
    generated = root / "simpsons/generated/default"
    names = ("__savegprlr_28", "__restgprlr_28")
    for name in names:
        body = extract(generated / "simpsons_recomp.65.cpp", name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        origins[name] = "simpsons_recomp.65.cpp"
        leaves += body.replace(f"DEFINE_REX_FUNC({name})",
                               f"void reference_{name.lstrip('_')}(PPCContext& ctx, uint8_t* base)")
        leaves += "\n"
    body = extract(generated / "simpsons_recomp.16.cpp", "sub_82444A80")
    hashes["sub_82444A80"] = hashlib.sha256(body.encode()).hexdigest()
    origins["sub_82444A80"] = "simpsons_recomp.16.cpp"
    boundary = "\tsub_8238E880(ctx, base);"
    if body.count(boundary) != 1:
        raise RuntimeError("The original allocation boundary changed")
    prefix, continuation = body.split(boundary)
    prefix = prefix.replace("DEFINE_REX_FUNC(sub_82444A80)",
                            "void reference_prefix(PPCContext& ctx, uint8_t* base)")
    prefix += "\tallocation_boundary(ctx, base);\n}\n"
    continuation = ("void reference_continuation(PPCContext& ctx, uint8_t* base) {\n"
                    "REX_FUNC_PROLOGUE();\nuint32_t ea{};\n" + continuation)
    for name in (*names, "sub_82444700", "sub_824449E8"):
        replacement = "reference_" + name.lstrip("_")
        prefix = re.sub(r"\b" + re.escape(name) + r"\(ctx, base\)",
                        replacement + "(ctx, base)", prefix)
        continuation = re.sub(r"\b" + re.escape(name) + r"\(ctx, base\)",
                              replacement + "(ctx, base)", continuation)
    return leaves + "\n" + prefix + "\n" + continuation, origins, hashes


def inputs(root):
    return list(dict.fromkeys([*leaf_inputs(root), *[root / p for p in (
        "simpsons/src/native_gpu_initialization_frame.h", "simpsons/src/native_gpu_aux_draws.h",
        "tools/bench/native_gpu_initialization_frame_check.cpp",
        "tools/native-renderer/check_gpu_initialization_frame.py")]]))


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, _, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-initialization-frame-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_initialization_frame_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_initialization_frame_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Initialization frame oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Initialization frame oracle did not pass")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root)[2] != hashes:
            raise RuntimeError("Reference bodies changed during qualification")
        report.update(source_hashes=sources, reference_body_hashes=hashes)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-initialization-frame-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
