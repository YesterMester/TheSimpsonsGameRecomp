"""Extract reviewed entity setters, controller dispatch and frame routines."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        (source.with_name('simpsons_recomp.65.cpp'), '__savegprlr_29', '__savegprlr_29', ()),
        (source.with_name('simpsons_recomp.65.cpp'), '__restgprlr_29', '__restgprlr_29', ()),
        (source, 'sub_82696D10', 'GameSetPosition', ('sub_823F2460',)),
        (source, 'sub_82697690', 'GameSetTransform',
         ('__savegprlr_29', '__restgprlr_29', 'sub_823EC410', 'sub_823F2748', 'sub_823F2460')),
        (source, 'sub_82697160', 'GameTransformChanged', ('sub_826A8708',)),
        (source.with_name('simpsons_recomp.52.cpp'), 'sub_8288D2E0',
         'GamePlayerSetPosition', ('sub_82696D10',)),
        (source.with_name('simpsons_recomp.52.cpp'), 'sub_8288D428',
         'GamePlayerSetTransform', ('sub_82697690',)),
        (source.with_name('simpsons_recomp.13.cpp'), 'sub_823EC410', 'GameVectorLength',
         ('sub_823EBF10',)),
        (source.with_name('simpsons_recomp.13.cpp'), 'sub_823F2748', 'GameFrameTransform',
         ('sub_823F41D8', 'sub_823F2460')),
        (source.with_name('simpsons_recomp.13.cpp'), 'sub_823F2460', 'GameFrameMarkDirty', ()),
        (source.with_name('simpsons_recomp.13.cpp'), 'sub_823F2540', 'GameFrameGetWorld',
         ('sub_823F24D8', 'sub_823F3B68')),
        (source.with_name('simpsons_recomp.13.cpp'), 'sub_823F24D8', 'GameFrameUpdateWorld',
         ('sub_823F24D8', 'sub_823F3B68', 'sub_82A3CD80')),
    ]
    text = '// Extracted unchanged from the reviewed generated game routines.\n'
    for path, symbol, name, allowed in routines:
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
