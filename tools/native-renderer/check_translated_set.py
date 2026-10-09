#!/usr/bin/env python3
"""Checks that the shipped translated shader set matches the shader translator.

The game serves the set only when the translator source hash recorded in it
equals the one of the translator it was built with (cmake/translator_hash.cmake),
and otherwise translates every shader during play. This computes that hash from
the sources the same way and compares it with the archive's, so a translator
change can't silently leave a set behind that the game ignores.

usage: check_translated_set.py [translated_shaders.tar.xz]
Exits 1 on a mismatch; rebuild the set with build_translated_set.sh then.
"""
import hashlib
import re
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "tools" / "rexglue-sdk"


def translator_hash():
    # Same inputs as REX_TRANSLATOR_HASH_INPUTS in src/graphics/CMakeLists.txt.
    inputs = sorted(
        [str(p) for p in (SDK / "src/graphics/pipeline/shader").glob("*.cpp")] +
        [str(p) for p in (SDK / "include/rex/graphics/pipeline/shader").glob("*.h")] +
        [str(SDK / "src/graphics/format/ucode.cpp"),
         str(SDK / "include/rex/graphics/format/ucode.h"),
         str(SDK / "include/rex/graphics/xenos.h")])
    lines = "".join(f"{Path(p).name}:{hashlib.sha256(Path(p).read_bytes()).hexdigest()}\n"
                    for p in inputs)
    return hashlib.sha256(lines.encode()).hexdigest()[:16]


def main():
    archive = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        ROOT / "tools/native-renderer/translated_shaders.tar.xz")
    recorded = set()
    with tarfile.open(archive) as tar:
        for member in tar:
            if member.name.endswith("translator_configuration.txt"):
                text = tar.extractfile(member).read().decode()
                match = re.search(r"translator_source=([0-9a-f]+)", text)
                if match:
                    recorded.add(match.group(1))
    current = translator_hash()
    if recorded != {current}:
        print(f"{archive.name} was made by translator {', '.join(sorted(recorded)) or 'unknown'}, "
              f"the sources are translator {current}: rebuild it with "
              "tools/native-renderer/build_translated_set.sh, or the game translates every "
              "shader during play")
        return 1
    print(f"{archive.name} matches translator {current}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
