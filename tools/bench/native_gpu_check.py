#!/usr/bin/env python3
"""Check captured frames with strict native GPU dependencies and cumulative reports.

Traces and output stay outside the repository. --compare-alias-fallback also
checks the new surface alias path against the existing resolve implementation.
"""

import argparse
import json
import os
from pathlib import Path
import re

from copy_free_resolve_check import CVARS
from replay_ab import compare, read_ppm, run_variant, write_png

DEPENDENCIES = {"edram", "memory_mirror", "scaled_mirror_read", "scaled_mirror_write",
                "resolve_fallback", "vertex_fallback", "texture_fallback"}
ALLOCATIONS = {"edram", "memory_mirror", "scaled_mirror"}


def read_report(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as error:
        return {"report_error": str(error)}


def validate(report):
    errors = []
    if report.get("version") != 1 or report.get("backend") != "vulkan":
        errors.append("missing or unsupported Vulkan coverage report")
    if report.get("strict") is not True or report.get("complete") is not True:
        errors.append("strict validation did not finish")
    if report.get("failed") is not False:
        errors.append("native validation reported a failure")
    if report.get("frames", 0) < 1:
        errors.append("no frames were rendered")
    dependencies = report.get("dependencies", {})
    allocations = report.get("allocations", {})
    if not DEPENDENCIES.issubset(dependencies) or not ALLOCATIONS.issubset(allocations):
        errors.append("coverage report is missing dependency or allocation counters")
    for kind, value in dependencies.items():
        if not all(isinstance(value.get(key), int) for key in ("calls", "bytes")):
            errors.append(f"invalid dependency counter: {kind}")
        if value.get("calls", 0):
            errors.append(f"legacy dependency: {kind}: {value.get('first_detail', '')}")
    for kind, value in allocations.items():
        if not all(isinstance(value.get(key), int)
                   for key in ("calls", "virtual_bytes", "resident_bytes")):
            errors.append(f"invalid allocation counter: {kind}")
        if any(value.get(key, 0) for key in ("calls", "virtual_bytes", "resident_bytes")):
            errors.append(f"legacy allocation: {kind}")
    operations = report.get("operations", {})
    if not operations.get("vertex_streams", 0) or not operations.get("resolve", 0):
        errors.append("native vertex streams and resolves were not both exercised")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--replayer", required=True, type=Path)
    parser.add_argument("--scales", nargs="+", type=int, choices=range(1, 5), default=[1, 2])
    parser.add_argument("--iterations", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=150)
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                        help="additional renderer setting, such as character_tessellation=high")
    parser.add_argument("--require-alias", action="store_true")
    parser.add_argument("--compare-alias-fallback", action="store_true")
    parser.add_argument("--verify-guard", action="store_true",
                        help="also prove that deliberately using the mirror fails")
    args = parser.parse_args()
    if args.iterations < 2:
        parser.error("--iterations must be at least 2")
    for setting in args.set:
        if not re.fullmatch(r"[A-Za-z0-9_]+=[^;\r\n]+", setting):
            parser.error(f"invalid setting: {setting}")
    settings = "".join(";" + setting for setting in args.set)
    for path in [args.replayer, *args.traces]:
        if not path.is_file():
            parser.error(f"file does not exist: {path}")
    args.output = args.output.resolve()
    if any(c in str(args.output) for c in (';', '\n', '\r')):
        parser.error("output path cannot contain cvar separators")
    args.output.mkdir(parents=True, exist_ok=False)
    if os.name == "posix":
        # The negative control intentionally aborts, without dumping game data.
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    results = []
    failures = 0
    aliases = 0
    for index, trace in enumerate(args.traces):
        for scale in args.scales:
            name = f"{index:02d}-{trace.stem}-{scale}x"
            report_path = args.output / (name + ".coverage.json")
            cvars = (CVARS + settings + f";resolution_scale={scale};native_resolve_copy_free=true;"
                     "native_resolve_surface_aliases=true;native_gpu_strict=true;"
                     f"native_gpu_report_path={report_path}")
            native = run_variant(str(args.replayer.resolve()), str(trace.resolve()),
                                 str(args.output / name), cvars, args.iterations, args.timeout)
            coverage = read_report(report_path)
            errors = validate(coverage)
            errors.extend(native["errors"])
            if native["rc"] != 0 or not native["ppm"]:
                errors.append(f"replay failed: {native['rc']}")
            if native["ppm"]:
                write_png(str(args.output / (name + ".png")), read_ppm(native["ppm"]))
            aliases += coverage.get("operations", {}).get("alias_transfer", 0)
            result = {"trace": str(trace.resolve()), "scale": scale, "native": native,
                      "coverage": coverage, "errors": errors}
            if args.compare_alias_fallback:
                reference = run_variant(
                    str(args.replayer.resolve()), str(trace.resolve()),
                    str(args.output / (name + "-reference")),
                    CVARS + settings + f";resolution_scale={scale};native_resolve_copy_free=true;"
                    "native_resolve_surface_aliases=false;native_gpu_strict=false;"
                    "native_gpu_report_path=", args.iterations, args.timeout)
                result["reference"] = reference
                errors.extend(reference["errors"])
                if reference["rc"] != 0 or not reference["ppm"] or not native["ppm"]:
                    errors.append("alias reference comparison did not render both frames")
                else:
                    difference = compare(read_ppm(native["ppm"]), read_ppm(reference["ppm"]))
                    if difference is None:
                        errors.append("alias reference image sizes differ")
                    else:
                        result["different_pixels"], result["max_difference"] = difference[:2]
                        if difference[0]:
                            errors.append(f"alias reference differs at {difference[0]} pixels")
            failures += bool(errors)
            results.append(result)
            (args.output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"{'FAIL' if errors else 'PASS'} {name}: " + ("; ".join(errors) or "native"),
                  flush=True)
    if args.require_alias and not aliases:
        failures += 1
        print("FAIL no native alias transfers ran", flush=True)
    if args.verify_guard:
        report_path = args.output / "guard.coverage.json"
        guard = run_variant(
            str(args.replayer.resolve()), str(args.traces[0].resolve()),
            str(args.output / "guard"), CVARS + ";resolution_scale=1;native_vertex_buffers=false;"
            "native_gpu_strict=true;" + f"native_gpu_report_path={report_path}",
            2, args.timeout)
        coverage = read_report(report_path)
        guarded = (guard["rc"] not in (0, "timeout") and coverage.get("failed") is True and
                   coverage.get("strict") is True and
                   coverage.get("dependencies", {}).get("memory_mirror", {}).get("calls", 0) > 0)
        results.append({"negative_control": guard, "coverage": coverage, "passed": guarded})
        (args.output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
        failures += not guarded
        print(f"{'PASS' if guarded else 'FAIL'} strict guard negative control", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
