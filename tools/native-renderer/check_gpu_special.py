#!/usr/bin/env python3
"""Compare native dirty range/vertex-ID calls with the generated game bodies."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract
from check_gpu_state import reference_source as state_reference


def reference_source(root):
    source, origins, hashes = state_reference(root)
    generated = root / "simpsons/generated/default"
    added = {n: "simpsons_recomp.16.cpp" for n in
             ("sub_8244CF78", "sub_8244D360", "sub_8244D7B8", "sub_8244DC30")}
    added["sub_8245EC68"] = "simpsons_recomp.17.cpp"
    added["sub_8245E988"] = "simpsons_recomp.17.cpp"
    added.update({f"__{kind}gprlr_{n}": "simpsons_recomp.65.cpp"
                  for kind in ("save", "rest") for n in (14, 17, 18, 21, 23)})
    renames = {n: "reference_" + n.lstrip("_") for n in [*origins, *added]}
    pieces = [source] + [f"void {renames[n]}(PPCContext&, uint8_t*);" for n in added]
    for name, file in added.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        origins[name] = file
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name.startswith("sub_"):
            counter = "reference_calls" if name in ("sub_8245EC68", "sub_8245E988") else "reference_draw_calls"
            body = body.replace("REX_FUNC_PROLOGUE();", f"REX_FUNC_PROLOGUE();\n ++{counter};", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        body = re.sub(r"ppc_trap\(ctx, base, [^)]*\)",
                      'throw std::runtime_error("Reference guest trap")', body)
        pieces.append(body)
    for n in ("8245FA00", "8245E530", "82457748"):
        pieces.append(f'REX_FUNC(sub_{n}) {{ (void)ctx; (void)base; '
                      f'throw std::runtime_error("Unsupported reference branch {n}"); }}')
    return "\n\n".join(pieces) + "\n", origins, hashes


def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_special.h", "simpsons/src/native_gpu_flush.h",
        "simpsons/src/native_gpu_samplers.h", "simpsons/src/native_gpu_state.cpp",
        "simpsons/src/native_gpu_indexed.h", "simpsons/src/native_gpu_vertices.h",
        "simpsons/src/native_gpu_state.h", "simpsons/src/native_gpu_constants.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "tools/bench/guest_memory_fixture.h",
        "simpsons/src/native_gpu_recorded_allocation.h", "simpsons/src/native_gpu_direct_draws.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_constants.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_coherency.h",
        "tools/bench/native_gpu_special_check.cpp", "tools/native-renderer/check_gpu_special.py",
        "tools/native-renderer/check_gpu_state.py")]


def run(root, build, output, samplers=False):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, origins, hashes = reference_source(root)
    paths = inputs(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-special-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_special_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_special_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        if samplers:
            command.append("-DNATIVE_GPU_SAMPLERS=1")
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"special oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Special draw oracle did not pass")
        for p in paths:
            if hashlib.sha256(p.read_bytes()).hexdigest() != sources[str(p.relative_to(root))]:
                raise RuntimeError(f"Source changed during qualification: {p}")
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
    parser.add_argument("--samplers", action="store_true",
                        help="Include standalone samplers and their dirty-range integration")
    args = parser.parse_args()
    if args.output_dir:
        run(root, args.sdk_build.resolve(), args.output_dir.resolve(), args.samplers)
    else:
        with tempfile.TemporaryDirectory(prefix="simpsons-special-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.samplers)


if __name__ == "__main__":
    main()
