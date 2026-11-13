"""Extract reviewed native hide/show and scene-attachment routines unchanged."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (36, 'sub_82697188', 'GameHide', ()),
        (36, 'sub_826971C0', 'GameShow', ()),
        (36, 'sub_82697200', 'GameForceShow', ()),
        (36, 'sub_826993F0', 'GameAttachModel', ('sub_826AF408', 'sub_82394368')),
        (36, 'sub_826994E0', 'GameDetachModel', ('sub_826AF4B0',)),
        (37, 'sub_826AF408', 'GameAttachNode', ('__savegprlr_29', '__restgprlr_29',
            'sub_826ADD28', 'sub_826ACD38')),
        (37, 'sub_826AF4B0', 'GameDetachNode', ('sub_826ACDF0',)),
        (65, '__savegprlr_29', 'GameSave29', ()),
        (65, '__restgprlr_29', 'GameRestore29', ()),
    ]
    text = '// Extracted unchanged from the reviewed generated game routines.\n'
    for number, symbol, name, allowed in routines:
        path = source.with_name(f'simpsons_recomp.{number}.cpp')
        digest, body = extract(path, symbol, name, allowed, allow_indirect=True)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
