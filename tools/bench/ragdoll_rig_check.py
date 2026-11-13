#!/usr/bin/env python3
"""Check named anatomical rig construction and affine bone reconstruction.

This is a deterministic math/definition check; it does not qualify live
character controller handover, physics collisions or recovery animations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = ROOT / 'tools/rexglue-sdk'
    includes = [ROOT / 'simpsons/src', sdk / 'include']
    includes += [sdk / 'thirdparty' / path for path in
                 ['simde', 'fmt/include', 'spdlog/include', 'sdl3/include', 'vulkan-headers/include']]
    sources = [ROOT / 'simpsons/src/entity_transform.cpp',
               ROOT / 'simpsons/src/native_ragdoll_rig.cpp',
               Path(__file__).with_suffix('.cpp')]
    command = [str(ROOT / 'tools/clang20/bin/clang++'), '-std=c++23', '-O2',
               '-march=x86-64', '-DSPDLOG_FMT_EXTERNAL', '-ffunction-sections',
               '-fdata-sections', '-Wl,--gc-sections']
    command += ['-I' + str(path) for path in includes]
    command += [str(path) for path in sources] + ['-o', str(output / 'ragdoll-rig-check')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(output / 'ragdoll-rig-check')], capture_output=True, text=True)
    report = {'scope': 'Biped definitions and affine reconstruction, not live gameplay',
              'command': command, 'exit_code': result.returncode,
              'stdout': result.stdout, 'stderr': result.stderr,
              'sources': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in sources}}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit(result.stderr or result.returncode)


if __name__ == '__main__':
    main()
