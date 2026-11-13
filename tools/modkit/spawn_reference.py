"""Extract the native definition lookup and its exact GUID/hash readers."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (source, 'sub_826A1108', 'GameFindDefinition', ('sub_8269B028', 'sub_82745390')),
        (source, 'sub_8269B028', 'GameDefinitionGuid', ('sub_82745BF8',)),
        (source.with_name('simpsons_recomp.41.cpp'), 'sub_82745390', 'GameGuidHash', ()),
    ]
    text = '// Unchanged native routines; malformed inputs stay in the guarded host reader.\n'
    for path, symbol, name, allowed in routines:
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
