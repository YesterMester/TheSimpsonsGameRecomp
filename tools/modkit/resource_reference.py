"""Extract original native resource GUID lookup and hashing unchanged."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (40, 'sub_82711460', 'GameResource', ('sub_82710EB0',)),
        (40, 'sub_82710EB0', 'GameResourceLookup', ('sub_82745390',)),
        (41, 'sub_82745390', 'GameGuidHash', ()),
    ]
    text = '// Extracted unchanged from the reviewed generated game routines.\n'
    for number, symbol, name, allowed in routines:
        path = source.with_name(f'simpsons_recomp.{number}.cpp')
        digest, body = extract(path, symbol, name, allowed)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
