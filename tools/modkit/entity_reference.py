"""Extract reviewed world registration, lookup, removal and destructor dispatch."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    routines = [
        ('sub_8269F990', 'GameDestroyEntity', ('sub_8268F538',)),
        ('sub_8268F538', 'GameDestroySubscriber', ()),
        ('sub_826A16F8', 'GameUnregisterEntity', ('sub_826A1008',)),
        ('sub_826A31F0', 'GameRegisterEntity', ('__savegprlr_28', '__restgprlr_28',
            'sub_826A2AE8', 'sub_826A1108', 'sub_8269B738', 'sub_826A2258', 'sub_826A3190')),
        ('sub_826A11E0', 'GameFindDirectEntity', ('sub_82745390',)),
    ]
    text = '// Extracted unchanged from the reviewed generated game routines.\n'
    for symbol, name, allowed in routines:
        digest, body = extract(source, symbol, name, allowed, allow_indirect=True)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
