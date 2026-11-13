#!/bin/sh
# Build the ARM64 game once, then package it with the shared launcher.
set -eu
if [ "$#" -gt 1 ]; then
    echo 'Usage: tools/android/build.sh [debug|release]' >&2
    exit 2
fi
android_variant=${1:-debug}
case "$android_variant" in
    debug) android_task=assembleDebug ;;
    release)
        android_task=assembleRelease
        : "${RECOMP_ANDROID_SIGNING_STORE_FILE:?Set the owner release keystore path}"
        : "${RECOMP_ANDROID_SIGNING_STORE_PASSWORD:?Set the release keystore password}"
        : "${RECOMP_ANDROID_SIGNING_KEY_ALIAS:?Set the release signing alias}"
        : "${RECOMP_ANDROID_SIGNING_KEY_PASSWORD:?Set the release signing key password}"
        if [ ! -f "$RECOMP_ANDROID_SIGNING_STORE_FILE" ]; then
            echo 'The owner release keystore does not exist.' >&2
            exit 2
        fi
        ;;
    *) echo 'Usage: tools/android/build.sh [debug|release]' >&2; exit 2 ;;
esac
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29}"
: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK directory}"
# Keep the development signing key consistent across terminal and sandboxed
# IDE builds. Android requires the same certificate to update an installed app.
: "${ANDROID_USER_HOME:=$HOME/.android}"
export ANDROID_USER_HOME
android_build_dir=${RECOMP_ANDROID_BUILD_DIR:-"$repo_dir/android/.cxx/native-arm64"}
android_jobs=${ANDROID_BUILD_JOBS:-2}
mkdir -p "$android_build_dir"
android_build_dir=$(CDPATH= cd -- "$android_build_dir" && pwd)
python3 - "$android_build_dir" <<'PY'
from pathlib import Path
import sys

cache = Path(sys.argv[1]) / "CMakeCache.txt"
if cache.exists():
    entries = {}
    for line in cache.read_text().splitlines():
        if "=" in line and ":" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            entries[key.split(":", 1)[0]] = value
    if entries.get("ANDROID_ABI") != "arm64-v8a":
        sys.exit("Select a separate Android ARM64 build directory; this cache belongs to another target.")
PY
cmake -S "$repo_dir/simpsons" -B "$android_build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_shared -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DREXSDK_DIR="$repo_dir/tools/rexglue-sdk" \
    -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$android_build_dir" --parallel "$android_jobs" --target simpsons simpsons_art
python3 "$repo_dir/tools/android/stage_native.py" "$android_build_dir" \
    "$ANDROID_NDK_HOME" "$android_build_dir/jniLibs"
cd "$repo_dir/android"
./gradlew "-PnativeLibraryDir=$android_build_dir/jniLibs" ":app:$android_task" --console=plain
