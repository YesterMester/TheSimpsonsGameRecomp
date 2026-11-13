#!/usr/bin/env python3
"""Compare prepared resolve draw writers and their original split-loop control.

The allocator, geometry construction, shader/resource setup and ring-growth
implementation remain outside these replacements and this CPU-only fixture.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from check_gpu_constants import compiler_flags, extract

CLEAR_HOOK = ("\t// HAND PATCH: native command construction after the real clear geometry.\n"
              "\tif (simpsons::native_gpu_calls::TryClearResolveDraw(ctx, base)) goto loc_native_824561A4;\n")
COPY_HOOK = ("\t// HAND PATCH: retain the original split loop and ring-growth steps.\n"
             "\tif (simpsons::native_gpu_calls::TryCopyResolveDraw(ctx, base)) goto loc_native_8246CD94;\n")
COPY_LABEL = ("loc_native_8246CD94:\n"
              "\t// Refresh the branch field here as well for register-locals builds.\n"
              "\tctx.cr0.compare<int32_t>(ctx.r29.s32, 0, ctx.xer);\n")


def original_body(body, copy):
    hook, label = (COPY_HOOK, COPY_LABEL) if copy else (CLEAR_HOOK, "loc_native_824561A4:\n")
    if body.count(hook) != 1 or body.count(label) != 1:
        raise RuntimeError("The resolve construction checkpoint changed")
    return body.replace(hook, "").replace(label, "")


def reference_source(root):
    sys.path.insert(0, str(root / "simpsons"))
    from register_locals import rewrite
    file = root / "simpsons/generated/default/simpsons_recomp.17.cpp"
    pieces, hashes = [], {}
    for name, copy in (("sub_82455570", False), ("sub_8246C750", True)):
        body = extract(file, name)
        old = original_body(body, copy)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        hashes[name + "_without_hook"] = hashlib.sha256(old.encode()).hexdigest()
        for native, content in ((False, old), (True, body)):
            if copy:
                first, last = content.index("loc_8246CD40:\n"), content.index("loc_8246CDA8:\n")
                segment = content[first:last] + "loc_8246CDA8:\nreturn;\n"
            else:
                first = content.index(CLEAR_HOOK if native else "\t// stwu r10,4(r30)\n",
                                      content.index("loc_82456070:\n"))
                last = content.index("\t// lwz r10,56(r31)\n", first)
                segment = content[first:last]
            for local in (False, True):
                label = ("patched" if native else "reference") + ("_copy" if copy else "_clear")
                label += "_locals" if local else ""
                source = (f"DEFINE_REX_FUNC({label}) {{\n\tREX_FUNC_PROLOGUE();\n"
                          "\tuint32_t ea{};\n" + segment + "}\n")
                if local:
                    outside = []
                    source = rewrite(source, label, outside)
                    if outside:
                        raise RuntimeError("Register-locals reference escaped its function")
                source = source.replace(f"DEFINE_REX_FUNC({label})",
                                        f"void {label}(PPCContext& ctx, uint8_t* base)")
                pieces.append(source)
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_resolve_draws.h", "simpsons/src/native_gpu_calls.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "simpsons/register_locals.py",
        "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/bench/native_gpu_resolve_draws_check.cpp",
        "tools/native-renderer/check_gpu_resolve_draws.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-resolve-draws-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_resolve_draws_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_resolve_draws_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Resolve-draw oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Resolve-draw oracle did not pass")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root)[1] != hashes:
            raise RuntimeError("Reference segments changed during qualification")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-resolve-draw-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
