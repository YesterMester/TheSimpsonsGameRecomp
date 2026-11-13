#!/usr/bin/env python3
"""Compare primary shader-reference construction, version write and publication."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from check_gpu_constants import compiler_flags, extract

HOOK = ("\t// HAND PATCH: native primary shader reference; retain fixups and publication.\n"
        "\tif (simpsons::native_gpu_calls::TryPrimaryShaderReference(ctx, base)) goto loc_82460220;\n")

def reference_source(root):
    body = extract(root / "simpsons/generated/default/simpsons_recomp.17.cpp", "sub_8245FA00")
    if body.count(HOOK) != 1:
        raise RuntimeError("Primary shader-reference checkpoint changed")
    old = body.replace(HOOK, "")
    hashes = {"sub_8245FA00": hashlib.sha256(body.encode()).hexdigest(),
              "sub_8245FA00_without_hook": hashlib.sha256(old.encode()).hexdigest()}
    pieces = []
    for native, source in [(False, old), (True, body)]:
        first = source.index("loc_824601C4:\n")
        last = source.index("loc_82460224:\n", first)
        name = "patched" if native else "reference"
        pieces.append(f"void {name}(PPCContext& ctx, uint8_t* base) {{\nREX_FUNC_PROLOGUE();\nuint32_t ea{{}};\n" + source[first:last] + "\n}\n")
    return "\n\n".join(pieces) + "\n", hashes

def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_shader_reference.h", "simpsons/src/native_gpu_calls.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_shader_reference.h",
        "tools/bench/native_gpu_shader_reference_check.cpp", "tools/native-renderer/check_gpu_shader_reference.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-shader-reference-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_shader_reference_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_shader_reference_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Shader reference oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Shader reference oracle did not pass")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-shader-reference-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
