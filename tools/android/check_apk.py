#!/usr/bin/env python3
"""Verify a packaged Android preview without installing it or reading game data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

from stage_native import SYSTEM_LIBS, elf_info

LIBRARIES = {"libmain.so", "libsimpsons_art.so", "librexruntime.so", "libSDL3.so", "libc++_shared.so"}
ASSETS = {"simpsons.toml", "launcher-settings.json", "touch-controls.json", "game-patches.json",
          "native-shaders-index.json", "launcher/index.html"}


def run(*command):
    environment = dict(os.environ)
    if environment.get("JAVA_HOME"):
        environment["PATH"] = str(Path(environment["JAVA_HOME"]) / "bin") + os.pathsep + environment.get("PATH", "")
    try:
        return subprocess.check_output([str(s) for s in command], text=True,
                                       stderr=subprocess.STDOUT, env=environment)
    except subprocess.CalledProcessError as error:
        raise ValueError(f"{Path(command[0]).name}: {error.output.strip()}") from error


def verify(apk, build_tools, readelf, release=False, expected_certificate=None, native_manifest=None):
    signing = run(build_tools / "apksigner", "verify", "--verbose", "--print-certs", apk)
    certificates = re.findall(r"^Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)$", signing, re.M)
    if len(certificates) != 1:
        raise ValueError("Expected one APK signing certificate")
    certificate = certificates[0].lower()
    if expected_certificate:
        expected = expected_certificate.replace(":", "").lower()
        if not re.fullmatch(r"[0-9a-f]{64}", expected) or certificate != expected:
            raise ValueError("APK signing certificate differs from the expected certificate")
    if release and re.search(r"^Signer #\d+ certificate DN: .*CN=Android Debug(?:,|$)", signing, re.M):
        raise ValueError("A release APK must use the owner's signing key")
    badging = run(build_tools / "aapt2", "dump", "badging", apk)
    if not re.search(r"^package: name='io.github.yestermester.simpsons' ", badging, re.M):
        raise ValueError("Unexpected APK application ID")
    if (not re.search(r"^(?:minSdkVersion|sdkVersion):'29'$", badging, re.M)
            or not re.search(r"^targetSdkVersion:'36'$", badging, re.M)):
        raise ValueError("Unexpected APK minimum or target Android API")
    debuggable = bool(re.search(r"^application-debuggable", badging, re.M))
    if release and debuggable:
        raise ValueError("Release APK is debuggable")
    run(build_tools / "zipalign", "-c", "-P", "16", "4", apk)
    native = {}
    with zipfile.ZipFile(apk) as archive, tempfile.TemporaryDirectory(prefix="simpsons-apk-check-") as temporary:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate APK entries")
        expected_libs = {"lib/arm64-v8a/" + name for name in LIBRARIES}
        if {name for name in names if name.startswith("lib/") and not name.endswith("/")} != expected_libs:
            raise ValueError("APK must contain exactly the five required ARM64 native libraries")
        missing = {"assets/" + name for name in ASSETS} - set(names)
        if missing:
            raise ValueError(f"APK is missing runtime assets: {sorted(missing)}")
        if any(name.lower().endswith((".xex", ".iso", ".xiso")) for name in names):
            raise ValueError("Game executable or disc image found in APK")
        for name in sorted(LIBRARIES):
            info = archive.getinfo("lib/arm64-v8a/" + name)
            if info.compress_type != zipfile.ZIP_STORED:
                raise ValueError(f"Native library is compressed: {name}")
            path = Path(temporary) / name
            path.write_bytes(archive.read(info))
            native[name] = elf_info(path)
            dynamic = run(readelf, "--dynamic", path)
            needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
            if set(needed) - LIBRARIES - SYSTEM_LIBS:
                raise ValueError(f"Unpackaged native dependency in {name}")
            native[name]["needed"] = needed
        if native_manifest:
            staged = json.loads(native_manifest.read_text())
            if set(staged) != LIBRARIES or any(staged[name]["sha256"] != native[name]["sha256"] for name in LIBRARIES):
                raise ValueError("Packaged libraries differ from the verified native staging manifest")
        shaders = json.loads(archive.read("assets/native-shaders-index.json"))
        if not shaders.get("files"):
            raise ValueError("Empty native shader index")
        for name, expected in shaders["files"].items():
            data = archive.read("assets/native_shaders/" + name)
            if len(data) != expected["size"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
                raise ValueError(f"Packaged shader differs from its content index: {name}")
    return {"apk": str(apk), "sha256": hashlib.file_digest(apk.open("rb"), "sha256").hexdigest(),
            "bytes": apk.stat().st_size, "certificate_sha256": certificate, "debuggable": debuggable,
            "release_checks": release, "native_libraries": native, "shader_files": len(shaders["files"]),
            "scope": "Package integrity, signature, assets and ELF/ZIP alignment; no phone execution", "ok": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--build-tools", required=True, type=Path)
    parser.add_argument("--readelf", default=shutil.which("llvm-readelf"))
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--expected-certificate-sha256", default=os.getenv("RECOMP_ANDROID_SIGNING_CERT_SHA256"))
    parser.add_argument("--native-manifest", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.readelf:
        parser.error("Pass the NDK's llvm-readelf with --readelf")
    try:
        report = verify(args.apk.resolve(strict=True), args.build_tools.resolve(strict=True), args.readelf,
                        args.release, args.expected_certificate_sha256, args.native_manifest)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"APK check failed: {error}\n")
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
