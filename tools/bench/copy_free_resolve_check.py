#!/usr/bin/env python3
"""Compare native image exchanges against native copies on captured GPU frames.

Use traces containing resolve-and-clear effect passes as well as ordinary frames.
Traces stay outside the repository because they contain game data. Replaying a
frame repeatedly exercises textures retained from earlier frames, which a single
replay does not cover.
"""

import argparse
import json
from pathlib import Path
import re

import numpy as np

from replay_ab import compare, read_ppm, run_variant, write_png


CVARS = (
    "render_target_path_vulkan=native;"
    "native_index_buffers=true;native_vertex_buffers=true;"
    "native_texture_uploads=true;native_vertex_buffer_cache=true;"
    "native_vertex_cache_refresh=true;native_index_buffer_cache=true;"
    "native_index_bounds_cache=true;pm4_bulk_state_registers=true;"
    "native_rt_image_copies=true;native_resolve=true;"
    "native_resolve_image_copies=true;native_resolve_buffers=true;"
    "native_resolve_buffer_reads=true;native_resolve_buffer_lazy_memory=true;"
    "native_resolve_buffer_texture_first=true;native_resolve_buffer_reuse=true"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--replayer", required=True, type=Path)
    parser.add_argument("--scales", nargs="+", type=int, choices=range(1, 5), default=[1, 2])
    parser.add_argument("--iterations", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--require-exchange", action="store_true",
                        help="fail unless at least one image exchange actually runs")
    args = parser.parse_args()
    if args.iterations < 2:
        parser.error("--iterations must be at least 2 to exercise retained textures")
    for path in [args.replayer, *args.traces]:
        if not path.is_file():
            parser.error(f"file does not exist: {path}")
    # Keep the logs and images of previous checks intact.
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    failures = 0
    exchanges = 0
    for index, trace in enumerate(args.traces):
        for scale in args.scales:
            case = {"trace": str(trace.resolve()), "scale": scale, "variants": {}}
            images = {}
            for name, enabled in [("copied", "false"), ("exchanged", "true")]:
                base = args.output / f"{index:02d}_{trace.stem}_{scale}x_{name}"
                cvars = f"{CVARS};resolution_scale={scale};native_resolve_copy_free={enabled}"
                result = run_variant(str(args.replayer.resolve()), str(trace.resolve()),
                                     str(base), cvars, args.iterations, args.timeout)
                case["variants"][name] = result
                log = Path(str(base) + ".log").read_text(errors="replace")
                counts = re.findall(r"\[native-resolve-copy-free\] (\d+) resolves", log)
                result["image_exchanges"] = int(counts[-1]) if counts else 0
                exchanges += result["image_exchanges"]
                if result["ppm"]:
                    images[name] = read_ppm(result["ppm"])
                    write_png(str(base) + ".png", images[name])
            passed = all(result["rc"] == 0 and not result["errors"]
                         for result in case["variants"].values())
            if len(images) == 2:
                compared = compare(images["copied"], images["exchanged"])
                if compared is not None:
                    count, maximum, diff = compared
                    case.update(differing_pixels=count, maximum_difference=maximum)
                    passed = passed and count == 0
                    if count:
                        heat = np.zeros(images["copied"].shape, dtype=np.uint8)
                        heat[..., 0] = np.minimum(255, diff.astype(np.int32) * 8)
                        write_png(str(args.output / f"{index:02d}_{scale}x_diff.png"), heat)
                else:
                    passed = False
            else:
                passed = False
            case["passed"] = passed
            results.append(case)
            failures += not passed
            print(f"{'PASS' if passed else 'FAIL'} {trace.name} {scale}x: "
                  f"{case.get('differing_pixels', 'unknown')} differing pixels", flush=True)
            (args.output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
    if args.require_exchange and not exchanges:
        print("FAIL no image exchanges ran; these traces do not exercise that path", flush=True)
        failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
