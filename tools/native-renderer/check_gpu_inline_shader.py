#!/usr/bin/env python3
"""Compare complete inline shader setup against the original generated call chain."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract
from check_gpu_shader_setup import original_body


def reference_source(root, register_locals=False):
    generated = root / "simpsons/generated/default"
    origins = {name: "simpsons_recomp.17.cpp" for name in (
        "sub_8245F6B0", "sub_82457F50", "sub_8245DA50", "sub_8245F560",
        "sub_8245F3D8", "sub_8245F4A8", "sub_8245EDC8")}
    origins["sub_82A3CD80"] = "simpsons_recomp.65.cpp"
    for number in (19, 20, 22, 28, 29):
        for prefix in ("__savegprlr_", "__restgprlr_"):
            origins[prefix + str(number)] = "simpsons_recomp.65.cpp"
    renames = {name: "reference_" + name.lstrip("_") for name in origins}
    source = (generated / "simpsons_recomp.65.cpp").read_text()
    start = source.index("static bool HandPatchEnsureWritable65(")
    helper = source[start:source.index("\n}", start) + 2]
    hashes = {"HandPatchEnsureWritable65": hashlib.sha256(helper.encode()).hexdigest()}
    if register_locals:
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    # Growth is outside the ready-reserve contract. Never fabricate a successful
    # child call; executing either one makes the accepted-case oracle fail.
    growth = '\n'.join(
        'REX_FUNC(' + name + ') { (void)ctx; (void)base; throw std::runtime_error("Original shader reserve required growth"); }'
        for name in ("sub_82457CC8", "sub_824578C0"))
    pieces = [helper, growth] + [f"void {name}(PPCContext&, uint8_t*);" for name in renames.values()]
    counters = {"sub_8245F6B0": "body_calls", "sub_82457F50": "reserve_calls",
                "sub_8245F560": "link_calls", "sub_8245EDC8": "declaration_calls",
                "sub_82A3CD80": "copy_calls"}
    for name, file in origins.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if name == "sub_8245F6B0":
            body = original_body(body)
            if "TryShaderSetup" in body:
                raise RuntimeError("The original inline shader checkpoint was not removed")
            hashes[name + "_without_shader_hooks"] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference registers outside the generated function")
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name in counters:
            body = body.replace("REX_FUNC_PROLOGUE();",
                                "REX_FUNC_PROLOGUE();\n ++" + counters[name] + ";", 1)
        body = body.replace("REX_PPC_SYNC();", "REX_PPC_SYNC();\n ++sync_calls;")
        for old, new in renames.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    return [root / name for name in (
        "simpsons/src/native_gpu_inline_shader.h", "simpsons/src/native_gpu_shader_linkage.h",
        "simpsons/src/native_gpu_shader_resources.h", "simpsons/src/native_gpu_vertex_declaration.h",
        "simpsons/src/native_gpu_shader_setup.h", "simpsons/src/native_gpu_state.cpp",
        "simpsons/src/native_gpu_memory.h", "simpsons/src/guest_memory.h",
        "simpsons/generated/default/simpsons_init.h", "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/ppc/barriers.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_program.h",
        "tools/bench/native_gpu_inline_shader_check.cpp", "tools/native-renderer/check_gpu_inline_shader.py",
        "tools/native-renderer/check_gpu_shader_setup.py", "simpsons/register_locals.py")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-inline-shader-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_inline_shader_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_inline_shader_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Inline shader oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if report.get("passed") is not True:
            raise RuntimeError("Inline shader oracle did not pass")
        if sources != {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in paths} or reference_source(root, register_locals)[1] != hashes:
            raise RuntimeError("Source changed during qualification")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-inline-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
