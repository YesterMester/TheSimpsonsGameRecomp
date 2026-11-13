#!/usr/bin/env python3
"""Collect observed shader modifications for build_translated_set.sh.

Usage: collect_shader_modifications.py output.txt inventory.csv [shader dump ...]

Include a dump_shaders directory as well as shader_inventory_csv: some setup
draws translate shaders but finish before a graphics pipeline is created.
"""

import csv
from pathlib import Path
import re
import sys


def collect(paths):
    modifications = set()
    filename = re.compile(r"([0-9A-Fa-f]{16})_(vs|ps)_([0-9A-Fa-f]{16})(?:\.translated)?\.spv")

    def add(hash, stage, modification):
        if not re.fullmatch(r"[0-9A-Fa-f]{16}", hash) or not re.fullmatch(
                r"[0-9A-Fa-f]{16}", modification):
            raise ValueError(f"Invalid shader modification: {hash} {stage} {modification}")
        if int(hash, 16):
            modifications.add(f"{hash.upper()} {stage} {modification.upper()}")

    for path in paths:
        if path.is_dir():
            for file in path.rglob("*.spv"):
                match = filename.fullmatch(file.name)
                if match:
                    add(*match.groups())
        elif path.suffix == ".csv":
            with path.open(newline="") as file:
                for row in csv.DictReader(file):
                    for stage in ("vs", "ps"):
                        add(row[stage + "_hash"], stage, row[stage + "_modification"])
        else:
            match = filename.fullmatch(path.name)
            if not path.is_file() or not match:
                raise ValueError(f"Expected a shader inventory, module or dump directory: {path}")
            add(*match.groups())
    return modifications


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    try:
        modifications = collect([Path(path) for path in sys.argv[2:]])
        if not modifications:
            raise ValueError("No observed shader modifications found")
        Path(sys.argv[1]).write_text("\n".join(sorted(modifications)) + "\n")
        print(f"Collected {len(modifications)} observed shader modifications")
    except (OSError, ValueError, KeyError) as error:
        sys.exit(str(error))
