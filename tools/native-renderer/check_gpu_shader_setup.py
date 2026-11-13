#!/usr/bin/env python3
"""Compare inline shader setup plus the real memcpy, without launching the game."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from check_gpu_constants import compiler_flags, extract
from check_gpu_initialization import reference_source as shared_reference

HOOKS = [
    ("\t// HAND PATCH: native inline shader metadata; retain the original copy and fixups.\n"
     "\tif (simpsons::native_gpu_calls::TryShaderSetup(0, ctx, base)) goto loc_native_resource_shader;\n", "loc_native_resource_shader:\n"),
    ("\t// HAND PATCH: native VS metadata after the real reservation.\n"
     "\tif (simpsons::native_gpu_calls::TryShaderSetup(1, ctx, base)) goto loc_native_copy_vs;\n", "loc_native_copy_vs:\n"),
    ("\t// HAND PATCH: native PS metadata; preserve its inline code and operand offsets.\n"
     "\tif (simpsons::native_gpu_calls::TryShaderSetup(2, ctx, base)) goto loc_native_copy_ps;\n", "loc_native_copy_ps:\n")]

def original_body(body):
    for hook, label in HOOKS:
        if hook in body:
            if body.count(hook) != 1 or body.count(label) != 1:
                raise RuntimeError("Shader setup checkpoint changed")
            body = body.replace(hook, "").replace(label, "")
    return body

def reference_source(root):
    shared, _, hashes = shared_reference(root)
    file = root / "simpsons/generated/default/simpsons_recomp.17.cpp"
    pieces = [shared]
    for kind, function in enumerate(["sub_8245F6B0", "sub_8246C750", "sub_8246C750"]):
        body = extract(file, function)
        old = original_body(body)
        hashes[function] = hashlib.sha256(body.encode()).hexdigest()
        hashes[function + "_without_shader_hooks"] = hashlib.sha256(old.encode()).hexdigest()
        for native, source in [(False, old), (True, body)]:
            if native:
                first = source.index(HOOKS[kind][0])
            elif kind == 0:
                first = source.index("\t// lis r11,-16384\n", source.index("goto loc_8245F808;"))
            elif kind == 1:
                first = source.index("\t// lis r11,-16384\n", source.index("\tsub_82457F50(ctx, base);"))
            else:
                first = source.index("\t// lis r11,-16368\n", source.index("loc_8246C9D0:\n"))
            boundary = "\tsub_82A3CD80(ctx, base);"
            last = source.index(boundary, first) + len(boundary)
            segment = source[first:last].replace("sub_82A3CD80(ctx, base)", "reference_sub_82A3CD80(ctx, base)")
            name = ("patched_" if native else "reference_") + str(kind)
            pieces.append(f"void {name}(PPCContext& ctx, uint8_t* base) {{\nREX_FUNC_PROLOGUE();\nuint32_t ea{{}};\n" + segment + "\n}\n")
    return "\n\n".join(pieces) + "\n", hashes

def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_shader_setup.h", "simpsons/src/native_gpu_calls.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_program.h",
        "tools/bench/native_gpu_shader_setup_check.cpp", "tools/native-renderer/check_gpu_shader_setup.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-shader-setup-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_shader_setup_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_shader_setup_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Shader setup oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Shader setup oracle did not pass")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-shader-setup-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
