"""Extract the original MMdl relocation and inverse operations unchanged."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    text = '// Extracted unchanged from the original native model routines.\n'
    for symbol, name in [('sub_82719770', 'GameModelRelocate'),
                         ('sub_82719B50', 'GameModelUnrelocate')]:
        digest, body = extract(source, symbol, name)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
