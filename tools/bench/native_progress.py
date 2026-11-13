#!/usr/bin/env python3
"""Summarize recorded native GPU counters without claiming whole-game coverage.

New reports pair outcomes at defined command and resource boundaries. Older
reports have only paired draw counters. Dependency events never serve as an
attempt denominator. Missing evidence and unexercised systems remain explicit.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

from native_coverage import ALLOCATIONS, DEPENDENCIES

ROUTE_SYSTEMS = {
    "Draw command dispatch": "draw_dispatch",
    "Geometry source selection": "geometry",
    "Texture upload commits": "textures",
    "Host resolve requests": "resolves",
    "Shader command dispatch": "shader_commands",
    "GPU synchronization": "synchronization",
    "Buffer submissions": "submission",
    "Presentation commands": "presentation",
    "State and constants": "state_commands",
    "Queries": "queries",
    "Other commands": "other_commands",
    "Host presentation": "host_presentation",
}
OUTCOMES = ("native", "retained", "mixed", "failed", "skipped", "unclassified")


def count(value):
    return type(value) is int and value >= 0


def read_sample(path):
    row = {"path": str(path.resolve()), "errors": []}
    try:
        raw = path.read_bytes()
        row["sha256"] = hashlib.sha256(raw).hexdigest()
        data = json.loads(raw)
        if not isinstance(data, dict):
            raise ValueError("report must be an object")
    except (OSError, ValueError) as error:
        row["errors"].append(str(error))
        return row
    row["report"] = data
    errors = row["errors"]
    if type(data.get("version")) is not int or data["version"] != 1:
        errors.append("unsupported report version")
    if data.get("backend") not in {"vulkan", "d3d11", "d3d12"}:
        errors.append("unknown backend")
    if data.get("complete") is not True or data.get("failed") is not False:
        errors.append("run did not finish successfully")
    if type(data.get("strict")) is not bool:
        errors.append("strict mode was not recorded")
    if not count(data.get("frames")) or data["frames"] == 0:
        errors.append("no rendered frames")
    operations = data.get("operations")
    if not isinstance(operations, dict) or not all(count(v) for v in operations.values()):
        errors.append("invalid operation counters")
    for name, required, keys in (
        ("dependencies", DEPENDENCIES, ("calls", "bytes")),
        ("allocations", ALLOCATIONS, ("calls", "virtual_bytes", "resident_bytes")),
    ):
        counters = data.get(name)
        if not isinstance(counters, dict) or not required.issubset(counters):
            errors.append(f"missing {name} counters")
            continue
        for kind, value in counters.items():
            if not isinstance(value, dict) or not all(count(value.get(key)) for key in keys):
                errors.append(f"invalid {name} counter: {kind}")
    if "route_coverage" in data:
        coverage = data["route_coverage"]
        if (not isinstance(coverage, dict) or type(coverage.get("version")) is not int
                or coverage["version"] != 1 or not isinstance(coverage.get("systems"), dict)):
            errors.append("invalid route coverage schema")
        else:
            if set(coverage["systems"]) != set(ROUTE_SYSTEMS.values()):
                errors.append("route system inventory does not match schema version 1")
            for system in ROUTE_SYSTEMS.values():
                value = coverage["systems"].get(system)
                if (not isinstance(value, dict) or not all(
                        count(value.get(key)) for key in (*OUTCOMES, "attempts", "in_flight"))):
                    errors.append(f"missing or invalid route counters: {system}")
                elif value["attempts"] != sum(value[key] for key in (*OUTCOMES, "in_flight")):
                    errors.append(f"route outcomes do not sum to attempts: {system}")
                elif data.get("complete") is True and value["in_flight"]:
                    errors.append(f"completed report has unfinished attempts: {system}")
    return row


def percentage(native, total):
    value = 100 * native / total if total else None
    if native < total and value == 100:
        value = math.nextafter(100.0, 0.0)
    return value


def route_share(rows, system):
    totals = dict.fromkeys((*OUTCOMES, "attempts", "in_flight"), 0)
    for row in rows:
        if row["errors"]:
            return {"percent": None, "reason": "invalid or incomplete input report"}
        coverage = row["report"].get("route_coverage")
        if coverage is None:
            return {"percent": None, "reason": "paired route counters were not recorded"}
        for key in totals:
            totals[key] += coverage["systems"][system][key]
    executed = totals["attempts"] - totals["skipped"]
    complete = not totals["unclassified"] and not totals["in_flight"]
    if not executed:
        reason = "not exercised (no executed attempts)"
    elif not complete:
        reason = "some attempts remain unclassified or unfinished"
    else:
        reason = "native completions / executed attempts; intentional skips excluded"
    return {**totals, "executed": executed,
            "percent": percentage(totals["native"], executed) if complete else None,
            "reason": reason, "measurement": "paired routes v1"}


def draw_share(rows):
    if any("route_coverage" in row.get("report", {}) for row in rows):
        return route_share(rows, "draw_dispatch")
    native = legacy = 0
    for row in rows:
        if row["errors"]:
            return {"percent": None, "reason": "invalid or incomplete input report"}
        operations = row["report"]["operations"]
        if any(key not in operations for key in ("native_draw_call", "legacy_draw_packet")):
            return {"percent": None, "reason": "paired draw counters were not recorded"}
        native += operations["native_draw_call"]
        legacy += operations["legacy_draw_packet"]
    total = native + legacy
    return {"native": native, "legacy": legacy, "total": total,
            "percent": percentage(native, total),
            "reason": "counted draw dispatches only" if total else "no draws were counted"}


def total_counter(rows, group, kind, key=None):
    total = 0
    for row in rows:
        if row["errors"]:
            return None
        value = row["report"].get(group, {}).get(kind)
        if key and isinstance(value, dict):
            value = value.get(key)
        if not count(value):
            return None
        total += value
    return total


def summarize(rows, scope):
    systems = {name: route_share(rows, key) for name, key in ROUTE_SYSTEMS.items()}
    systems["Draw command dispatch"] = draw_share(rows)
    for label, operations, dependency in (
        ("Geometry source selection", ("vertex_streams",), "vertex_fallback"),
        ("Texture upload commits", ("cpu_texture_upload", "gpu_texture_upload"), "texture_fallback"),
        ("Host resolve requests", ("resolve",), "resolve_fallback"),
    ):
        values = [total_counter(rows, "operations", kind) for kind in operations]
        systems[label].update({
            "recorded_native_operations": sum(values) if all(v is not None for v in values) else None,
            "recorded_dependency_events": total_counter(rows, "dependencies", dependency, "calls"),
        })
    backends = {}
    for backend in ("vulkan", "d3d11", "d3d12"):
        samples = [row for row in rows if row.get("report", {}).get("backend") == backend]
        if samples:
            backends[backend] = {"reports": len(samples), **draw_share(samples)}
    return {
        "version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "scope": scope,
        "valid_input_reports": all(not row["errors"] for row in rows),
        "overall_completion_percent": None,
        "full_game_native_proven": False,
        "current_build_qualified": False,
        "tracked_systems": len(ROUTE_SYSTEMS),
        "exercised_systems": sum(value.get("executed", value.get("total", 0)) > 0
                                 for value in systems.values()),
        "paired_counters_present": all("route_coverage" in row.get("report", {}) for row in rows),
        "legacy_dependency_events": (sum(value["calls"] for row in rows
                                         for value in row["report"]["dependencies"].values())
                                     if all(not row["errors"] for row in rows) else None),
        "legacy_allocation_events": (sum(value["calls"] for row in rows
                                         for value in row["report"]["allocations"].values())
                                     if all(not row["errors"] for row in rows) else None),
        "systems": systems,
        "backends": backends,
        "samples": rows,
    }


def bar(percent):
    if percent is None:
        return "?" * 20 + "   unknown"
    # A value just below 100 must not appear as a full bar or round up to 100.
    filled = min(20, int(percent / 5))
    text = "100%" if percent == 100 else "<100%" if percent > 99.99 else f"{percent:.2f}%"
    return "█" * filled + "░" * (20 - filled) + "   " + text


def markdown(report):
    lines = ["# Recorded native GPU measurements", "", report["scope"], "",
             "These are recorded-work measurements, not development completion percentages.",
             "Saved reports do not qualify the current build or unrecorded campaign states.", "",
             "```text"]
    for name, system in report["systems"].items():
        value = ("·" * 20 + "   not exercised"
                 if system["reason"].startswith("not exercised") else bar(system["percent"]))
        lines.append(f"{name:25} {value}")
    lines += ["```", "", "Unknown does not mean 0%. No overall percentage is calculated.",
              f"Exercised systems: {report['exercised_systems']}/{report['tracked_systems']}.", "",
              f"Reported compatibility dependency events: {report['legacy_dependency_events']}; "
              f"compatibility allocation events: {report['legacy_allocation_events']}.", "",
              "Paired percentage = native completions / executed attempts, excluding intentional skips.",
              "Mixed, retained and failed attempts stay in the denominator. Unclassified or",
              "unfinished attempts prevent a percentage. Resource and command units differ.",
              "Old reports use native_draw_call / (native_draw_call + legacy_draw_packet),",
              "including native predicated no-ops. No ratio proves complete API conversion.", "",
              "| Renderer | Counted native draws | Counted legacy draws | Native share |",
              "|---|---:|---:|---:|"]
    for backend, value in report["backends"].items():
        share = "unknown" if value["percent"] is None else bar(value["percent"]).split()[-1]
        lines.append(f"| {backend} | {value.get('native', 'unknown')} | "
                     f"{value.get('legacy', value.get('retained', 'unknown'))} | {share} |")
    lines += ["", "| System | Native | Retained | Mixed | Failed | Skipped | Unclassified | Executed |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for name, system in report["systems"].items():
        if system.get("measurement") == "paired routes v1":
            values = [str(system[key]) for key in (*OUTCOMES, "executed")]
            lines.append(f"| {name} | " + " | ".join(values) + " |")
    lines += ["", "| System | Recorded native operations | Recorded dependency events |",
              "|---|---:|---:|"]
    for name, system in report["systems"].items():
        if "recorded_native_operations" in system:
            operations = system["recorded_native_operations"]
            events = system["recorded_dependency_events"]
            lines.append(f"| {name} | {operations if operations is not None else 'unknown'} | "
                         f"{events if events is not None else 'unknown'} |")
    lines += ["", "Dependency events can overlap and differ from native operation counts.",
              "Zero operations means unexercised, even when dependencies are also zero.", "",
              "## Input evidence", ""]
    for sample in report["samples"]:
        lines.append(f"- `{sample['path']}`")
        lines.append(f"  SHA-256: `{sample.get('sha256', 'unavailable')}`")
        for error in sample["errors"]:
            lines.append(f"  ERROR: {error}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--scope", required=True, help="identify the run, build, device and coverage limits")
    parser.add_argument("--output", type=Path, help="write JSON including evidence hashes")
    parser.add_argument("--markdown", type=Path, help="write a block graph and evidence table")
    parser.add_argument("--relative-to", type=Path,
                        help="store input paths relative to this directory; all inputs must be inside it")
    args = parser.parse_args()
    inputs = [path.resolve() for path in args.reports]
    outputs = [path.resolve() for path in (args.output, args.markdown) if path]
    if len(set(inputs)) != len(inputs):
        parser.error("duplicate input paths would double-count observations")
    if len(set(outputs)) != len(outputs) or set(inputs) & set(outputs):
        parser.error("outputs must be separate from one another and all inputs")
    if any(path.exists() for path in outputs):
        parser.error("output already exists; choose a new snapshot path")
    if args.relative_to:
        root = args.relative_to.resolve()
        if not root.is_dir() or any(not path.is_relative_to(root) for path in inputs):
            parser.error("relative input paths must belong to an existing root directory")
    rows = [read_sample(path) for path in inputs]
    if args.relative_to:
        for row in rows:
            row["path"] = Path(row["path"]).relative_to(root).as_posix()
    report = summarize(rows, args.scope)
    document = markdown(report)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    if args.markdown:
        args.markdown.write_text(document)
    print(document)
    return 0 if report["valid_input_reports"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
