#!/usr/bin/env python3
"""Check that linked native GPU hooks really replace the generated functions.

A source wrapper can compile successfully under a C++-mangled name while the
game still uses the original C symbol. Inspect the final artifact and require
each handwritten GPU hook to be strong, unmangled and distinct from its original.
This is a linkage check, not a rendering or native-coverage measurement.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check(root, binary, nm):
    files = sorted((root / "simpsons/src").glob("native_gpu_*.cpp"))
    sources = {str(p.relative_to(root)): sha(p) for p in files}
    hooks = {}
    for source in files:
        for name in re.findall(r"^REX_FUNC\((sub_[0-9A-F]{8})\)\s*\{", source.read_text(), re.M):
            if name in hooks:
                raise RuntimeError("Duplicate native GPU hook definition: " + name)
            hooks[name] = str(source.relative_to(root))
    if not hooks:
        raise RuntimeError("No handwritten native GPU hooks found")
    binary_hash = sha(binary)
    result = subprocess.run([str(nm), "--defined-only", "--format=posix", str(binary)],
                            capture_output=True, text=True, check=True)
    symbols = {}
    wanted = set(hooks) | {"__imp__" + name for name in hooks}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            if fields[0] not in wanted and not re.match(r"_Z\d+sub_[0-9A-F]{8}R", fields[0]):
                continue
            if fields[0] in symbols:
                raise RuntimeError("Ambiguous linked symbol: " + fields[0])
            symbols[fields[0]] = dict(kind=fields[1], address=int(fields[2], 16))
    rows = []
    for name, source in sorted(hooks.items()):
        public, original = symbols.get(name), symbols.get("__imp__" + name)
        mangled = [s for s in symbols if re.match(r"_Z\d+" + name + r"R", s)]
        passed = bool(public and original and public["kind"] == "T" and
                      public["address"] != original["address"] and not mangled)
        rows.append(dict(name=name, source=source, public=public, original=original,
                         mangled=mangled, passed=passed))
    if sha(binary) != binary_hash or sources != {str(p.relative_to(root)): sha(p) for p in files}:
        raise RuntimeError("Inputs changed during the linked-hook audit")
    return dict(passed=all(p["passed"] for p in rows), binary=str(binary),
                sha256=binary_hash, hook_count=len(rows), hooks=rows, source_hashes=sources,
                rendering_qualified=False, full_game_native_complete=False)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path,
                        help="Unstripped or debug-stripped executable retaining its symbol table")
    parser.add_argument("--nm", type=Path, default=root / "tools/clang20/bin/llvm-nm")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = check(root, args.binary.resolve(), args.nm.resolve())
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(passed=report["passed"], hook_count=report["hook_count"],
                          failed_hooks=[r["name"] for r in report["hooks"] if not r["passed"]]),
                     indent=2))
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
