#!/usr/bin/env bash
# Build xenos_translate, the offline Xenos -> SPIR-V shader compiler.
#
# It reuses the engine's own shader translator, so it has to be built against
# the same source tree the runtime library was built from. Building against the
# prebuilt SDK's shipped headers while linking a differently-versioned runtime
# .so causes struct-layout skew and a crash - so this compiles against the
# rexglue-sdk SOURCE headers and links the source-built runtime library.
#
# Prerequisites: build the ReXGlue SDK from source first (the normal project
# build does this), so tools/rexglue-sdk/out/<platform>/librexruntime*.so exists.
#
# Usage:  tools/shaderkit/build.sh   [output-binary-path]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="$ROOT/tools/rexglue-sdk"
TP="$SDK/thirdparty"
CLANG="$ROOT/tools/clang20/bin/clang++"
OUT="${1:-$ROOT/tools/shaderkit/xenos_translate}"

# Locate the source-built runtime library (any config: rd/d/plain).
SOLIB_DIR="$SDK/out/linux-amd64"
RUNTIME=""
for name in librexruntimerd.so librexruntime.so librexruntimed.so; do
  [ -f "$SOLIB_DIR/$name" ] && RUNTIME="$name" && break
done
if [ -z "$RUNTIME" ]; then
  echo "error: no source-built runtime library in $SOLIB_DIR" >&2
  echo "       build the ReXGlue SDK from source first." >&2
  exit 1
fi
LINK_NAME="${RUNTIME#lib}"; LINK_NAME="${LINK_NAME%.so}"   # librexruntimerd.so -> rexruntimerd

[ -x "$CLANG" ] || { echo "error: clang not found at $CLANG" >&2; exit 1; }

echo "linking against $SOLIB_DIR/$RUNTIME"

# Compile definitions must match the SDK's own RelWithDebInfo build exactly -
# these change struct layouts, so a mismatch crashes at runtime.
"$CLANG" -std=c++23 -O2 -g -DNDEBUG -fuse-ld=lld -mcmodel=large -msse4.1 \
  -DREXGLUE_BUILD_CONFIG='"RelWithDebInfo"' \
  -DREXGLUE_ENABLE_PERF_COUNTERS -DREXGLUE_ENABLE_PROFILING -DREX_HAS_VULKAN=1 \
  -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL \
  -DTRACY_DELAYED_INIT -DTRACY_ENABLE -DTRACY_FIBERS -DTRACY_IMPORTS \
  -DTRACY_MANUAL_LIFETIME -DTRACY_ON_DEMAND \
  -I "$SDK/include" \
  -I "$TP/glslang" -I "$TP/renderdoc" -I "$TP/volk" \
  -I "$TP/vulkan-headers/include" -I "$TP/vulkan-memory-allocator/include" \
  -I "$TP/tracy/public" -I "$TP/fmt/include" -I "$TP/spdlog/include" \
  -I "$TP/simde" -I "$TP/disruptorplus/include" -I "$TP/xxHash" \
  "$ROOT/tools/shaderkit/xenos_translate.cpp" \
  -L "$SOLIB_DIR" -Wl,-rpath,"$SOLIB_DIR" -l"$LINK_NAME" \
  -o "$OUT"

echo "built $OUT"
echo "run:  LD_LIBRARY_PATH=$SOLIB_DIR $OUT <shader.vs.ucode> <out.spv>"
