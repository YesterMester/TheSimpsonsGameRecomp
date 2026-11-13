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
# Pass - instead of a trace to bake only the storage without replaying a frame.
#
# Existing archive variants are retranslated too, so cache differences cannot
# silently reduce the coverage of the next shipped set.
# Additional modifications use "HASH vs|ps MODIFICATION" lines, collected by
# collect_shader_modifications.py. They add device variants without baking every
# possible primitive expansion for every vertex shader.
# usage: build_translated_set.sh <cache dir> <trace.xtr|-> [archive] [previous archive] [modifications] [runtime library directory]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CACHE=$(cd "$1" && pwd)
TRACE=$2
OUT=${3:-$ROOT/tools/native-renderer/translated_shaders.tar.xz}
PREVIOUS=${4:-$ROOT/tools/native-renderer/translated_shaders.tar.xz}
MODIFICATIONS=${5:-}
RUNTIME=${6:-$ROOT/tools/rexglue-sdk/out/linux-amd64}
WORK=$(mktemp -d)
ARCHIVE_TEMP=$(mktemp "$OUT.tmp.XXXXXX")
cleanup() {
  rm -rf "$WORK"
  rm -f "$ARCHIVE_TEMP"
}
trap cleanup EXIT

python3 - "$PREVIOUS" "$WORK/modifications.txt" "$MODIFICATIONS" <<'PY'
from pathlib import Path
import re, sys, tarfile
previous, destination = map(Path, sys.argv[1:3])
variants = set()
if previous.is_file():
    with tarfile.open(previous) as archive:
        for member in archive:
            match = re.fullmatch(r"([0-9A-Fa-f]{16})_(vs|ps)_([0-9A-Fa-f]{16})\.spv", Path(member.name).name)
            if match:
                hash, stage, modification = match.groups()
                variants.add(f"{hash.upper()} {stage} {modification.upper()}")
preserved = len(variants)
if sys.argv[3]:
    for number, line in enumerate(Path(sys.argv[3]).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        match = re.fullmatch(r"([0-9A-Fa-f]{16})\s+(vs|ps)\s+([0-9A-Fa-f]{16})", line)
        if not match:
            sys.exit(f"Invalid shader modification on line {number}: {line}")
        hash, stage, modification = match.groups()
        variants.add(f"{hash.upper()} {stage} {modification.upper()}")
destination.write_text("\n".join(sorted(variants)) + "\n")
print(f"Preserving {preserved} shader modifications; adding {len(variants)-preserved} observed variants")
PY

if [ ! -f "$CACHE/45410809.xsh" ] && \
   [ ! -f "$CACHE/shaders/shareable/45410809.xsh" ]; then
  echo "no Simpsons shader storage found in $CACHE" >&2
  exit 1
fi

if [ "$TRACE" = - ]; then
  python3 "$ROOT/tools/native-renderer/build_shader_bake.py" "$WORK/shader_bake" "$RUNTIME"
else
  sh "$ROOT/tools/native-renderer/build_trace_reference.sh" "$WORK/trace_reference" \
    "$RUNTIME" "$RUNTIME"
fi
for scale in 1 2; do
  rm -rf "$WORK/cache"
  if [ -f "$CACHE/45410809.xsh" ]; then
    mkdir -p "$WORK/cache/shaders"
    cp -r "$CACHE" "$WORK/cache/shaders/shareable"
  else
    cp -r "$CACHE" "$WORK/cache"
  fi
  if [ "$TRACE" = - ]; then
    if ! "$WORK/shader_bake" "$WORK/cache" "$WORK/set/translated" "$scale" \
      "$WORK/modifications.txt" > "$WORK/export_$scale.log" 2>&1; then
      echo "source shader bake at scale $scale failed, see the log:" >&2
      tail -20 "$WORK/export_$scale.log" >&2
      exit 1
    fi
  elif ! TRACE_SHADER_STORAGE="$WORK/cache:45410809" \
    TRACE_CVARS="resolution_scale=$scale;aot_shader_path=$WORK/none;aot_export_path=$WORK/set/translated;aot_export_storage=true;aot_export_modifications=$WORK/modifications.txt" \
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
  grep -a "\[aot-storage\]" "$WORK/export_$scale.log" || true
  grep -a -o "exported [0-9]* shader translations" "$WORK/export_$scale.log"
done
tar -C "$WORK/set" -cf - translated | xz -9 > "$ARCHIVE_TEMP"
python3 "$ROOT/tools/native-renderer/check_translated_set.py" "$ARCHIVE_TEMP"
python3 - "$PREVIOUS" "$ARCHIVE_TEMP" <<'PY'
from pathlib import Path
import sys, tarfile
def modules(path):
    with tarfile.open(path) as archive:
        return {member.name for member in archive if member.name.endswith('.spv')}
previous, current = map(Path, sys.argv[1:])
present = modules(current)
if not present:
    sys.exit("No translated shaders were exported")
missing = modules(previous) - present if previous.is_file() else set()
if missing:
    sys.exit(f"Rebuild omitted {len(missing)} previously shipped shader variants")
print(f"Verified {len(present)} modules without losing existing variants")
PY
mv "$ARCHIVE_TEMP" "$OUT"
echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
