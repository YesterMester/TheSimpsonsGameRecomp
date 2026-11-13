#!/usr/bin/env python3
"""Compare native dirty Begin/fill/End with real generated flushes and allocators."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract
from check_gpu_special import reference_source as flush_reference


def reference_source(root, register_locals=False):
    source, origins, hashes = flush_reference(root)
    generated = root / "simpsons/generated/default"
    added = {n: "simpsons_recomp.16.cpp" for n in
             ("sub_8244C450", "sub_8244C970", "sub_8244C8F0", "sub_8244CEA0")}
    added.update({n: "simpsons_recomp.17.cpp" for n in
                  ("sub_82456D60", "sub_82456B58", "sub_824564D0", "sub_82458E60", "sub_824605C0")})
    added.update({f"__{kind}gprlr_{n}": "simpsons_recomp.65.cpp"
                  for kind in ("save", "rest") for n in (22, 29)})
    renames = {n: "reference_" + n.lstrip("_") for n in [*origins, *added]}
    pieces = [source] + [f"void {renames[n]}(PPCContext&, uint8_t*);" for n in added]
    for name, file in added.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        origins[name] = file
        if name == "sub_824605C0":
            compact = re.sub(r"\s+", "", re.sub(r"//[^\n]*", "", body[body.index("{") + 1:body.rindex("}")]))
            if compact != "REX_FUNC_PROLOGUE();return;":
                raise RuntimeError("The retained EndVertices tail gained behavior")
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name in ("sub_8244C450", "sub_8244C970", "sub_82456D60"):
            counter = "allocator_body_calls" if name == "sub_82456D60" else "reference_begin_calls"
            body = body.replace("REX_FUNC_PROLOGUE();", f"REX_FUNC_PROLOGUE();\n ++{counter};", 1)
        if name == "sub_82458E60":
            body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++recorded_allocator_calls;", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        body = re.sub(r"ppc_trap\(ctx, base, [^)]*\)",
                      'throw std::runtime_error("Reference guest trap")', body)
        pieces.append(body)
    for n in ("82459E28", "82451DE0", "82452018", "82451E10"):
        pieces.append(f'REX_FUNC(sub_{n}) {{ (void)ctx; (void)base; '
                      f'throw std::runtime_error("Unsupported reference allocation branch {n}"); }}')
    source = "\n\n".join(pieces) + "\n"
    if register_locals:
        import importlib.util
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = re.sub(r"^void (reference_\w+)\(PPCContext& ctx, uint8_t\* base\) \{",
                        r"DEFINE_REX_FUNC(\1) {", source, flags=re.M)
        outside = []
        source = module.rewrite(source, "begin_state_reference", outside)
        if outside:
            raise RuntimeError("Reference registers outside generated functions")
    return source, origins, hashes


def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_begin.h", "simpsons/src/native_gpu_vertices.h",
        "simpsons/src/native_gpu_recorded_allocation.h", "simpsons/register_locals.py",
        "simpsons/src/native_gpu_direct_draws.h", "tools/rexglue-sdk/include/rex/graphics/native_gpu_direct.h",
        "simpsons/src/native_gpu_indexed.h", "simpsons/src/native_gpu_flush.h",
        "simpsons/src/native_gpu_samplers.h", "simpsons/src/native_gpu_state.h",
        "simpsons/src/native_gpu_constants.h", "simpsons/src/native_gpu_calls.cpp",
        "simpsons/src/guest_memory.h", "simpsons/src/native_gpu_memory.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_constants.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_coherency.h",
        "tools/bench/guest_memory_fixture.h", "tools/bench/native_gpu_begin_state_check.cpp",
        "tools/native-renderer/check_gpu_begin_state.py", "tools/native-renderer/check_gpu_special.py",
        "tools/native-renderer/check_gpu_state.py")]


def run(root, build, output, register_locals=False):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, origins, hashes = reference_source(root, register_locals)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-begin-state-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_begin_state_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_begin_state_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Begin state oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Begin state oracle did not pass")
        for f, h in sources.items():
            if hashlib.sha256((root / f).read_bytes()).hexdigest() != h:
                raise RuntimeError("Source changed during qualification: " + f)
        if reference_source(root, register_locals)[2] != hashes:
            raise RuntimeError("Reference bodies changed during qualification")
        report.update(source_hashes=sources, reference_body_hashes=hashes,
                      register_locals=register_locals)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-build", type=Path, default=root / "tools/rexglue-sdk/build")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--register-locals", action="store_true")
    args = parser.parse_args()
    if args.output_dir:
        run(root, args.sdk_build.resolve(), args.output_dir.resolve(), args.register_locals)
    else:
        with tempfile.TemporaryDirectory(prefix="simpsons-begin-state-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
