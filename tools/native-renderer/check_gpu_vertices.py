#!/usr/bin/env python3
"""Compare prepared Begin/fill/End with the current generated code and real allocator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract


def reference_source(root, indexed=False):
    generated = root / "simpsons/generated/default"
    origins = {
        **{n: "simpsons_recomp.16.cpp" for n in ("sub_8244C450", "sub_8244C8F0")},
        **{n: "simpsons_recomp.17.cpp" for n in
           ("sub_82456D60", "sub_82456B58", "sub_824564D0", "sub_824605C0")},
        **{f"__{kind}gprlr_{n}": "simpsons_recomp.65.cpp"
           for kind in ("save", "rest") for n in (22, 29)}}
    if indexed:
        origins.update({n: "simpsons_recomp.16.cpp" for n in
                        ("sub_8244C970", "sub_8244CEA0", "sub_8244CF78", "sub_8244D360", "sub_82444A48")})
        origins["sub_8246AEF0"] = "simpsons_recomp.17.cpp"
        origins.update({f"__{kind}gprlr_{n}": "simpsons_recomp.65.cpp"
                        for kind in ("save", "rest") for n in (14, 18)})
    bodies = {n: extract(generated / p, n) for n, p in origins.items()}
    tail = bodies["sub_824605C0"]
    tail = tail[tail.index("{") + 1:tail.rindex("}")]
    if re.sub(r"\s+", "", re.sub(r"//[^\n]*", "", tail)) != "REX_FUNC_PROLOGUE();return;":
        raise RuntimeError("EndVertices' original tail gained behavior; requalify the replacement")
    hashes = {n: hashlib.sha256(b.encode()).hexdigest() for n, b in bodies.items()}
    renames = {n: "reference_" + n.lstrip("_") for n in bodies}
    pieces = [f"void {r}(PPCContext&, uint8_t*);" for r in renames.values()]
    for n, b in bodies.items():
        b = b.replace(f"DEFINE_REX_FUNC({n})", f"void {renames[n]}(PPCContext& ctx, uint8_t* base)")
        if n in ("sub_8244C450", "sub_8244C970", "sub_8244CF78", "sub_8244D360",
                 "sub_82444A48", "sub_8246AEF0", "sub_82456D60"):
            counter = "allocator_body_calls" if n == "sub_82456D60" else "reference_begin_calls"
            b = b.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++" + counter + ";", 1)
        for name, replacement in renames.items():
            b = re.sub(r"\b" + re.escape(name) + r"\(ctx, base\)", replacement + "(ctx, base)", b)
        # A guest trap must fail the standalone oracle, without importing the
        # game's logging/runtime solely for an unsupported reference branch.
        b = re.sub(r"ppc_trap\(ctx, base, [^)]*\)",
                   'throw std::runtime_error("Reference guest trap")', b)
        pieces.append(b)
    # These branches are outside the accepted preflight. Observing one fails
    # the reference execution rather than pretending to flush/allocate/wait.
    for n in ("8245EC68", "8245FA00", "8245E530", "8245E8C8", "8245EB08", "8245E988",
              "82457CC8", "82457748", "82458E60", "82459E28", "82451DE0", "82452018", "82451E10"):
        pieces.append(f'REX_FUNC(sub_{n}) {{ (void)ctx; (void)base; '
                      f'throw std::runtime_error("Unconverted reference branch {n}"); }}')
    return "\n\n".join(pieces) + "\n", origins, hashes


def run(root, build, output, indexed=False):
    output.mkdir(parents=True, exist_ok=True)
    reference, origins, hashes = reference_source(root, indexed)
    harness = "native_gpu_indexed_check.cpp" if indexed else "native_gpu_vertices_check.cpp"
    generated = root / "simpsons/generated/default"
    compiler, flags, cwd = compiler_flags(build)
    inputs = [root / p for p in (
        "simpsons/src/native_gpu_vertices.h", "simpsons/src/native_gpu_vertices.cpp",
        "simpsons/src/guest_memory.h", "simpsons/src/native_gpu_memory.h",
        "simpsons/src/native_gpu_recorded_allocation.h", "simpsons/src/native_gpu_direct_draws.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "simpsons/src/native_gpu_indexed.h",
        "simpsons/src/native_gpu_aux_draws.h", "simpsons/src/native_gpu_calls.cpp",
        "tools/native-renderer/check_gpu_vertices.py", "tools/bench/" + harness)]
    source_hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    with tempfile.TemporaryDirectory(prefix="simpsons-vertices-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_vertices_reference.inc").write_text(reference)
        common = [compiler, *flags, "-g0", "-I" + str(generated),
                  "-I" + str(root / "simpsons/src"), "-I" + str(ram)]
        command = [*common, str(root / "tools/bench" / harness),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        assert report.get("passed") is True
        for p in inputs:
            if hashlib.sha256(p.read_bytes()).hexdigest() != source_hashes[str(p.relative_to(root))]:
                raise RuntimeError(f"Source changed during qualification: {p}")
        for name, file in origins.items():
            if hashlib.sha256(extract(generated / file, name).encode()).hexdigest() != hashes[name]:
                raise RuntimeError(f"Reference changed during qualification: {name}")
        report.update(source_hashes=source_hashes, reference_body_hashes=hashes)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        print(json.dumps(report, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=root / "tools/rexglue-sdk/build")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--indexed", action="store_true",
                        help="Compare indexed Begin/End and prepared draw ranges as well")
    args = parser.parse_args()
    if args.output_dir:
        run(root, args.sdk_build.resolve(), args.output_dir.resolve(), args.indexed)
    else:
        with tempfile.TemporaryDirectory(prefix="simpsons-vertices-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.indexed)


if __name__ == "__main__":
    main()
