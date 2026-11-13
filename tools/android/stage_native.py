#!/usr/bin/env python3
"""Stage the compiled ARM64 libraries and check their loadable ELF segments."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SYSTEM_LIBS = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libvulkan.so",
               "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libOpenSLES.so", "libaaudio.so",
               "libjnigraphics.so", "libz.so"}


def elf_info(path):
    data = path.read_bytes()
    if (data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 16)[0] != 3
            or struct.unpack_from("<H", data, 18)[0] != 183):
        raise ValueError(f"Not a little-endian ARM64 ELF library: {path}")
    offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    segments = []
    for i in range(count):
        kind, _, file_offset, virtual, _, _, _, alignment = struct.unpack_from("<IIQQQQQQ", data, offset + i * entry_size)
        if kind == 1:
            if alignment < 16384 or (virtual - file_offset) % 16384:
                raise ValueError(f"ELF load segment is not 16 KB aligned: {path}")
            segments.append(alignment)
    if not segments:
        raise ValueError(f"No loadable segments: {path}")
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "load_alignments": segments}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("ndk", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    toolchain = args.ndk / "toolchains/llvm/prebuilt"
    hosts = [p for p in toolchain.iterdir() if p.is_dir()]
    if len(hosts) != 1:
        raise ValueError("Choose an NDK with one host toolchain")
    host = hosts[0]
    suffix = ".exe" if (host / "bin/llvm-readelf.exe").exists() else ""
    sources = {
        "libmain.so": args.build_dir / "libmain.so",
        "libsimpsons_art.so": args.build_dir / "libsimpsons_art.so",
        "librexruntime.so": ROOT / "tools/rexglue-sdk/out/android-arm64/librexruntime.so",
        "libSDL3.so": ROOT / "tools/rexglue-sdk/out/android-arm64/libSDL3.so",
        "libc++_shared.so": host / "sysroot/usr/lib/aarch64-linux-android/libc++_shared.so",
    }
    destination = args.output / "arm64-v8a"
    destination.mkdir(parents=True, exist_ok=True)
    report = {}
    for name, source in sources.items():
        if not source.is_file():
            raise FileNotFoundError(f"Build the complete Android game first: {source}")
        elf_info(source)
        dynamic = subprocess.check_output([str(host / f"bin/llvm-readelf{suffix}"), "--dynamic", str(source)], text=True)
        dependencies = [line.split("[", 1)[1].split("]", 1)[0] for line in dynamic.splitlines() if "(NEEDED)" in line]
        unknown = set(dependencies) - sources.keys() - SYSTEM_LIBS
        if unknown:
            raise ValueError(f"Unpackaged native dependencies in {name}: {sorted(unknown)}")
        target = destination / name
        shutil.copy2(source, target)
        subprocess.run([str(host / f"bin/llvm-strip{suffix}"), "--strip-unneeded", str(target)], check=True)
        report[name] = dict(elf_info(target), needed=dependencies)
    (args.output / "native-libraries.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
