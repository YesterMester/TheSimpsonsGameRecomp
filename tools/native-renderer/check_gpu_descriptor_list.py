#!/usr/bin/env python3
"""Compare native descriptor expansion and CPU cache barriers with complete original routines."""

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
    from check_gpu_chunk_close import reference_source as close_reference
    source, hashes = close_reference(root, register_locals)
    stub = 'REX_FUNC(sub_82457748) { (void)ctx; (void)base; ++unsupported_close_calls; throw std::runtime_error("Original close needs sub_82457748"); }'
    assert source.count(stub) == 1
    source = 'void reference_sub_82457748(PPCContext&, uint8_t*);\n' + source.replace(
        stub, 'REX_FUNC(sub_82457748) { reference_sub_82457748(ctx, base); }')
    names = ("sub_82457748", "sub_8245DA50")
    renames = {name: "reference_" + name for name in (*names, "sub_82456D60")}
    renames.update({name: "reference_" + name.lstrip("_") for name in ("__savegprlr_29", "__restgprlr_29")})
    if register_locals:
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    pieces = [source] + [f"void {renames[name]}(PPCContext&, uint8_t*);" for name in names]
    for name in names:
        body = extract(root / "simpsons/generated/default/simpsons_recomp.17.cpp", name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference register rewrite escaped its function")
        body = body.replace(f"DEFINE_REX_FUNC({name})", f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++" + ("descriptor_calls" if name == names[0] else "cache_calls") + ";", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + old + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    prefix = "#define NATIVE_GPU_REFERENCE_REGISTER_LOCALS 1\n" if register_locals else ""
    return prefix + "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    from check_gpu_chunk_close import inputs as close_inputs
    return close_inputs(root) + [root / p for p in (
        "simpsons/src/native_gpu_descriptor_list.h", "simpsons/src/native_gpu_cache_range.h",
        "tools/bench/native_gpu_descriptor_list_check.cpp", "tools/native-renderer/check_gpu_descriptor_list.py",
        "tools/rexglue-sdk/include/rex/ppc/barriers.h")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-descriptor-list-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_chunk_growth_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_descriptor_list_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Descriptor-list oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Descriptor-list oracle failed")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-descriptor-list-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
