"""Extract the original compiled-property readers and their dependencies."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    helpers = source.with_name('simpsons_recomp.65.cpp')
    routines = [(helpers, symbol, symbol, ()) for symbol in
                ('__savegprlr_27', '__restgprlr_27', '__savegprlr_29', '__restgprlr_29')]
    routines += [
        (source.with_name('simpsons_recomp.41.cpp'), 'sub_827451C0', 'sub_827451C0', ()),
        (source, 'sub_8269A5F8', 'sub_8269A5F8',
         ('__savegprlr_27', '__restgprlr_27', 'sub_827451C0')),
        (source, 'sub_8269B588', 'GamePropertyBegin', ('sub_8269A5F8',)),
        (source, 'sub_8269B428', 'GamePropertySelect',
         ('__savegprlr_29', '__restgprlr_29', 'sub_8269A5F8')),
        (source, 'sub_8269A7A8', 'GamePropertyValue', ()),
        (source, 'sub_8269B0F8', 'GamePropertyGuid', ()),
    ]
    text = '// Unchanged original property readers; malformed records use only the host guard.\n'
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
