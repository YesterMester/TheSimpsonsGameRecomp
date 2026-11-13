#!/usr/bin/env python3
"""Check a staged Windows extractor against a small synthetic XISO."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import stage_windows_extractor as package


def fixture(path):
    # XDVDFS media descriptor and a balanced directory tree. These bytes are
    # test data, with no game executable or assets in them.
    sector = 2048
    image = bytearray(40 * sector)
    magic = b'MICROSOFT*XBOX*MEDIA'
    image[32 * sector:32 * sector + len(magic)] = magic
    image[33 * sector - len(magic):33 * sector] = magic
    struct.pack_into('<II', image, 32 * sector + 20, 33, 76)

    def entry(at, left, right, start, size, attrs, name):
        name = name.encode()
        struct.pack_into('<HHIIBB', image, at, left, right, start, size, attrs, len(name))
        image[at + 14:at + 14 + len(name)] = name

    payload = b'synthetic executable fixture\0not a game\n'
    note = b'Nested file extraction works.\n'
    # Root at 0, children at 20 and 48 bytes (offsets are in four-byte units).
    entry(33 * sector, 5, 12, 0, 0, 0x10, 'empty')
    entry(33 * sector + 20, 0, 0, 35, len(payload), 0x20, 'default.xex')
    entry(33 * sector + 48, 0, 0, 34, 24, 0x10, 'Nested')
    entry(34 * sector, 0, 0, 36, len(note), 0x20, 'note.txt')
    image[35 * sector:35 * sector + len(payload)] = payload
    image[36 * sector:36 * sector + len(note)] = note
    path.write_bytes(image)
    return payload, note


def check(output, runner=()):
    output = Path(output).resolve()
    package.verify(output)
    exe = output / package.ISOLATED_EXE
    env = os.environ.copy()
    # On Windows use the same app-local runtime path as the launcher. Wine
    # also accepts the native DLL via WINEDLLPATH when doing a private check.
    env['PATH'] = str(output / 'extract-xiso-runtime') + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='simpsons-xiso-check-') as temp:
        root = Path(temp)
        iso = root / 'disc with spaces.iso'
        target = root / 'installed game'
        payload, note = fixture(iso)
        result = subprocess.run([*runner, str(exe), '-x', '-d', str(target), str(iso)],
                                cwd=root, env=env, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f'extract-xiso failed ({result.returncode}): {result.stdout}\n{result.stderr}')
        assert (target / 'default.xex').read_bytes() == payload
        assert (target / 'Nested/note.txt').read_bytes() == note
        assert (target / 'empty').is_dir()
        actual = sorted(p.relative_to(target).as_posix() for p in target.rglob('*') if p.is_file())
        assert actual == ['Nested/note.txt', 'default.xex'], actual
    return {'ok': True, 'extractor_sha256': package.EXE_SHA256,
            'fixture': 'spaces in paths, file contents, nested and empty directories'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--wine', type=Path, help='Optional Wine runner for a private Linux check')
    args = parser.parse_args()
    print(json.dumps(check(args.package, [str(args.wine)] if args.wine else []), indent=2))


if __name__ == '__main__':
    main()
