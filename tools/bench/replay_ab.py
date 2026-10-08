#!/usr/bin/env python3
"""Replay GPU frame traces through renderer variants and compare the results.

Each trace is played headlessly by trace_reference (no game window), once per
variant, with the frame repeated so caches reach the state they have in a
running game. The final images are compared pixel for pixel against the first
variant, and the steady-state GPU time per category is reported.

usage:
  replay_ab.py <out_dir> <trace.xtr>... [--variant name=cvar=value;cvar=value]...
               [--iterations N] [--timeout SECONDS] [--replayer PATH]

Without --variant, compares native_resolve off ("legacy") against on ("native").
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time
import zlib

import numpy as np

DEFAULT_REPLAYER = "/home/deck/simpsons-test/trace_reference_dev"


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    pixels = np.frombuffer(parts[3], dtype=np.uint8, count=width * height * 3)
    return pixels.reshape(height, width, 3)


def write_png(path, image):
    height, width, _ = image.shape
    raw = b"".join(b"\x00" + image[y].tobytes() for y in range(height))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def run_variant(replayer, trace, out_base, cvars, iterations, timeout):
    env = dict(os.environ)
    env["TRACE_CVARS"] = cvars
    env["TRACE_BENCH"] = str(iterations)
    env["REX_GPU_PROFILE_FRAMES"] = str(max(1, iterations // 2))
    ppm = out_base + ".ppm"
    log = out_base + ".log"
    if os.path.exists(ppm):
        os.remove(ppm)
    start = time.time()
    with open(log, "wb") as log_file:
        try:
            proc = subprocess.run([replayer, trace, ppm], env=env, stdout=log_file,
                                  stderr=subprocess.STDOUT, timeout=timeout)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    elapsed = time.time() - start
    text = open(log, "rb").read().decode("utf-8", "replace")
    bench = re.findall(r"TraceDump: bench \d+ iterations - mean ([\d.]+) ms, median ([\d.]+) ms",
                       text)
    profiles = re.findall(r"\[gpu-profile\] \d+ frames: gpu frame span ([\d.]+)ms \|(.*?) \(ms",
                          text)
    errors = [line for line in text.splitlines()
              if "[error]" in line or "could not set cvar" in line][:5]
    return {
        "rc": rc,
        "elapsed": elapsed,
        "ppm": ppm if os.path.exists(ppm) else None,
        "median_ms": float(bench[-1][1]) if bench else None,
        "gpu_ms": float(profiles[-1][0]) if profiles else None,
        "categories": profiles[-1][1].strip() if profiles else "",
        "errors": errors,
    }


def compare(image_a, image_b):
    if image_a.shape != image_b.shape:
        return None
    diff = np.abs(image_a.astype(np.int16) - image_b.astype(np.int16)).max(axis=2)
    return int((diff > 0).sum()), int(diff.max()), diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out_dir")
    parser.add_argument("traces", nargs="+")
    parser.add_argument("--variant", action="append", default=[])
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--replayer", default=DEFAULT_REPLAYER)
    args = parser.parse_args()

    variants = []
    for spec in args.variant or ["legacy=native_resolve=false", "native=native_resolve=true"]:
        name, _, cvars = spec.partition("=")
        variants.append((name, cvars))
    os.makedirs(args.out_dir, exist_ok=True)

    failures = 0
    for trace in args.traces:
        trace_name = os.path.splitext(os.path.basename(trace))[0]
        print(f"== {trace_name}", flush=True)
        reference = None
        for name, cvars in variants:
            out_base = os.path.join(args.out_dir, f"{trace_name}_{name}")
            result = run_variant(args.replayer, trace, out_base, cvars, args.iterations,
                                 args.timeout)
            if result["rc"] != 0 or result["errors"]:
                failures += 1
            line = f"  {name:10s} rc={result['rc']} {result['elapsed']:.1f}s"
            if result["gpu_ms"] is not None:
                line += f" gpu {result['gpu_ms']:.2f} ms/frame"
            if result["median_ms"] is not None:
                line += f" wall {result['median_ms']:.2f} ms"
            if result["ppm"]:
                image = read_ppm(result["ppm"])
                write_png(out_base + ".png", image)
                if reference is None:
                    reference = (name, image)
                else:
                    compared = compare(reference[1], image)
                    if compared is None:
                        line += f" | size differs from {reference[0]}"
                        failures += 1
                    else:
                        count, max_diff, diff = compared
                        if count:
                            failures += 1
                            heat = np.zeros(image.shape, dtype=np.uint8)
                            heat[..., 0] = np.minimum(255, diff.astype(np.int32) * 8)
                            write_png(out_base + "_diff.png", heat)
                            line += (f" | {count} pixels differ from {reference[0]} "
                                     f"(max {max_diff})")
                        else:
                            line += f" | identical to {reference[0]}"
            else:
                line += " | no image"
                failures += 1
            print(line, flush=True)
            if result["categories"]:
                print(f"    {result['categories']}", flush=True)
            for error in result["errors"]:
                print(f"    {error[:200]}", flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
