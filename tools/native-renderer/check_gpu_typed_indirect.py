#!/usr/bin/env python3
"""Compare typed, masked and canonical constant descriptors with the real helper and register-locals variant.

Only its post-growth command writer is replaced; real save/restore and descriptor
control execute in the CPU fixture. The growth implementation is outside it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from check_gpu_constants import compiler_flags, extract

HOOK = ("\t// HAND PATCH: retain descriptor parsing, growth and the original loop.\n"
        "\tif (simpsons::native_gpu_calls::TryIndirectConstants(ctx, base)) goto loc_8245F8B0;\n")

def reference_source(root):
    sys.path.insert(0, str(root / "simpsons"))
    from register_locals import rewrite
    generated = root / "simpsons/generated/default"
    body = extract(generated / "simpsons_recomp.17.cpp", "sub_8245F810")
    if body.count(HOOK) != 1:
        raise RuntimeError("The indirect-constant checkpoint changed")
    old = body.replace(HOOK, "")
    hashes = {"sub_8245F810": hashlib.sha256(body.encode()).hexdigest(),
              "sub_8245F810_without_hook": hashlib.sha256(old.encode()).hexdigest()}
    pieces = []
    for name in ["__savegprlr_25", "__restgprlr_25"]:
        helper = extract(generated / "simpsons_recomp.65.cpp", name)
        hashes[name] = hashlib.sha256(helper.encode()).hexdigest()
        pieces.append(helper.replace(f"DEFINE_REX_FUNC({name})",
                                     f"void reference_{name.lstrip('_')}(PPCContext& ctx, uint8_t* base)"))
    for native, source in [(False, old), (True, body)]:
        for local in [False, True]:
            name = ("patched" if native else "reference") + ("_locals" if local else "")
            if local:
                outside = []
                source_variant = rewrite(source, name, outside)
                if outside:
                    raise RuntimeError("Register local escaped its function")
            else:
                source_variant = source
            source_variant = source_variant.replace("DEFINE_REX_FUNC(sub_8245F810)",
                                                    f"void {name}(PPCContext& ctx, uint8_t* base)")
            for helper in ["__savegprlr_25", "__restgprlr_25"]:
                source_variant = source_variant.replace(helper + "(ctx, base)",
                                                       "reference_" + helper.lstrip('_') + "(ctx, base)")
            pieces.append(source_variant)
    return "\n\n".join(pieces) + "\n", hashes

def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_indirect.h", "simpsons/src/native_gpu_calls.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "simpsons/register_locals.py",
        "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_indirect.h",
        "tools/bench/native_gpu_typed_indirect_check.cpp", "tools/native-renderer/check_gpu_typed_indirect.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-typed-indirect-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_typed_indirect_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_typed_indirect_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Indirect constants oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Indirect constants oracle did not pass")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root)[1] != hashes:
            raise RuntimeError("Reference segment changed during qualification")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-typed-indirect-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
