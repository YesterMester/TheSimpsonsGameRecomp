#!/usr/bin/env python3
"""Compare bounded device draw-header stores in live and owned recorded buffers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from check_gpu_constants import compiler_flags, extract

def reference_source(root):
    import re
    functions = [(16, name, 2) for name in
                 ["sub_8244C450", "sub_8244C970", "sub_8244CF78", "sub_8244D360", "sub_8244D7B8", "sub_8244DC30"]]
    functions += [(16, "sub_824529C0", 1)]
    functions += [(17, name, 1) for name in
                  ["sub_82455570", "sub_8246AEF0", "sub_8246AF58", "sub_8246C750"]]
    pieces = []; hashes = {}; registers = []; state_stores = []
    pattern = re.compile(r"REX_STORE_U32\(ea, simpsons::native_gpu_calls::DirectRingDrawHeader\(base, ctx.r31.u32, ea, ctx\.(r[0-9]+)\.u32\)\);")
    state_pattern = re.compile(r"REX_STORE_U32\(ea, simpsons::native_gpu_calls::DirectRingStateHeader\(base, ctx.r31.u32, ea, ctx\.(r[0-9]+)\.u32\)\);")
    for unit, name, expected in functions:
        file = root / f"simpsons/generated/default/simpsons_recomp.{unit}.cpp"
        body = extract(file, name)
        matches = list(pattern.finditer(body))
        if len(matches) != expected:
            raise RuntimeError("Direct draw header store inventory changed: " + name)
        states = list(state_pattern.finditer(body))
        if len(states) != int(unit == 16 and name != "sub_824529C0" or name == "sub_8246C750"):
            raise RuntimeError("Direct vertex-offset store inventory changed: " + name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        original = pattern.sub(lambda m: "REX_STORE_U32(ea, ctx." + m[1] + ".u32);", body)
        original = state_pattern.sub(lambda m: "REX_STORE_U32(ea, ctx." + m[1] + ".u32);", original)
        hashes[name + "_without_direct_header_stores"] = hashlib.sha256(original.encode()).hexdigest()
        for m, state in [(m, False) for m in matches] + [(m, True) for m in states]:
            i = len(registers); registers.append(m[1])
            state_stores.append(state)
            patched = m[0]; old = "REX_STORE_U32(ea, ctx." + m[1] + ".u32);"
            for native, statement in [(False, old), (True, patched)]:
                prefix = "patched_" if native else "reference_"
                pieces.append(f"void {prefix}{i}(PPCContext& ctx, uint8_t* base, uint32_t ea) {{\nREX_FUNC_PROLOGUE();\n{statement}\n}}\n")
    pieces.append("using Store = void(*)(PPCContext&, uint8_t*, uint32_t);\n")
    for name in ["patched", "reference"]:
        pieces.append("const std::array<Store, " + str(len(registers)) + "> " + name + "_stores = {" + ",".join(name + "_" + str(i) for i in range(len(registers))) + "};\n")
    pieces.append("const std::array<PPCRegister PPCContext::*, " + str(len(registers)) + "> header_registers = {" + ",".join("&PPCContext::" + reg for reg in registers) + "};\n")
    pieces.append("const std::array<bool, " + str(len(registers)) + "> state_stores = {" + ",".join("true" if state else "false" for state in state_stores) + "};\n")
    return "\n".join(pieces), hashes

def inputs(root):
    return [root / p for p in (
        "simpsons/src/native_gpu_direct_draws.h", "simpsons/src/native_gpu_calls.h",
        "simpsons/src/native_gpu_calls.cpp", "simpsons/src/guest_memory.h",
        "simpsons/src/native_gpu_memory.h", "tools/bench/guest_memory_fixture.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_direct.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_predication.h",
        "tools/bench/native_gpu_direct_ring_check.cpp", "tools/native-renderer/check_gpu_direct_ring.py")]


def run(root, build, output):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs(root)}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-direct-ring-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_direct_ring_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_direct_ring_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Direct draw store oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Direct draw store oracle did not pass")
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
        with tempfile.TemporaryDirectory(prefix="simpsons-direct-ring-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
