#!/bin/bash
# Build the independent pixel read-back check for the fullscreen search shader.
set -euo pipefail
task_dir="$(cd "$(dirname "$0")" && pwd)"
output_dir="${1:-$task_dir/out/shader-search-check}"
mkdir -p "$output_dir"
"${CXX:-c++}" -std=c++17 -O2 \
    -I"$task_dir/../rexglue-sdk/thirdparty/vulkan-headers/include" \
    "$task_dir/shader_search_check.cpp" -l:libvulkan.so.1 \
    -o "$output_dir/shader_search_check.next"
mv "$output_dir/shader_search_check.next" "$output_dir/shader_search_check"
glslangValidator -V --target-env vulkan1.2 \
    -o "$output_dir/shader_search_check.vert.spv" "$task_dir/shader_search_check.vert"
