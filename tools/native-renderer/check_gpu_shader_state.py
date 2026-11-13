#!/usr/bin/env python3
"""Compare conditional shader/state batches and pixel binding with the actual game code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from check_gpu_constants import compiler_flags, extract

HOOKS = [
    ("\t// HAND PATCH: native pixel reference after the original declaration fixups.\n"
     "\tif (simpsons::native_gpu_calls::TryShaderState(0, ctx, base)) goto loc_native_8245FBF4;\n"),
    ("\t// HAND PATCH: native conditional shader/state batch; keep growth and publication.\n"
     "\tif (simpsons::native_gpu_calls::TryShaderState(1, ctx, base)) goto loc_82460220;\n")]

def original_body(body):
    for hook in HOOKS:
        if body.count(hook) != 1:
            raise RuntimeError("Shader-state checkpoint changed")
        body = body.replace(hook, "")
    if body.count("loc_native_8245FBF4:\n") != 1:
        raise RuntimeError("Pixel reference boundary changed")
    return body.replace("loc_native_8245FBF4:\n", "")

def reference_source(root):
    body = extract(root / "simpsons/generated/default/simpsons_recomp.17.cpp", "sub_8245FA00")
    old = original_body(body)
    hashes = {"sub_8245FA00": hashlib.sha256(body.encode()).hexdigest(),
              "sub_8245FA00_without_shader_state_hooks": hashlib.sha256(old.encode()).hexdigest()}
    pieces = []
    for native, source in [(False, old), (True, body)]:
        for kind in [0, 1]:
            first = source.index("loc_8245FBA0:\n" if kind == 0 else "loc_8245FF08:\n")
            if kind == 0:
                last = source.index("\t// lwz r10,10580(r30)\n", first)
                segment = source[first:last]
            else:
                last = source.index("loc_824601A0:\n", first)
                segment = source[first:last] + "loc_82460220:\n" + source[source.index("\t// stw r11,48(r30)\n", source.index("loc_82460220:\n")):source.index("loc_82460224:\n")]
            name = ("patched_" if native else "reference_") + str(kind)
            pieces.append(f"void {name}(PPCContext& ctx, uint8_t* base) {{\nREX_FUNC_PROLOGUE();\nuint32_t ea{{}};\n" + segment + "\n}\n")
    return "\n\n".join(pieces) + "\n", hashes

def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_shader_state.h", "simpsons/src/native_gpu_calls.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_shader_reference.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_predication.h",
        "tools/bench/native_gpu_shader_state_check.cpp", "tools/native-renderer/check_gpu_shader_state.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-shader-state-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_shader_state_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_shader_state_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Shader state oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Shader state oracle did not pass")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-shader-state-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
