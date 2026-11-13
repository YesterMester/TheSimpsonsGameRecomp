#!/usr/bin/env python3
"""Keeps condition registers, CTR, XER and the reservation in locals.

Generated code keeps every guest register in the shared PPCContext, so the
compiler has to store each condition register field, CTR and XER write to
memory and, as guest stores may alias the context, reload them after every
guest store. None of them carries a value across calls in compiled code (the
ABI preserves cr2-cr4 for the caller, which a local does too), so each function
can keep its own: the recompiler's cr/ctr/xer/reserved_as_local options. Those
need regenerating the code, which would drop its hand patches, so this rewrites
copies of the generated units instead: ctx.cr6 becomes the local cr6, declared
at the top of each function, and so on.

register_locals.py --out DIR [--stamp FILE] GENERATED.cpp ...
Copies are only rewritten when their contents change.
"""
import argparse
import os
import re
import sys
from pathlib import Path

REGISTER = re.compile(r'\bctx\.(cr[0-7]|ctr|xer|reserved)\b')
ORDER = ['ctr', 'xer', 'reserved'] + [f'cr{i}' for i in range(8)]
DECLARATION = {'ctr': 'PPCRegister ctr{};', 'xer': 'PPCXERRegister xer{};',
               'reserved': 'PPCRegister reserved{};'}
DECLARATION.update({f'cr{i}': f'PPCCRRegister cr{i}{{}};' for i in range(8)})


def write_if_changed(path, text):
    try:
        if path.read_text() == text:
            return False
    except OSError:
        pass
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(text)
    os.replace(tmp, path)
    return True


def rewrite(text, name, outside):
    lines = text.split('\n')
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not (line.startswith('DEFINE_REX_FUNC(') and line.rstrip().endswith('{')):
            if REGISTER.search(line):
                outside.append(f'{name}:{i + 1}')
            out.append(line)
            i += 1
            continue
        end = i + 1
        while end < len(lines) and lines[end] != '}':
            end += 1
        used = set()

        def local(match):
            used.add(match.group(1))
            return match.group(1)

        body = [REGISTER.sub(local, b) if 'ctx.' in b else b for b in lines[i + 1:end]]
        at = 1 if body and 'REX_FUNC_PROLOGUE' in body[0] else 0
        body[at:at] = ['\t' + DECLARATION[r] for r in ORDER if r in used]
        out.append(line)
        out.extend(body)
        out.extend(lines[end:end + 1])
        i = end + 1
    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--stamp')
    parser.add_argument('units', nargs='+')
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    outside = []
    for unit in args.units:
        unit = Path(unit)
        write_if_changed(out / unit.name, rewrite(unit.read_text(), unit.name, outside))
    # A use outside a function would read the context while the functions
    # write their locals.
    if outside:
        sys.exit('register_locals.py: guest register outside a function at ' + ', '.join(outside))
    if args.stamp:
        Path(args.stamp).touch()


if __name__ == '__main__':
    main()
