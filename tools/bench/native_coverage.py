#!/usr/bin/env python3
"""Validate native resource counters without launching the game.

Checks apply only to the supplied reports. They do not establish coverage of
unrecorded game states or qualify a different GPU driver.
"""
import argparse
import hashlib
import json
from pathlib import Path

DEPENDENCIES = {"edram", "memory_mirror", "scaled_mirror_read", "scaled_mirror_write",
                "resolve_fallback", "vertex_fallback", "texture_fallback"}
ALLOCATIONS = {"edram", "memory_mirror", "scaled_mirror"}


def read_report(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as error:
        return {"report_error": str(error)}


def validate(report, expected_backend="vulkan", required_operations=("vertex_streams", "resolve")):
    errors = []
    if not isinstance(report, dict):
        return ["coverage report is not an object"]
    if expected_backend not in {"vulkan", "d3d11", "d3d12"}:
        return ["unknown expected renderer"]
    if type(report.get("version")) is not int or report["version"] != 1 or report.get("backend") != expected_backend:
        errors.append(f"missing or unsupported {expected_backend} coverage report")
    if report.get("strict") is not True or report.get("complete") is not True:
        errors.append("strict validation did not finish")
    if report.get("failed") is not False:
        errors.append("native validation reported a failure")
    if type(report.get("frames")) is not int or report["frames"] < 1:
        errors.append("no frames were rendered")
    dependencies = report.get("dependencies", {})
    allocations = report.get("allocations", {})
    if not isinstance(dependencies, dict) or not isinstance(allocations, dict):
        return errors + ["coverage counters are not objects"]
    if not DEPENDENCIES.issubset(dependencies) or not ALLOCATIONS.issubset(allocations):
        errors.append("coverage report is missing dependency or allocation counters")
    for kind, value in dependencies.items():
        if not isinstance(value, dict) or not all(
                type(value.get(key)) is int and value[key] >= 0 for key in ("calls", "bytes")):
            errors.append(f"invalid dependency counter: {kind}")
            continue
        if any(value[key] for key in ("calls", "bytes")):
            errors.append(f"legacy dependency: {kind}: {value.get('first_detail', '')}")
    for kind, value in allocations.items():
        if not isinstance(value, dict) or not all(
                type(value.get(key)) is int and value[key] >= 0
                for key in ("calls", "virtual_bytes", "resident_bytes")):
            errors.append(f"invalid allocation counter: {kind}")
            continue
        if any(value.get(key, 0) for key in ("calls", "virtual_bytes", "resident_bytes")):
            errors.append(f"legacy allocation: {kind}")
    operations = report.get("operations", {})
    if not isinstance(operations, dict):
        errors.append("native operations are not an object")
    else:
        for kind in required_operations:
            if type(operations.get(kind)) is not int or operations[kind] < 1:
                errors.append(f"native operation was not exercised: {kind}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--backend", choices=["vulkan", "d3d11", "d3d12"], required=True)
    parser.add_argument("--require-operation", action="append", dest="operations",
                        help="required counter; defaults to vertex_streams and resolve")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output and args.output.resolve() in {p.resolve() for p in args.reports}:
        parser.error("output must be separate from input reports")
    operations = args.operations if args.operations is not None else ["vertex_streams", "resolve"]
    results = []
    for path in args.reports:
        report = read_report(path)
        errors = validate(report, args.backend, operations)
        result = {"path": str(path.resolve()), "errors": errors,
                  "frames": report.get("frames", 0) if isinstance(report, dict) else 0}
        if path.is_file():
            result["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        results.append(result)
        print(f"{'FAIL' if errors else 'PASS'} {path}: " + ("; ".join(errors) or "zero legacy dependencies"))
    passed = not any(row["errors"] for row in results)
    output = {"version": 1, "backend": args.backend, "passed": passed,
              "required_operations": operations, "full_game_native_proven": False,
              "scope": "supplied reports only", "reports": results}
    if args.output:
        args.output.write_text(json.dumps(output, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
