#!/usr/bin/env python3
"""Export the SDK's built-in SM 5.1 shaders for native Direct3D 11 checks."""

import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=root / "tools/rexglue-sdk")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for subsystem in ("graphics", "ui"):
        folder = args.sdk / "src" / subsystem / "shaders/bytecode/d3d12_5_1"
        for header in sorted(folder.glob("*.h")):
            match = re.search(r"const BYTE \w+\[\]\s*=\s*\{([^}]+)\}", header.read_text())
            if not match:
                raise ValueError(f"No shader byte array in {header}")
            code = bytes(int(value.strip(), 0) for value in match[1].split(",") if value.strip())
            if code[:4] != b"DXBC" or len(code) < 32:
                raise ValueError(f"Invalid shader container in {header}")
            name = f"{subsystem}_{header.stem}.sm51"
            (args.output / name).write_bytes(code)
            manifest.append({"source": str(header.relative_to(args.sdk)), "file": name,
                             "bytes": len(code), "sha256": hashlib.sha256(code).hexdigest()})
    if not manifest:
        raise ValueError("The SDK has no built-in SM 5.1 shaders")
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Exported {len(manifest)} SDK shaders to {args.output}")


if __name__ == "__main__":
    main()
