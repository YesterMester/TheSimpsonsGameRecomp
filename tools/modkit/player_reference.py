"""Extract the original human-control and input-routing readers for comparison."""
import argparse
from pathlib import Path

from native_reference import extract


def generate(source, output):
    text = '// Extracted unchanged from the generated game readers.\n'
    for symbol, name, allowed in (
        ('sub_8288DBE0', 'GameHasHumanControl', ()),
        ('sub_82884788', 'GameReadPlayerButton', ('sub_826BBC98',)),
    ):
        digest, body = extract(source, symbol, name, allowed)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
