"""Extract the original projection helper and RenderWare view-window writer."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (65, '__savegprlr_28', '__savegprlr_28', ()),
        (65, '__restgprlr_28', '__restgprlr_28', ()),
        (40, 'sub_82714588', 'GameCameraProjection',
         ('__savegprlr_28', '__restgprlr_28', 'sub_823EC7C0', 'sub_823EC768')),
        (13, 'sub_823F1C98', 'GameCameraViewWindow', ('sub_823F2460',)),
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
