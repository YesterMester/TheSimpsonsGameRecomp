#!/usr/bin/env python3
"""Qualify native call transports on real backends using matching private builds.

Build native_geometry_check and its SDK together first. This checks draw/state
submission and exact GPU output; it does not qualify the whole game or devices
other than the driver running the fixture. Windows modes can use Proton.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-exe", type=Path)
    parser.add_argument("--linux-runtime", type=Path)
    parser.add_argument("--windows-exe", type=Path)
    parser.add_argument("--proton", type=Path)
    parser.add_argument("--compat-data", type=Path)
    parser.add_argument("--steam-root", type=Path)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--native-state", action="store_true",
                        help="Also qualify native render/fetch/bool/flush state ranges")
    parser.add_argument("--vertex-stride", type=int, default=16,
                        help="Consumer vertex stride in bytes (28 matches utility clear geometry)")
    parser.add_argument("--explicit-major", action="store_true",
                        help="Exercise the major-mode bit used by vertex-ID draw calls")
    parser.add_argument("--native-coherency", action="store_true",
                        help="Qualify ordered native coherency before sampler/texture use")
    parser.add_argument("--native-shaders", action="store_true",
                        help="Qualify native inline shader binding and state invalidation")
    parser.add_argument("--native-predication", action="store_true",
                        help="Qualify native mask halves and selected state updates")
    parser.add_argument("--native-ports", action="store_true",
                        help="Qualify initialization register ports and the opposite shader stage")
    parser.add_argument("--native-sync", action="store_true",
                        help="Qualify retirement coherency and shader-completion events")
    parser.add_argument("--native-indirect", action="store_true",
                        help="Use indirect constants for all GPU result checks and raw-bit probes")
    parser.add_argument("--shader-memory-cache", action="store_true",
                        help="Check shader contents changing at one address within an epoch")
    parser.add_argument("--native-reference", action="store_true",
                        help="Use memory-reference shader bindings for all GPU output checks")
    parser.add_argument("--native-selected-ranges", action="store_true",
                        help="Qualify selected register ranges and predicated shader references")
    parser.add_argument("--native-typed-indirect", action="store_true",
                        help="Use typed indirect constants for live shader, fetch and context updates")
    parser.add_argument("--native-direct", action="store_true",
                        help="Render the GPU fixtures through native direct draw arguments")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache = args.cache_dir.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    inputs = [root / p for p in (
        "tools/native-renderer/native_geometry_check.cpp",
        "tools/rexglue-sdk/include/rex/graphics/native_gpu_calls.h",
        "tools/rexglue-sdk/src/graphics/native_gpu_calls.cpp",
        "tools/rexglue-sdk/include/rex/graphics/command_processor.h",
        "tools/rexglue-sdk/src/graphics/command_processor.cpp")]
    if args.native_state:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_state.h")
    if args.native_coherency:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_coherency.h")
    if args.native_shaders:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_program.h")
    if args.native_predication:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_predication.h")
    if args.native_ports:
        require(args.native_shaders, "Initialization checks require native shaders")
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_ports.h")
        inputs.extend(root / path for path in (
            "tools/rexglue-sdk/include/rex/graphics/pipeline/shader/cache_key.h",
            "tools/rexglue-sdk/include/rex/graphics/vulkan/pipeline_cache.h",
            "tools/rexglue-sdk/include/rex/graphics/d3d12/pipeline_cache.h",
            "tools/rexglue-sdk/src/graphics/vulkan/pipeline_cache.cpp",
            "tools/rexglue-sdk/src/graphics/d3d12/pipeline_cache.cpp",
            "tools/rexglue-sdk/src/graphics/d3d11/pipeline_cache.cpp"))
    if args.native_sync:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_sync.h")
    if args.native_indirect:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_indirect.h")
    if args.native_reference:
        require(args.native_shaders and args.shader_memory_cache,
                "Shader-reference checks need native shaders and memory-cache probes")
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_shader_reference.h")
    if args.native_selected_ranges:
        require(args.native_predication and args.native_reference,
                "Conditional ranges need native predication and shader references")
    if args.native_typed_indirect:
        require(args.native_indirect and args.native_state,
                "Typed constants need native indirect constants and native state")
    if args.native_direct:
        inputs.append(root / "tools/rexglue-sdk/include/rex/graphics/native_gpu_direct.h")
    binaries = []
    if args.linux_exe:
        require(args.linux_runtime, "Specify the matching private Linux runtime")
        binaries.extend([args.linux_exe.resolve(), args.linux_runtime.resolve()])
    if args.windows_exe:
        binaries.extend([args.windows_exe.resolve(),
                         args.windows_exe.resolve().parent / "librexruntime.dll"])
        if args.proton:
            require(args.compat_data and args.steam_root,
                    "Proton needs a private compatibility directory and Steam root")
    require(binaries, "Specify at least one platform executable")
    hashes = {str(p): sha(p) for p in inputs + binaries}
    modes = (["native", "compute"] if args.linux_exe else []) + (
        ["dx11", "dx12", "dx12-bindful"] if args.windows_exe else [])
    checks = []
    for mode in modes:
        require(shutil.disk_usage(root).free >= 25 * 1024**3, "25 GiB disk reserve reached")
        directory = output / ("gpu-" + mode)
        directory.mkdir(exist_ok=True)
        require(not (directory / "command-processor-report.json").exists(),
                f"Use a new output directory; a previous report exists for {mode}")
        env = dict(os.environ, XDG_CACHE_HOME=str(cache), MESA_SHADER_CACHE_DIR=str(cache / "mesa"))
        if mode in ("native", "compute"):
            exe = args.linux_exe.resolve()
            env["LD_LIBRARY_PATH"] = str(args.linux_runtime.resolve().parent)
            command = [str(exe), str(directory), mode]
        else:
            exe = args.windows_exe.resolve()
            if args.proton:
                env.update(STEAM_COMPAT_DATA_PATH=str(args.compat_data.resolve()),
                           STEAM_COMPAT_CLIENT_INSTALL_PATH=str(args.steam_root.resolve()),
                           WINEDEBUG="-all", WINEDLLOVERRIDES="d3dcompiler_47=n,b")
                command = [str(args.proton.resolve()), "run", str(exe),
                           "Z:" + str(directory).replace("/", "\\"), mode]
            else:
                command = [str(exe), str(directory), mode]
        command.extend(["--gpu-indices", "--affine-addresses", "--buffer-budget-mb", "16",
                        "--clear-caches", "--native-calls", "--native-constants", "--native-vertices"])
        if args.native_state:
            command.append("--native-state")
        if args.vertex_stride != 16:
            command.extend(["--vertex-stride", str(args.vertex_stride)])
        if args.explicit_major:
            command.append("--explicit-major")
        if args.native_coherency:
            command.append("--native-coherency")
        if args.native_shaders:
            command.append("--native-shaders")
        if args.native_predication:
            command.append("--native-predication")
        if args.native_ports:
            command.append("--native-ports")
        if args.native_sync:
            command.append("--native-sync")
        if args.native_indirect:
            command.append("--native-indirect")
        if args.shader_memory_cache:
            command.append("--shader-memory-cache")
        if args.native_reference:
            command.append("--native-reference")
        if args.native_selected_ranges:
            command.append("--native-selected-ranges")
        if args.native_typed_indirect:
            command.append("--native-typed-indirect")
        if args.native_direct:
            command.append("--native-direct")
        started = time.monotonic()
        with (directory / "run.log").open("w") as log:
            try:
                result = subprocess.run(command, cwd=exe.parent, env=env,
                                        stdout=log, stderr=subprocess.STDOUT, timeout=300)
            except subprocess.TimeoutExpired as error:
                (directory / "run.json").write_text(json.dumps(dict(
                    command=command, cwd=str(exe.parent), exit_code=None, timed_out=True,
                    seconds=time.monotonic() - started), indent=2) + "\n")
                raise RuntimeError(f"{mode} fixture timed out; inspect {directory}") from error
        (directory / "run.json").write_text(json.dumps(dict(
            command=command, cwd=str(exe.parent), exit_code=result.returncode,
            seconds=time.monotonic() - started), indent=2) + "\n")
        require(result.returncode == 0, f"{mode} fixture failed; inspect {directory}")
        report = json.loads((directory / "command-processor-report.json").read_text())
        coverage = json.loads((directory / "native-coverage.json").read_text())
        require(report.get("passed") and report["cases"] == 120 and
                report["exact_bytes"] == 251658240 and report["cache_clears"] == 80,
                f"{mode} output/case coverage changed")
        require(report["vertex_stride"] == args.vertex_stride,
                f"{mode} did not exercise the requested vertex stride")
        require(report["explicit_major"] == args.explicit_major,
                f"{mode} did not exercise the requested major mode")
        require(report["requested_explicit_draws"] == (400 if args.explicit_major else 0),
                f"{mode} explicit draw request coverage changed")
        require(report["native_coherency_calls"] == (121 if args.native_coherency else 0),
                f"{mode} native coherency coverage changed")
        require(report["native_shader_calls"] == (440 + int(args.native_ports) if args.native_shaders else 0) and
                report["native_invalidation_calls"] == (1 if args.native_shaders else 0),
                f"{mode} native program coverage changed")
        require(report["restored_snapshots"] == 440 and
                report["native_mask_calls"] == (8 if args.native_predication else 0) and
                report["native_selected_states"] == ((1332 if args.native_predication else 0) +
                                                       (5 if args.native_selected_ranges else 0)) and
                report["native_skipped_states"] == ((6 if args.native_predication else 0) +
                                                      (5 if args.native_selected_ranges else 0)),
                f"{mode} native predication coverage changed")
        require(report["native_port_calls"] == (3 if args.native_ports else 0) and
                report["native_port_writes"] == (10 if args.native_ports else 0) and
                report["native_pixel_shader_bindings"] == (1 if args.native_ports else 0),
                f"{mode} native initialization coverage changed")
        require(report["native_sync_coherency_calls"] == (1 if args.native_sync else 0) and
                report["native_shader_events"] == (8 if args.native_sync else 0),
                f"{mode} native synchronization coverage changed")
        require(report["native_indirect_calls"] == (3348 if args.native_typed_indirect else
                                                      248 if args.native_indirect else 0) and
                report["native_indirect_words"] == (563900 if args.native_typed_indirect else
                                                      7946 if args.native_indirect else 0),
                f"{mode} native indirect-constant coverage changed")
        require(report["native_typed_indirect_calls"] == (3348 if args.native_typed_indirect else 0) and
                report["typed_indirect_types"] == (31 if args.native_typed_indirect else 0) and
                report["typed_restore_calls"] == (3080 if args.native_typed_indirect else 0),
                f"{mode} typed-constant coverage changed")
        require(report["shader_memory_bindings"] == (4 if args.shader_memory_cache else 0),
                f"{mode} shader memory-cache coverage changed")
        require(report["native_shader_references"] == (445 if args.native_reference else 0) and
                report["skipped_shader_references"] == (1 if args.native_reference else 0),
                f"{mode} native shader-reference coverage changed")
        require(report["conditional_ranges"] == (10 if args.native_selected_ranges else 0) and
                report["conditional_range_words"] == (524 if args.native_selected_ranges else 0) and
                report["predicated_shader_references"] == (4 if args.native_selected_ranges else 0),
                f"{mode} conditional range or shader coverage changed")
        require(report["native_constant_updates"] == 240 and report["native_submissions"] == 564 and
                (report["indexed_vertices_submissions"] == 1 if args.native_direct else
                    report["indexed_vertices_submissions"] > 1) and
                (report["indexed_range_submissions"] == 1 if args.native_direct else
                    report["indexed_range_submissions"] > 1) and
                report["bin_skipped_vertices"] == 2 and report["bin_skipped_ranges"] == 2 and
                report["bin_skipped_bare"] == 1, f"{mode} native transport coverage is incomplete")
        require(report["direct_draws"] == (560 if args.native_direct else 0) and
                report["direct_indexed"] == (320 if args.native_direct else 0) and
                report["direct_queries"] == (280 if args.native_direct else 0) and
                report["direct_padded_auto"] == (120 if args.native_direct else 0) and
                report["direct_skips"] == (1 if args.native_direct else 0),
                f"{mode} direct draw coverage changed")
        if args.native_state:
            require(report["native_state_calls"] == (447 if args.native_typed_indirect else 3087) and
                    report["native_state_registers"] == (1357 if args.native_typed_indirect else 553997),
                    f"{mode} native state coverage changed")
        require(coverage["strict"] and coverage["complete"] and not coverage["failed"] and
                coverage["operations"]["native_draw_call"] == report["native_submissions"] and
                coverage["operations"]["legacy_draw_packet"] == 0,
                f"{mode} did not finish strictly through native calls")
        require(all(v["calls"] == 0 and v["bytes"] == 0
                    for v in coverage["dependencies"].values()) and
                all(v["calls"] == 0 and v["virtual_bytes"] == 0 and v["resident_bytes"] == 0
                    for v in coverage["allocations"].values()),
                f"{mode} used a compatibility dependency or allocation")
        if mode in ("native", "compute"):
            require(report["observed_explicit_draws"] == (400 if args.explicit_major else 0),
                    f"{mode} native draw major mode changed")
            require(report["observed_draws"] == 560 and report["observed_indexed_draws"] == 320 and
                    report["observed_halfword_offsets"] == 160, f"{mode} backend draw arguments changed")
        checks.append(dict(mode=mode, report=report,
                           coverage_sha256=sha(directory / "native-coverage.json")))
        print(mode, "passed", flush=True)
    require(all(sha(Path(p)) == value for p, value in hashes.items()),
            "Source or binary changed during qualification")
    final = dict(passed=True, scope="Native call fixtures on the current driver; Windows APIs may use Proton",
                 full_game_native_proven=False, source_and_binary_hashes=hashes,
                 cases=sum(c["report"]["cases"] for c in checks),
                 exact_bytes=sum(c["report"]["exact_bytes"] for c in checks), configurations=checks)
    (output / "gpu-qualification.json").write_text(json.dumps(final, indent=2) + "\n")
    print(json.dumps({k: final[k] for k in ("passed", "cases", "exact_bytes")}))


if __name__ == "__main__":
    main()
