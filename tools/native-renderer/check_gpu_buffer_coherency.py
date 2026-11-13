#!/usr/bin/env python3
"""Compare both native buffer-range writers with their current generated bodies."""

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
    names = {name: 65 for name in ("__savegprlr_28", "__restgprlr_28",
                                   "__savegprlr_29", "__restgprlr_29")}
    names.update(sub_824415D0=16, sub_8244E208=16)
    renames = {name: "reference_" + name.lstrip("_") for name in names}
    if register_locals:
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    pieces = [f"void {renames[name]}(PPCContext&, uint8_t*);" for name in names]
    hashes = {}
    for name, number in names.items():
        body = extract(root / f"simpsons/generated/default/simpsons_recomp.{number}.cpp", name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference register rewrite escaped its function")
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        for old, new in renames.items():
            body = re.sub(r"\b" + old + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    unlock = extract(root / "simpsons/generated/default/simpsons_recomp.16.cpp", "sub_82441238")
    unlock = unlock.split("loc_82441334:\n", 1)[1].split("loc_824413A0:\n", 1)[0]
    checkpoint = "\tif (simpsons::native_gpu_calls::TryUnlockCoherency(ctx, base)) goto loc_824413A0;\n"
    if unlock.count(checkpoint) != 1:
        raise RuntimeError("Cannot identify the exact native unlock checkpoint")
    unlock = unlock.replace(checkpoint, "")
    hashes["sub_82441238_flush_original"] = hashlib.sha256(unlock.encode()).hexdigest()
    # The retained guard is deliberately a no-op in the generated module too.
    pieces.append("static bool HandPatchEnsureWritable16(void*, size_t) { return true; }")
    pieces.append("void reference_unlock_flush(PPCContext& ctx, uint8_t* base) {\n uint32_t ea;\n" + unlock + "}\n")
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    return [root / p for p in ("simpsons/src/native_gpu_buffer_coherency.h",
            "simpsons/src/native_gpu_state.cpp", "simpsons/src/native_gpu_memory.h",
            "simpsons/src/guest_memory.h", "tools/rexglue-sdk/include/rex/graphics/native_gpu_sync.h",
            "simpsons/src/native_gpu_calls.cpp",
            "tools/bench/native_gpu_buffer_coherency_check.cpp",
            "tools/native-renderer/check_gpu_buffer_coherency.py")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-buffer-coherency-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_buffer_coherency_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_buffer_coherency_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=120)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Buffer coherency comparison exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Buffer coherency comparison did not pass")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}:
            raise RuntimeError("Sources changed during qualification")
        if reference_source(root, register_locals)[1] != hashes:
            raise RuntimeError("Reference changed during qualification")
        report.update(register_locals=register_locals, source_hashes=sources, reference_body_hashes=hashes)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=root / "tools/rexglue-sdk/build")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--register-locals", action="store_true")
    args = parser.parse_args()
    run(root, args.sdk_build.resolve(), args.output_dir.resolve(), args.register_locals)


if __name__ == "__main__":
    main()
