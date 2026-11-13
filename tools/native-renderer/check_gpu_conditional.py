#!/usr/bin/env python3
"""Compare native conditional selected-state commands with the real generated builder/helpers."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract


def reference_source(root):
    generated = root / "simpsons/generated/default"
    origins = {"sub_8245E530": "simpsons_recomp.17.cpp"}
    origins.update({name: "simpsons_recomp.65.cpp" for name in
                    ("__savegprlr_20", "__restgprlr_20")})
    renames = {name: "reference_" + name.lstrip("_") for name in origins}
    hashes = {}
    pieces = [f"void {n}(PPCContext&, uint8_t*);" for n in renames.values()]
    for name, file in origins.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        counter = {"sub_8245E530": "reference_calls"}.get(name)
        if counter:
            body = body.replace("REX_FUNC_PROLOGUE();", f"REX_FUNC_PROLOGUE();\n ++{counter};", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    pieces.append('REX_FUNC(sub_82457CC8) { (void)ctx; (void)base; '
                  'throw std::runtime_error("Reference entered unsupported command-buffer growth"); }')
    return "\n\n".join(pieces) + "\n", origins, hashes


def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_conditional.h", "simpsons/src/native_gpu_calls.cpp", "simpsons/src/native_gpu_state.cpp",
        "simpsons/src/native_gpu_memory.h", "simpsons/src/guest_memory.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_coherency.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_predication.h",
        "tools/bench/guest_memory_fixture.h", "tools/bench/native_gpu_conditional_check.cpp",
        "tools/native-renderer/check_gpu_conditional.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, origins, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-conditional-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_conditional_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_conditional_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Conditional oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Conditional oracle did not pass")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-conditional-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
