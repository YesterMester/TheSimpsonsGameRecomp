#!/usr/bin/env python3
"""Compare resource binding and initialization against the complete generated routines."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract


def reference_source(root, register_locals=False):
    generated = root / "simpsons/generated/default"
    origins = {name: "simpsons_recomp.16.cpp" for name in
               ("sub_82445278", "sub_82445578", "sub_824408E0", "sub_82445798",
                "sub_82445240", "sub_82445480", "sub_824457F0")}
    origins.update({name: "simpsons_recomp.65.cpp" for name in
                    ("sub_82A3CD80", "sub_82A3C440", "__savegprlr_28", "__restgprlr_28",
                     "__savegprlr_25", "__restgprlr_25", "__savegprlr_24", "__restgprlr_24",
                     "__savegprlr_27", "__restgprlr_27")})
    renames = {name: "reference_" + name.lstrip("_") for name in origins}
    source = (generated / "simpsons_recomp.65.cpp").read_text()
    start = source.index("static bool HandPatchEnsureWritable65(")
    end = source.index("\n}", start) + 2
    helper = source[start:end]
    hashes = {"HandPatchEnsureWritable65": hashlib.sha256(helper.encode()).hexdigest()}
    if register_locals:
        import importlib.util
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    growth = 'REX_FUNC(sub_82459810) { (void)ctx; (void)base; throw std::runtime_error("Reference required retirement-buffer growth"); }'
    pieces = [helper, growth] + [f"void {n}(PPCContext&, uint8_t*);" for n in renames.values()]
    for name, file in origins.items():
        body = extract(generated / file, name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside = []
            body = module.rewrite(body, name, outside)
            if outside:
                raise RuntimeError("Reference registers outside generated function")
        body = body.replace(f"DEFINE_REX_FUNC({name})",
                            f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name in ("sub_82A3CD80", "sub_82A3C440"):
            counter = "copy_calls" if name == "sub_82A3CD80" else "clear_calls"
            body = body.replace("REX_FUNC_PROLOGUE();", f"REX_FUNC_PROLOGUE();\n ++{counter};", 1)
        for old, new in renames.items():
            body = re.sub(r"\b" + re.escape(old) + r"\(ctx, base\)", new + "(ctx, base)", body)
        pieces.append(body)
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    return [root / name for name in (
        "simpsons/src/native_gpu_resource_binding.h", "simpsons/src/native_gpu_resource_initialization.h",
        "simpsons/src/native_gpu_state.cpp", "simpsons/src/native_gpu_shader_resources.h",
        "simpsons/src/native_gpu_vertex_declaration.h", "simpsons/generated/default/simpsons_init.h",
        "simpsons/src/native_gpu_memory.h", "simpsons/src/guest_memory.h",
        "tools/bench/guest_memory_fixture.h", "tools/bench/native_gpu_resource_binding_check.cpp",
        "tools/native-renderer/check_gpu_resource_binding.py", "simpsons/register_locals.py")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-resource-binding-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_resource_binding_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_resource_binding_check.cpp"),
                   str(root / "tools/rexglue-sdk/src/core/mutex.cpp"), "-pthread",
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Resource binding oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Resource binding oracle did not pass")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root, register_locals)[1] != hashes:
            raise RuntimeError("Reference writers changed during qualification")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-resource-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
