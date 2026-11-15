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

With --elide-msr-fences it also drops the sequentially consistent fence the
recompiler emits before every mfmsr and mtmsrd. Those instructions only read
or set the interrupt-enable bit; on the console they are not memory barriers,
and the game orders memory with explicit sync/lwsync/eieio, which this does not
touch (rex/ppc/barriers.h: real fences on ARM64, compiler fences on x86, whose
ordering already covers them). Guest atomics (lwarx/stwcx.) are full-barrier
compare-and-swaps. The global lock that mtmsrd takes and releases already
orders the locked section (acquire on entry, release on exit). In heavy scenes the game's
graphics library toggles interrupts around every resource lock, and these two
full fences per section were most of those functions' time.

register_locals.py --out DIR [--stamp FILE] [--elide-msr-fences] GENERATED.cpp ...
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


MSR_COMMENT = re.compile(r'^\t// (mfmsr|mtmsrd)\b')
SEQ_CST_FENCE = '\tstd::atomic_thread_fence(std::memory_order_seq_cst);'


def elide_msr_fences(text):
    """Drops the fence line right after each mfmsr/mtmsrd comment; returns (text, sites, elided)."""
    lines = text.split('\n')
    out, sites, elided = [], 0, 0
    i = 0
    while i < len(lines):
        out.append(lines[i])
        if MSR_COMMENT.match(lines[i]):
            sites += 1
            if i + 1 < len(lines) and lines[i + 1] == SEQ_CST_FENCE:
                elided += 1
                i += 2
                continue
        i += 1
    return '\n'.join(out), sites, elided


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
    parser.add_argument('--elide-msr-fences', action='store_true')
    parser.add_argument('units', nargs='+')
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    outside = []
    sites = elided = 0
    for unit in args.units:
        unit = Path(unit)
        text = unit.read_text()
        if args.elide_msr_fences:
            text, unit_sites, unit_elided = elide_msr_fences(text)
            sites += unit_sites
            elided += unit_elided
        write_if_changed(out / unit.name, rewrite(text, unit.name, outside))
    # A use outside a function would read the context while the functions
    # write their locals.
    if outside:
        sys.exit('register_locals.py: guest register outside a function at ' + ', '.join(outside))
    # Every site is expected to have exactly the generated fence; anything else
    # (a hand patch, a changed generator) must be looked at before eliding.
    if elided != sites:
        sys.exit(f'register_locals.py: {sites - elided} of {sites} mfmsr/mtmsrd sites lack the expected fence')
    if args.stamp:
        Path(args.stamp).touch()


if __name__ == '__main__':
    main()
