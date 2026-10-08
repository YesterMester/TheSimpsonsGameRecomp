#!/bin/sh
# Builds trace_reference (headless GPU trace replayer) against the current SDK.
#
# The trace playback classes (TraceDump, TracePlayer, TraceReader) are not part
# of the runtime library, so they are compiled into the tool. Compiler and
# flags come from the SDK's compile_commands.json so the tool always matches
# the library it loads.
#
# usage: build_trace_reference.sh [output] [rpath]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$ROOT/tools/rexglue-sdk
OUT=${1:-$ROOT/tools/native-renderer/out/trace_reference}
RPATH=${2:-$SDK/out/linux-amd64}
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

FLAGS=$(python3 - "$SDK/build/compile_commands.json" <<'EOF'
import json, shlex, sys
for entry in json.load(open(sys.argv[1])):
    if entry["file"].endswith("src/graphics/graphics_system.cpp"):
        args = shlex.split(entry["command"])
        keep, skip = [], False
        for a in args[1:]:
            if skip:
                skip = False
                continue
            if a in ("-o", "-c"):
                skip = True
                continue
            keep.append(a)
        print(args[0])
        print(" ".join(shlex.quote(a) for a in keep))
        break
EOF
)
CXX=$(echo "$FLAGS" | head -1)
CXXFLAGS=$(echo "$FLAGS" | tail -1)

pids=""
for src in "$ROOT/tools/native-renderer/trace_reference.cpp" \
           "$SDK/src/graphics/trace_dump.cpp" \
           "$SDK/src/graphics/trace_player.cpp" \
           "$SDK/src/graphics/trace_reader.cpp"; do
  eval "\"$CXX\" $CXXFLAGS -c \"$src\" -o \"$OBJ/$(basename "$src" .cpp).o\"" &
  pids="$pids $!"
done
failed=0
for pid in $pids; do
  wait "$pid" || failed=1
done
if [ "$failed" -ne 0 ]; then
  echo "compile failed" >&2
  exit 1
fi

# Let LD_LIBRARY_PATH select the runtime for A/B runs.
LD_LIBRARY_PATH=$HOME/simpsons-build-shim${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  "$CXX" -o "$OUT" "$OBJ"/*.o -L"$SDK/out/linux-amd64" -lrexruntimerd "$SDK/out/linux-amd64/libsnappyrd.a" \
  -Wl,-rpath,"$RPATH" -Wl,--enable-new-dtags -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
echo "built $OUT (rpath $RPATH)"
