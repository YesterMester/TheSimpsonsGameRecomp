#!/usr/bin/env python3
"""Compare whole shader constant/depth helpers against their generated routines."""

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


def reference_source(root, register_locals=False):
    generated = root / "simpsons/generated/default"
    origins = {"sub_8245E330": "simpsons_recomp.17.cpp",
               "sub_8245F810": "simpsons_recomp.17.cpp",
               "__savegprlr_25": "simpsons_recomp.65.cpp",
               "__restgprlr_25": "simpsons_recomp.65.cpp"}
    renames = {name: "reference_" + name.lstrip("_") for name in origins}
    hashes = {}
    if register_locals:
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    pieces = ['REX_FUNC(sub_82457CC8) { (void)ctx; (void)base; throw std::runtime_error("Original shader helper required ring growth"); }']
    pieces += [f"void {name}(PPCContext&, uint8_t*);" for name in renames.values()]
    hook = ("\t// HAND PATCH: retain descriptor parsing, growth and the original loop.\n"
            "\tif (simpsons::native_gpu_calls::TryIndirectConstants(ctx, base)) goto loc_8245F8B0;\n")
    for name, file in origins.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if name == "sub_8245F810":
            if body.count(hook) != 1:
                raise RuntimeError("The shader descriptor checkpoint changed")
            body = body.replace(hook, "")
            hashes[name + "_without_hooks"] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference registers outside the generated function")
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name in ("sub_8245E330", "sub_8245F810"):
            counter = "control_calls" if name == "sub_8245E330" else "constant_calls"
            body = body.replace("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n ++" + counter + ";", 1)
        if name == "sub_8245F810":
            publication = "REX_STORE_U32(ctx.r29.u32 + 48, ctx.r3.u32);"
            if body.count(publication) != 1:
                raise RuntimeError("The shader descriptor publication changed")
            body = body.replace(publication, publication + "\n ++constant_records;")
        for old, new in renames.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    return [root / name for name in (
        "simpsons/src/native_gpu_shader_constants.h", "simpsons/src/native_gpu_shader_control.h",
        "simpsons/src/native_gpu_state.cpp", "simpsons/src/native_gpu_memory.h",
        "simpsons/src/guest_memory.h", "simpsons/generated/default/simpsons_init.h",
        "tools/bench/guest_memory_fixture.h", "tools/bench/native_gpu_shader_coordination_check.cpp",
        "tools/native-renderer/check_gpu_shader_coordination.py", "simpsons/register_locals.py",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_indirect.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_ports.h",
        "tools/rexglue-sdk/src/graphics/command_processor.cpp",
        "tools/rexglue-sdk/src/graphics/vulkan/command_processor.cpp",
        "tools/rexglue-sdk/src/graphics/d3d12/command_processor.cpp",
        "tools/rexglue-sdk/src/graphics/d3d11/command_processor.cpp")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-shader-coordination-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_shader_coordination_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_shader_coordination_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Shader coordination oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if report.get("passed") is not True:
            raise RuntimeError("Shader coordination oracle did not pass")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-coordination-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
