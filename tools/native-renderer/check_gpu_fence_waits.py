#!/usr/bin/env python3
"""Compare native allocation/marker waits with complete original roots and timer helpers."""

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
    from check_gpu_chunk_growth import reference_source as growth_reference
    source, hashes = growth_reference(root, register_locals)
    for name in ["sub_82451DE0", "sub_82452018", "sub_82451E10"]:
        if name == "sub_82451DE0":
            stub = 'REX_FUNC(sub_82451DE0) { (void)ctx; (void)base; ++blocked_calls; throw WaitRequired(); }'
        else:
            stub = f'REX_FUNC({name}) {{ (void)ctx; (void)base; throw std::runtime_error("Unexpected reference branch {name}"); }}'
        assert source.count(stub) == 1
        source = source.replace(stub, '')
    dispatch = 'REX_FUNC(sub_82457CC8) { (void)ctx; (void)base; ++recursive_calls; throw std::runtime_error("Original marker requires recursive growth"); }'
    assert source.count(dispatch) == 1
    source = source.replace(dispatch, 'REX_FUNC(sub_82457CC8) { WaitDispatchBoundary(ctx, base); }')
    entries = {"sub_82451DE0":"wait_begin_entry", "sub_82452018":"wait_poll_entry", "sub_82451E10":"wait_end_entry"}
    for old, new in entries.items():
        source = re.sub(r"\b" + old + r"\(ctx, base\)", new + "(ctx, base)", source)
    prefixes = [f"void {name}(PPCContext&, uint8_t*);" for name in entries.values()]
    names = {"sub_82451DE0":16, "sub_82452018":16, "sub_82451E10":16, "sub_82432FF0":15}
    renames = {name:"reference_"+name for name in names}
    renames.update({name:"reference_"+name.lstrip("_") for name in ("__savegprlr_29", "__restgprlr_29")})
    if register_locals:
        spec=importlib.util.spec_from_file_location("register_locals",root/"simpsons/register_locals.py")
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    pieces = prefixes + [source] + [f"void {renames[name]}(PPCContext&, uint8_t*);" for name in names]
    counters={"sub_82451DE0":"wait_begin_calls", "sub_82452018":"wait_poll_calls", "sub_82451E10":"wait_end_calls"}
    for name, number in names.items():
        body = extract(root/f"simpsons/generated/default/simpsons_recomp.{number}.cpp", name)
        hashes[name] = hashlib.sha256(body.encode()).hexdigest()
        if register_locals:
            outside=[];body=module.rewrite(body,name,outside)
            if outside:raise RuntimeError("Reference register rewrite escaped its function")
        body=body.replace(f"DEFINE_REX_FUNC({name})",f"void {renames[name]}(PPCContext& ctx, uint8_t* base)")
        if name in counters:body=body.replace("REX_FUNC_PROLOGUE();","REX_FUNC_PROLOGUE();\n ++"+counters[name]+";",1)
        for old,new in renames.items():body=re.sub(r"\b"+old+r"\(ctx, base\)",new+"(ctx, base)",body)
        body=body.replace("REX_QUERY_TIMEBASE()","WaitTimebase(base)")
        body=body.replace("sub_82466388(ctx, base);","WaitHangBoundary(ctx, base);")
        pieces.append(body)
    pieces += [
        'void wait_begin_entry(PPCContext& ctx,uint8_t* base) { WaitBeginObserved(ctx,base); reference_sub_82451DE0(ctx,base); }',
        'void wait_poll_entry(PPCContext& ctx,uint8_t* base) { WaitPollStep(ctx,base); reference_sub_82452018(ctx,base); }',
        'void wait_end_entry(PPCContext& ctx,uint8_t* base) { WaitEndObserved(ctx,base); reference_sub_82451E10(ctx,base); }']
    return "\n\n".join(pieces)+"\n",hashes


def inputs(root):
    from check_gpu_chunk_growth import inputs as growth_inputs
    return growth_inputs(root)+[root/p for p in (
        "simpsons/src/native_gpu_fence_waits.h", "simpsons/src/native_gpu_ring_submission.h",
        "simpsons/src/native_gpu_chunk_close.h", "tools/bench/native_gpu_fence_waits_check.cpp",
        "tools/native-renderer/check_gpu_fence_waits.py", "simpsons/src/ring_wait.cpp")]


def run(root, build, output, register_locals):
    if shutil.disk_usage(root).free < 25 * 1024**3:
        raise RuntimeError("The 25 GiB working reserve is not available")
    output.mkdir(parents=True, exist_ok=True)
    reference, hashes = reference_source(root, register_locals)
    paths = inputs(root)
    sources = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    compiler, flags, cwd = compiler_flags(build)
    with tempfile.TemporaryDirectory(prefix="simpsons-fence-waits-", dir="/dev/shm") as temporary:
        ram = Path(temporary)
        (ram / "native_gpu_chunk_growth_reference.inc").write_text(reference)
        command = [compiler, *flags, "-g0", "-I" + str(root / "simpsons/generated/default"),
                   "-I" + str(root / "simpsons/src"), "-I" + str(ram),
                   str(root / "tools/bench/native_gpu_fence_waits_check.cpp"),
                   "--ld-path=" + str(root / "tools/clang20/bin/ld.lld"),
                   "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-o", str(ram / "check")]
        (output / "build.json").write_text(json.dumps(dict(command=command, cwd=cwd), indent=2) + "\n")
        with (output / "build.log").open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(ram / "check")], capture_output=True, text=True, timeout=180)
        (output / "run.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Fence-waits oracle exited {result.returncode}")
        report = json.loads(result.stdout)
        if not report.get("passed"):
            raise RuntimeError("Fence-waits oracle failed")
        if sources != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}:
            raise RuntimeError("Source changed during qualification")
        if reference_source(root, register_locals)[1] != hashes:
            raise RuntimeError("Reference changed during qualification")
        report.update(source_hashes=sources, reference_body_hashes=hashes, register_locals=register_locals)
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
        with tempfile.TemporaryDirectory(prefix="simpsons-fence-waits-report-", dir="/dev/shm") as temporary:
            run(root, args.sdk_build.resolve(), Path(temporary), args.register_locals)


if __name__ == "__main__":
    main()
