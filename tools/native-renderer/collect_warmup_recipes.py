#!/usr/bin/env python3
"""Collect first-launch metadata from owned executable and shader-cache inventories."""

import argparse
import ctypes
import ctypes.util
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True,
                        help="xex_inventory JSON identifying original executable programs")
    parser.add_argument("--shaders", type=Path, required=True, help="Validated .xsh storage")
    parser.add_argument("--pipelines", type=Path, nargs="+", required=True,
                        help="Current-format .fbo.vk.xpso storages")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    library = ctypes.util.find_library("xxhash")
    if not library:
        parser.error("The developer collector requires libxxhash (XXH3).")
    hash_bytes = ctypes.CDLL(library).XXH3_64bits
    hash_bytes.restype = ctypes.c_uint64
    hash_bytes.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    original = {int(item["hash"], 16)
                for item in json.loads(args.inventory.read_text())["containers"]}
    data = args.shaders.read_bytes()
    if data[:8] != struct.pack("<II", 0x48534558, 0x19122020):
        raise ValueError("Unsupported shader storage")
    programs = {}
    offset = 8
    while offset < len(data):
        digest, packed = struct.unpack_from("<QI", data, offset)
        words = packed & 0x7FFFFFFF
        code = data[offset + 12:offset + 12 + words * 4]
        if not 0 < words <= 0xFFFF or len(code) != words * 4 or hash_bytes(code, len(code)) != digest:
            raise ValueError("Invalid shader record")
        programs[digest] = (packed >> 31, code)
        offset += 12 + len(code)
    bases = {digest: code for digest, (stage, code) in programs.items()
             if digest in original and stage == 0}
    patches = []
    for target, (stage, code) in sorted(programs.items()):
        if target in original or stage != 0:
            continue
        candidates = [(sum(a != b for a, b in zip(source, code)), digest, source)
                      for digest, source in bases.items() if len(source) == len(code)]
        if not candidates:
            continue  # Generated programs cannot be reconstructed from these originals.
        _, digest, source = min(candidates)
        changes = []
        rebuilt = bytearray(source)
        for word in range(len(code) // 4):
            begin = word * 4
            mask = (int.from_bytes(source[begin:begin + 4], "big") ^
                    int.from_bytes(code[begin:begin + 4], "big"))
            if mask:
                changes.append(f"{word}:{mask:08X}")
                rebuilt[begin:begin + 4] = (int.from_bytes(source[begin:begin + 4], "big") ^
                                           mask).to_bytes(4, "big")
        if bytes(rebuilt) != code or hash_bytes(bytes(rebuilt), len(rebuilt)) != target:
            raise ValueError("Reconstructed vertex program does not match its identity")
        patches.append(f"{digest:016X} {target:016X} {len(code) // 4} " + " ".join(changes))

    records = {}
    version = None
    size = None
    for path in args.pipelines:
        data = path.read_bytes()
        magic, api, swapped = struct.unpack_from("<III", data)
        current_version = int.from_bytes(data[8:12], "big")
        if magic != 0x53504558 or api != 0:
            raise ValueError(f"Not a Vulkan host render-target storage: {path}")
        if version is not None and current_version != version:
            raise ValueError("Pipeline schema versions differ; do not relabel older records")
        version = current_version
        body = data[12:]
        if not body:
            continue
        sizes = [candidate for candidate in range(32, 256)
                 if len(body) % (candidate + 8) == 0 and all(
                     hash_bytes(body[o + 8:o + candidate + 8], candidate) ==
                     struct.unpack_from("<Q", body, o)[0]
                     for o in range(0, len(body), candidate + 8))]
        if len(sizes) != 1 or (size is not None and sizes[0] != size):
            raise ValueError(f"Invalid or inconsistent pipeline descriptions: {path}")
        size = sizes[0]
        for offset in range(0, len(body), size + 8):
            digest = struct.unpack_from("<Q", body, offset)[0]
            records[digest] = body[offset + 8:offset + size + 8]
    if not records or not patches:
        raise ValueError("Refusing to replace metadata with an empty collection")
    args.output.mkdir(parents=True, exist_ok=True)
    outputs = {
        "warmup-vertex-patches.txt": "REX_VERTEX_PATCHES 1\n" + "\n".join(patches) + "\n",
        "warmup-pipelines.txt": f"REX_VULKAN_PIPELINES {version:08X} {size}\n" +
            "\n".join(f"{digest:016X} {code.hex().upper()}"
                      for digest, code in sorted(records.items())) + "\n",
    }
    for name, text in outputs.items():
        destination = args.output / name
        temporary = destination.with_suffix(".tmp")
        temporary.write_text(text)
        temporary.replace(destination)
    print(f"Collected {len(patches)} vertex patches and {len(records)} pipeline recipes")


if __name__ == "__main__":
    main()
