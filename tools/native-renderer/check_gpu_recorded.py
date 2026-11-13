#!/usr/bin/env python3
"""Compare native record views through the game's actual buffer-editing routines."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_gpu_constants import compiler_flags, extract

HOOK = ("\t// HAND PATCH: expose native record payloads to the original buffer editors.\n"
        "\tctx.r9.u64 = native_view.Header(ctx.r9.u32, ctx.r11.u32);\n")
VIEW_HOOK = ("\t// HAND PATCH: keep aggregate native editing state inside the copied iterator.\n"
             "\tsimpsons::native_gpu_calls::RecordedIteratorView native_view(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32);\n")
RESTART_HOOKS = (
    "\t// HAND PATCH: retain aggregate origin when restarting at an inner draw.\n\tuint32_t native_restart_flags = 0;\n",
    "\tnative_restart_flags = simpsons::native_gpu_calls::RecordedIteratorView::RestartFlags(base, ctx.r30.u32, ctx.r28.u32);\n",
    "\tif (native_restart_flags) simpsons::StoreGuestU32(base, ctx.r30.u32 + 24, native_restart_flags);\n")
EDITORS = ["sub_82458B80", "sub_82459260", "sub_82459630", "sub_824599F8",
           "sub_82459A70", "sub_82459CF0", "sub_82458818", "sub_82458890", "sub_8245BB48",
           "sub_82459068", "sub_824586B0", "sub_824587B0", "sub_8245AF30",
           "sub_8245B2E8", "sub_8245B410", "sub_8245B578", "sub_8245B798",
           "sub_8245B9B8", "sub_8245BC90", "sub_8245BE28", "sub_8245BFC0", "sub_8245C0F8"]


def reference_source(root, register_locals=False):
    generated = root / "simpsons/generated/default"
    functions = EDITORS + ["sub_82A3C440", "sub_82A3CD80"]
    for n in (25, 26, 27, 28):
        functions.extend([f"__savegprlr_{n}", f"__restgprlr_{n}"])
    bodies = {name: extract(generated / ("simpsons_recomp.17.cpp" if name in EDITORS
                                        else "simpsons_recomp.65.cpp"), name)
              for name in functions}
    if bodies[EDITORS[0]].count(HOOK) != 1 or bodies[EDITORS[0]].count(VIEW_HOOK) != 1:
        raise RuntimeError("The buffer iterator checkpoint changed")
    hashes = {name: hashlib.sha256(body.encode()).hexdigest() for name, body in bodies.items()}
    hashes["sub_82458B80_without_hook"] = hashlib.sha256(
        bodies[EDITORS[0]].replace(HOOK, "").replace(VIEW_HOOK, "").encode()).hexdigest()
    restart = bodies["sub_82459A70"]
    for hook in RESTART_HOOKS:
        if restart.count(hook) != 1:
            raise RuntimeError("Recorded draw restart checkpoint changed")
        restart = restart.replace(hook, "")
    hashes["sub_82459A70_without_hooks"] = hashlib.sha256(restart.encode()).hexdigest()
    support = (generated / "simpsons_recomp.65.cpp").read_text()
    start = support.index("static bool HandPatchEnsureWritable65(")
    helper = support[start:support.index("\n}", start) + 2]
    hashes["HandPatchEnsureWritable65"] = hashlib.sha256(helper.encode()).hexdigest()
    pieces = [helper]
    if register_locals:
        import importlib.util
        spec = importlib.util.spec_from_file_location("register_locals", root / "simpsons/register_locals.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    for prefix in ("reference", "native"):
        renames = {name: prefix + "_" + name.lstrip("_") for name in functions}
        pieces.extend(f"void {renames[name]}(PPCContext&, uint8_t*);" for name in functions)
        for name, body in bodies.items():
            if prefix == "reference" and name == EDITORS[0]:
                body = body.replace(HOOK, "").replace(VIEW_HOOK, "")
            if prefix == "reference" and name == "sub_82459A70":
                body = restart
            if register_locals:
                body = module.rewrite(body, name, set())
            body = body.replace(f"DEFINE_REX_FUNC({name})",
                                f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
            for original, replacement in renames.items():
                body = re.sub(r"\b" + re.escape(original) + r"\(ctx, base\)",
                              replacement + "(ctx, base)", body)
            pieces.append(body)
    return "\n\n".join(pieces) + "\n", hashes


def inputs(root):
    headers = list((root / "tools/rexglue-sdk/include/rex/graphics").glob("native_gpu_*.h"))
    return headers + [root / p for p in (
        "simpsons/src/native_gpu_recorded.h", "simpsons/src/native_gpu_recorded_view.h",
        "simpsons/src/native_gpu_memory.h", "simpsons/src/guest_memory.h",
        "tools/bench/guest_memory_fixture.h", "tools/bench/native_gpu_recorded_check.cpp",
        "tools/native-renderer/check_gpu_recorded.py", "simpsons/register_locals.py")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-recorded-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_recorded_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_recorded_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Recorded buffer oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Recorded buffer oracle did not pass")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root, register_locals)[1] != hashes:
            raise RuntimeError("Reference body changed during qualification")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-recorded-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
