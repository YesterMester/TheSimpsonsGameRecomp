"""Extract the original game's logical-controller reader for the hook check."""
import argparse
from pathlib import Path

from native_reference import extract


def generate(source, output):
    digest, body = extract(source, "sub_82B766A8", "GameInputRead",
                           ("__imp__XamInputGetState",))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('// Extracted unchanged from the generated game reader.\n'
                      f'inline constexpr char kGameInputSource[] = "{digest}";\n'
                      + body + '\n', encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
