#!/usr/bin/env python3
"""
xsh_inventory - read the game's shader cache and report what's in it.

This is step one of taking the GPU native: knowing which shaders have been
collected. The engine (Xenia-derived) already writes every unique
Xenos shader it translates into a cache file on disk, keyed by a hash of the
shader's microcode. That file is a ready-made, real-world corpus of the
game's shaders. It covers the scenes played so far; unseen levels, effects and
shader variants can add more programs.

An offline shader recompiler (the GPU equivalent of what XenonRecomp does for
the CPU) would translate each of these microcode blobs to native shader code
ahead of time, so the engine never has to translate at runtime. This tool
reads the cache, tells you how many unique shaders it contains and how they split
between vertex and pixel, and can dump each one's raw microcode out for that
recompiler to consume.

The cache file is `<title_id>.xsh` under the engine's cache/shaders/shareable
folder. Format (little-endian), read straight from the engine's own writer
(rexglue-sdk vulkan/pipeline_cache.h, ShaderStoredHeader):

    file header:  u32 magic 'XESH' (0x48534558), u32 version (byte-swapped)
    then repeated:
      u64 ucode_data_hash              (XXH3 of the microcode)
      u32 packed: bits 0..30 = dword count, bit 31 = type (0=vertex,1=pixel)
      <dword count * 4> bytes of microcode
"""

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

MAGIC = 0x48534558           # 'XESH' little-endian
KVERSION = 0x20201219        # ShaderStoredHeader::kVersion (stored byte-swapped)


def _default_cache():
    """Where the engine keeps its shader cache, per platform."""
    home = Path.home()
    candidates = [
        home / ".local/share/simpsons/cache/shaders/shareable",
        home / ".var/app/com.visualstudio.code/data/simpsons/cache/shaders/shareable",
    ]
    for c in candidates:
        if c.is_dir():
            xsh = list(c.glob("*.xsh"))
            if xsh:
                return xsh[0]
    return None


def parse_xsh(path):
    """Yield (hash, shader_type, ucode_bytes) for every shader in the cache."""
    data = Path(path).read_bytes()
    if len(data) < 8:
        raise ValueError("file too small to be a shader cache")
    magic, version_swapped = struct.unpack_from("<II", data, 0)
    if magic != MAGIC:
        raise ValueError(f"not a shader cache (magic {magic:#010x}, expected {MAGIC:#010x})")
    # version is stored byte-swapped; swap back to compare
    version = struct.unpack("<I", struct.pack(">I", version_swapped))[0]
    if version != KVERSION:
        print(f"warning: cache version {version:#010x} != expected {KVERSION:#010x} "
              f"(format may have shifted)", file=sys.stderr)
    off = 8
    n = len(data)
    while off + 12 <= n:
        ucode_hash, packed = struct.unpack_from("<QI", data, off)
        off += 12
        dword_count = packed & 0x7FFFFFFF
        shader_type = (packed >> 31) & 1
        byte_count = dword_count * 4
        if off + byte_count > n:
            print(f"warning: truncated shader record at offset {off} "
                  f"(wanted {byte_count} bytes, {n - off} left)", file=sys.stderr)
            break
        ucode = data[off:off + byte_count]
        off += byte_count
        yield ucode_hash, shader_type, ucode


def cmd_report(args, path):
    shaders = list(parse_xsh(path))
    if not shaders:
        print("The cache is empty - no shaders recorded yet. Play the game a bit "
              "(or a scene you care about) so the engine records its shaders, then "
              "run this again.")
        return
    by_hash = {}
    for h, t, u in shaders:
        by_hash[h] = (t, u)          # dedupe by hash (the cache shouldn't dupe, but be safe)
    types = Counter(t for (t, _u) in by_hash.values())
    sizes = sorted(len(u) for (_t, u) in by_hash.values())

    print(f"Shader cache: {path}")
    print(f"  file size:        {Path(path).stat().st_size:,} bytes")
    print()
    print(f"  unique shaders:   {len(by_hash)}")
    print(f"    vertex shaders: {types.get(0, 0)}")
    print(f"    pixel shaders:  {types.get(1, 0)}")
    print()
    total = sum(sizes)
    print(f"  microcode total:  {total:,} bytes ({total // 4:,} dwords)")
    print(f"  per-shader size:  min {sizes[0]}  median {sizes[len(sizes)//2]}  "
          f"max {sizes[-1]} bytes")
    print()
    print("  This is the collected set an offline shader recompiler can translate.")
    print("  More scenes and shader variants may add programs to the cache.")
    print("  Run with `extract` to write each shader's microcode out for it.")


def cmd_extract(args, path):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    seen = set()
    count = 0
    for h, t, ucode in parse_xsh(path):
        if h in seen:
            continue
        seen.add(h)
        kind = "vs" if t == 0 else "ps"
        name = f"{h:016x}.{kind}.ucode"
        (out / name).write_bytes(ucode)
        count += 1
    print(f"Extracted {count} unique shader microcode blobs to {out}/")
    print("Each is a raw Xenos microcode blob named <hash>.<vs|ps>.ucode -")
    print("the input an offline Xenos->SPIR-V/DXBC recompiler consumes, one shader per file.")


def cmd_list(args, path):
    for h, t, ucode in parse_xsh(path):
        print(f"{h:016x}  {'vertex' if t == 0 else 'pixel '}  {len(ucode):>6} bytes")


def resolve_cache(args):
    if args.cache:
        p = Path(args.cache)
        if p.is_dir():
            xsh = list(p.glob("*.xsh"))
            if not xsh:
                sys.exit(f"no .xsh shader cache found in {p}")
            return xsh[0]
        return p
    p = _default_cache()
    if not p:
        sys.exit("couldn't find a shader cache automatically. Point me at one with "
                 "--cache <path to .xsh or its folder>. It lives under the engine's "
                 "cache/shaders/shareable directory after you've run the game.")
    return p


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="xsh_inventory",
        description="Inventory and extract the game's Xenos shaders from the engine's cache.")
    ap.add_argument("--cache", help="path to a .xsh file or the folder holding it "
                                     "(found automatically by default)")
    sub = ap.add_subparsers(dest="command")
    sub.add_parser("report", help="summarize the shader set (default)")
    sub.add_parser("list", help="list every shader: hash, type, size")
    e = sub.add_parser("extract", help="write each shader's raw microcode to files")
    e.add_argument("--out", default="shaders_ucode", help="output folder (default: shaders_ucode)")
    args = ap.parse_args(argv)
    path = resolve_cache(args)
    {"list": cmd_list, "extract": cmd_extract}.get(args.command, cmd_report)(args, path)


if __name__ == "__main__":
    main()
