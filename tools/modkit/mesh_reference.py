"""Extract EA native-data relocation and copy/rebase without changing their operations."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (65, '__savegprlr_28', '__savegprlr_28', ()),
        (65, '__restgprlr_28', '__restgprlr_28', ()),
        (49, 'sub_82831280', 'GameMeshRelocate', ()),
        (49, 'sub_828312D0', 'GameMeshCopy',
         ('__savegprlr_28', '__restgprlr_28', 'sub_82A3CD80')),
    ]
    text = '// Original native mesh operations; memory copy is a fixture boundary.\n'
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
