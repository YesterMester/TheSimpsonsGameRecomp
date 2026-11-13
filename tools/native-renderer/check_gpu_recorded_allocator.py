#!/usr/bin/env python3
"""Compare fixed-recording allocation with the complete generated routines."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract


def reference_source(root, register_locals=False):
    source = root / "simpsons/generated/default/simpsons_recomp.17.cpp"
    names = ("sub_82458E60", "sub_82456D60")
    renames = {name: "reference_" + name for name in names}
    pieces = [f"void {name}(PPCContext&, uint8_t*);" for name in renames.values()]
    hashes = {}
    if register_locals:
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    for name in names:
        body = extract(source, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if "simpsons::" in body or "HAND PATCH" in body:
            raise RuntimeError("Allocator reference gained a checkpoint; inspect it before extracting")
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference register rewrite escaped its function")
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        counter = "storage_calls" if name == names[0] else "dispatch_calls"
        body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++" + counter + ";", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + old + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    # These real branches are deliberately outside fixed-storage ownership.
    # Reaching one fails the oracle instead of fabricating an allocation.
    for name in ("sub_82456B58", "sub_82459E28"):
        pieces.append(f'REX_FUNC({name}) {{ (void)ctx; (void)base; '
                      f'throw std::runtime_error("Reference reached non-fixed allocator {name}"); }}')
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_recorded_allocator.h", "simpsons/src/native_gpu_vertices.cpp",
        "simpsons/src/native_gpu_memory.h", "simpsons/src/guest_memory.h",
        "simpsons/generated/default/simpsons_init.h", "simpsons/register_locals.py",
        "tools/bench/guest_memory_fixture.h", "tools/bench/native_gpu_recorded_allocator_check.cpp",
        "tools/native-renderer/check_gpu_recorded_allocator.py")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-recorded-allocator-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_recorded_allocator_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_recorded_allocator_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Allocator oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Allocator oracle failed")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root, register_locals)[1] != hashes:
            raise RuntimeError("Reference changed during qualification")
        report.update(source_hashes=sources, reference_body_hashes=hashes, register_locals=register_locals)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=root / "tools/rexglue-sdk/build")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--register-locals", action="store_true")
    args = parser.parse_args()
    if args.output_dir:
        run(root, args.sdk_build.resolve(), args.output_dir.resolve(), args.register_locals)
    else:
        with tempfile.TemporaryDirectory(prefix="simpsons-recorded-allocator-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
