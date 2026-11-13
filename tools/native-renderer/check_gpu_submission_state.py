#!/usr/bin/env python3
"""Compare native submission cache-flush construction with complete original routines."""

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
    source = source.replace("RecordGuard(original_guards, original_base, host_addr, len);",
                            "RecordGuard(original_guards, original_base, host_addr, len); InjectRange(original_base);")
    generated = root / "simpsons/generated/default"
    origins = {name: "simpsons_recomp.17.cpp" for name in ("sub_82457050", "sub_824567D8", "sub_82456768")}
    origins.update({name: "simpsons_recomp.65.cpp" for name in ("__savegprlr_20", "__restgprlr_20")})
    renames = {name: "reference_" + name.lstrip("_") for name in (*origins, "sub_82456D60")}
    pieces = [source] + [f"void {renames[name]}(PPCContext&, uint8_t*);" for name in origins]
    if register_locals:
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    for name, file in origins.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference register rewrite escaped its function")
        if name == "sub_824567D8":
            # The register-locals pass keeps the reservation in a local, while
            # context mode uses ctx.reserved. Observe the real captured word in
            # both forms without changing its control flow or guest effects.
            value = "reserved.u64" if register_locals else "ctx.reserved.u64"
            assert body.count("return;") == 1
            body = body.replace("return;", "reference_reservation = " + value + "; return;")
        body = body.replace(f"DEFINE_REX_FUNC({name})", f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        counter = {"sub_82457050": "submission_calls", "sub_824567D8": "range_drain_calls", "sub_82456768": "range_merge_calls"}.get(name)
        if counter:
            body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++" + counter + ";", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + old + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    from check_gpu_chunk_close import inputs as close_inputs
    return close_inputs(root) + [root / p for p in (
        "simpsons/src/native_gpu_submission_state.h", "simpsons/src/native_gpu_recorded_view.h",
        "tools/bench/native_gpu_submission_state_check.cpp", "tools/native-renderer/check_gpu_submission_state.py",
        "tools/rexglue-sdk/src/core/mutex.cpp")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-submission-state-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_chunk_growth_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_submission_state_check.cpp"),
                   str(root / "tools/rexglue-sdk/src/core/mutex.cpp"), "-pthread",
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Submission-state oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Submission-state oracle failed")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-submission-state-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
