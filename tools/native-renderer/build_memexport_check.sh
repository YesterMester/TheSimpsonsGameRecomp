#!/bin/sh
# Match the runtime's compiler, headers and C++ ABI; the check uses real Vulkan.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/tools/native-renderer/out/vertex_memexport_check}
python3 - "$ROOT" "$OUT" <<'PY'
import json, os, pathlib, shlex, subprocess, sys
root = pathlib.Path(sys.argv[1])
sdk = root / 'tools/rexglue-sdk'
output = pathlib.Path(sys.argv[2]).resolve()
entry = next(e for e in json.loads((sdk / 'build/compile_commands.json').read_text())
             if e['file'].endswith('src/graphics/graphics_system.cpp'))
args = shlex.split(entry['command'])
compiler = args.pop(0)
flags = []
i = 0
while i < len(args):
    if args[i] in ('-o', '-c'):
        i += 2
    else:
        flags.append(args[i])
        i += 1
linker = pathlib.Path(compiler).parent / 'ld.lld'
if not linker.exists():
    linker = root / 'tools/clang20/bin/ld.lld'
if linker.exists():
    flags.append('--ld-path=' + str(linker))
output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([compiler, *flags, str(root / 'tools/native-renderer/vertex_memexport_check.cpp'),
                '-L' + str(sdk / 'out/linux-amd64'), '-lrexruntimerd', '-l:libvulkan.so.1',
                '-Wl,-rpath,' + str(sdk / 'out/linux-amd64'), '-Wl,--enable-new-dtags',
                '-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2', '-o', str(output)],
               cwd=entry['directory'], check=True)
print('built', output)
PY
