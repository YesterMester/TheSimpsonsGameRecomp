#!/usr/bin/env python3
"""Package XboxDev's unchanged Win64 extractor and its C runtime dependency."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import urllib.request
import zipfile

TAG = "build-202609111233"
URL = f"https://github.com/XboxDev/extract-xiso/releases/download/{TAG}/extract-xiso-Win64_Release.zip"
ARCHIVE_SHA256 = "fc891dfa72f08e3838a2f8b7ec34a8e4d61678da1cc8fee879677f00bf114ce3"
EXE_SHA256 = "7e97536f978735d586a2f78327aa337b9a19b3e364fa715316fb42d07d33e0b7"
RUNTIME = "extract-xiso-runtime/VCRUNTIME140.dll"
ISOLATED_EXE = "extract-xiso-runtime/extract-xiso.exe"
LICENSE = "licenses/extract-xiso/LICENSE.TXT"
MANIFEST = "extract-xiso-source.json"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require_amd64(data):
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Not a Windows executable")
    offset = struct.unpack_from("<I", data, 60)[0]
    if (offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0"
            or struct.unpack_from("<H", data, offset + 4)[0] != 0x8664
            or struct.unpack_from("<H", data, offset + 24)[0] != 0x20B):
        raise ValueError("The extractor and runtime must both be Windows x64")


def stage(output, archive, runtime):
    if sha256(archive) != ARCHIVE_SHA256:
        raise ValueError("The upstream extractor archive checksum does not match")
    with zipfile.ZipFile(io.BytesIO(archive)) as zip_file:
        # The upstream ZIP uses Windows backslashes even on other hosts. Do
        # not extract paths from it; read only the two pinned payloads.
        files = {}
        for name in zip_file.namelist():
            basename = name.replace("\\", "/").split("/")[-1]
            if basename in ("extract-xiso.exe", "LICENSE.TXT"):
                if basename in files:
                    raise ValueError("Duplicate upstream archive member")
                files[basename] = zip_file.read(name)
    exe = files["extract-xiso.exe"]
    if sha256(exe) != EXE_SHA256 or not files["LICENSE.TXT"]:
        raise ValueError("The upstream extractor or its license is invalid")
    dll = Path(runtime).read_bytes()
    require_amd64(exe)
    require_amd64(dll)
    output = Path(output)
    # Replace the old root EXE on updates, and run the private copy beside its
    # DLL. This keeps its C runtime out of the game's DLL search directory.
    payloads = {"extract-xiso.exe": exe, ISOLATED_EXE: exe,
                RUNTIME: dll, LICENSE: files["LICENSE.TXT"]}
    for name, data in payloads.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    manifest = {
        "project": "XboxDev/extract-xiso", "tag": TAG, "archive_url": URL,
        "archive_sha256": ARCHIVE_SHA256,
        "source_url": f"https://github.com/XboxDev/extract-xiso/tree/{TAG}",
        "extractor_is_unmodified_upstream_release": True,
        "runtime_source": "Microsoft C runtime distributed with the packaging Python installation",
        "files": {name: {"bytes": len(data), "sha256": sha256(data)} for name, data in payloads.items()},
    }
    (output / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    verify(output)
    return manifest


def verify(output):
    output = Path(output)
    manifest = json.loads((output / MANIFEST).read_text(encoding="utf-8"))
    expected_files = {"extract-xiso.exe", ISOLATED_EXE, RUNTIME, LICENSE}
    if (manifest.get("tag") != TAG or manifest.get("archive_sha256") != ARCHIVE_SHA256
            or set(manifest.get("files", {})) != expected_files):
        raise ValueError("Unexpected Windows extractor provenance")
    for name, expected in manifest["files"].items():
        data = (output / name).read_bytes()
        if len(data) != expected["bytes"] or sha256(data) != expected["sha256"]:
            raise ValueError(f"Packaged extractor file changed: {name}")
    if any(sha256((output / name).read_bytes()) != EXE_SHA256
           for name in ("extract-xiso.exe", ISOLATED_EXE)):
        raise ValueError("The packaged extractor is not the pinned upstream release")
    require_amd64((output / RUNTIME).read_bytes())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--archive", type=Path, help="Use an already downloaded, checksum-verified ZIP")
    parser.add_argument("--runtime-dll", type=Path)
    parser.add_argument("--verify", action="store_true", help="Check an existing staged package")
    args = parser.parse_args()
    if args.verify:
        manifest = verify(args.output)
    else:
        runtime = args.runtime_dll or Path(sys.base_prefix) / "vcruntime140.dll"
        if not runtime.is_file():
            parser.error("vcruntime140.dll is missing from the Windows Python installation; pass --runtime-dll")
        if args.archive:
            archive = args.archive.read_bytes()
        else:
            request = urllib.request.Request(URL, headers={"User-Agent": "TheSimpsonsGameRecomp-packaging"})
            with urllib.request.urlopen(request, timeout=60) as response:
                archive = response.read(1024 * 1024 + 1)
            if len(archive) > 1024 * 1024:
                raise ValueError("Unexpected extractor download size")
        manifest = stage(args.output, archive, runtime)
    print(f"extract-xiso {TAG}: {manifest['files']['extract-xiso.exe']['bytes']} bytes, SHA-256 {EXE_SHA256}")


if __name__ == "__main__":
    main()
