#!/bin/sh
# Builds the executable shader extractor against the source-built SDK.
# Compiler flags come from the SDK build to keep runtime struct layouts exact.
# usage: build_xex_inventory.sh [output]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$ROOT/tools/rexglue-sdk
OUT=${1:-$ROOT/tools/shaderkit/xex_inventory}
python3 - "$SDK" "$ROOT/tools/shaderkit/xex_inventory.cpp" "$OUT" <<'PY'
import json, os, pathlib, shlex, subprocess, sys, tempfile

sdk, source, output = map(pathlib.Path, sys.argv[1:])
entries = json.loads((sdk / 'build/compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('src/graphics/graphics_system.cpp'))
args = shlex.split(entry['command'])
flags, skip = [], False
for arg in args[1:]:
    if skip:
        skip = False
    elif arg in ('-o', '-c'):
        skip = True
    else:
        flags.append(arg)
output = output.absolute()
output.parent.mkdir(parents=True, exist_ok=True)
library = sdk / 'out/linux-amd64'
with tempfile.TemporaryDirectory() as temp:
    obj = pathlib.Path(temp) / 'xex_inventory.o'
    subprocess.run([args[0], *flags, '-I' + str(sdk / 'thirdparty/inja/third_party/include'),
                    '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([args[0], '-o', str(output), str(obj), '-L' + str(library),
                    '-lrexruntimerd', '-Wl,-rpath,' + str(library),
                    '-Wl,--enable-new-dtags',
                    '-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2'], check=True)
print('built', output)
PY
