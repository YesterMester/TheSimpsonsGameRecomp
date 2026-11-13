#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/tools/bench/out/audio_realtime_check}
exec sh "$ROOT/tools/bench/build_audio_shutdown_check.sh" "$OUT" \
    "$ROOT/tools/bench/audio_realtime_check.cpp"
