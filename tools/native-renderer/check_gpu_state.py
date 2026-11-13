#!/usr/bin/env python3
"""Compare native state/fetch writers with the current generated game code."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract


def reference_source(root):
    generated = root / "simpsons/generated/default"
    origins = {n: "simpsons_recomp.17.cpp" for n in ("sub_8245E8C8", "sub_8245EB08")}
    origins.update({n: "simpsons_recomp.65.cpp" for n in ("__savegprlr_26", "__restgprlr_26")})
    bodies = {n: extract(generated / p, n) for n, p in origins.items()}
    source = (generated / "simpsons_recomp.17.cpp").read_text()
    helper = re.search(r"static bool HandPatchEnsureWritable\([^\n]+\) \{[^}]+\}", source)
    if not helper:
        raise RuntimeError("Cannot find the state writer's page-protection helper")
    helper = helper.group(0)
    body = helper[helper.index("{") + 1:helper.rindex("}")]
    compact = re.sub(r"\s+", "", re.sub(r"//[^\n]*", "", body))
    if compact != "(void)host_addr;(void)len;returntrue;":
        raise RuntimeError("The page-protection helper changed; requalify native state writes")
    hashes = {n: hashlib.sha256(b.encode()).hexdigest() for n, b in bodies.items()}
    hashes["HandPatchEnsureWritable"] = hashlib.sha256(helper.encode()).hexdigest()
    renamed = {n: "reference_" + n.lstrip("_") for n in bodies}
    pieces = [helper] + [f"void {n}(PPCContext&, uint8_t*);" for n in renamed.values()]
    for name, body in bodies.items():
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renamed[name]}(PPCContext& ctx, uint8_t* base)")
        if name.startswith("sub_"):
            body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++reference_calls;", 1)
        for old, new in renamed.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    for n in ("8245E278", "82457CC8"):
        pieces.append(f'REX_FUNC(sub_{n}) {{ (void)ctx; (void)base; '
                      f'throw std::runtime_error("Unsupported reference slow path {n}"); }}')
    return "\n\n".join(pieces) + "\n", origins, hashes


def run(root, build, output):
    output.mkdir(parents=True, exist_ok=True)
    reference, origins, hashes = reference_source(root)
    paths = [root / p for p in (
        "simpsons/src/native_gpu_state.h", "simpsons/src/native_gpu_state.cpp",
        "simpsons/src/guest_memory.h", "simpsons/src/native_gpu_memory.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/bench/native_gpu_state_check.cpp", "tools/native-renderer/check_gpu_state.py")]
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-state-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_state_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_state_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"state oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("State oracle did not pass")
        for p in paths:
            if hashlib.sha256(p.read_bytes()).hexdigest() != sources[str(p.relative_to(root))]:
                raise RuntimeError(f"Source changed during qualification: {p}")
        if reference_source(root)[2] != hashes:
            raise RuntimeError("Reference bodies changed during qualification")
        report.update(source_hashes=sources, reference_body_hashes=hashes)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-state-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
