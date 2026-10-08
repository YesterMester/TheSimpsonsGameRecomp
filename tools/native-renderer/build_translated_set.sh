#!/bin/sh
# Builds the translated shader set shipped in native_shaders/translated: every
# shader of a shader storage, translated at 1x and 2x draw resolution scale the
# way the runtime translates it, with the plain / level 0 texture variants of
# the pixel shaders, exported (aot_export_path) and archived.
#
# Run it on a Steam Deck with the current SDK build: the runtime serves the
# set only where the translator configuration (device features, render target
# path, translator source hash) matches the one it was made with, and
# translates at runtime everywhere else.
#
# The storage is a copy of the game's cache directory (cache/shaders/shareable/
# 45410809.*); it is appended to while loading, so the original isn't used.
# The input can be the cache root or its shaders/shareable directory.
# Any captured trace works as the frame to stop at.
#
# usage: build_translated_set.sh <cache dir> <trace.xtr> [archive]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CACHE=$(cd "$1" && pwd)
TRACE=$2
OUT=${3:-$ROOT/tools/native-renderer/translated_shaders.tar.xz}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ ! -f "$CACHE/45410809.xsh" ] && \
   [ ! -f "$CACHE/shaders/shareable/45410809.xsh" ]; then
  echo "no Simpsons shader storage found in $CACHE" >&2
  exit 1
fi

sh "$ROOT/tools/native-renderer/build_trace_reference.sh" "$WORK/trace_reference" \
  "$ROOT/tools/rexglue-sdk/out/linux-amd64"
for scale in 1 2; do
  rm -rf "$WORK/cache"
  if [ -f "$CACHE/45410809.xsh" ]; then
    mkdir -p "$WORK/cache/shaders"
    cp -r "$CACHE" "$WORK/cache/shaders/shareable"
  else
    cp -r "$CACHE" "$WORK/cache"
  fi
  if ! TRACE_SHADER_STORAGE="$WORK/cache:45410809" \
    TRACE_CVARS="resolution_scale=$scale;aot_shader_path=$WORK/none;aot_export_path=$WORK/set/translated;aot_export_storage=true" \
    "$WORK/trace_reference" "$TRACE" "$WORK/frame.ppm" > "$WORK/export_$scale.log" 2>&1; then
    echo "export at scale $scale failed, see the log:" >&2
    tail -20 "$WORK/export_$scale.log" >&2
    exit 1
  fi
  if grep -a -q '\[error\]\|could not set cvar' "$WORK/export_$scale.log" || \
    ! grep -a -q "exported [1-9][0-9]* shader translations" "$WORK/export_$scale.log"; then
    echo "export at scale $scale failed, see the log:" >&2
    tail -20 "$WORK/export_$scale.log" >&2
    exit 1
  fi
  grep -a -o "exported [0-9]* shader translations" "$WORK/export_$scale.log"
done
tar -C "$WORK/set" -cf - translated | xz -9 > "$OUT"
echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
