"""Extract unchanged native texture relocation and XG address fixups."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (16, "sub_8243FE90", "sub_8243FE90", ()),
        (83, "sub_82C20E80", "sub_82C20E80", ("sub_8243FE90",)),
        (41, "sub_82736EA8", "GameTextureRelocate", ("sub_82C20E80",)),
    ]
    text = "// Original texture and raster operations; no renderer or allocation stubs.\n"
    for number, symbol, name, allowed in routines:
        path = source.with_name(f"simpsons_recomp.{number}.cpp")
        digest, body = extract(path, symbol, name, allowed)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
