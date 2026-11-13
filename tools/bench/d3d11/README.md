# Native Direct3D 11 checks

These checks use the Windows D3D11 API directly. They build independently of the
game; the backend itself ships as the experimental `gpu = "d3d11"` choice in the
Windows builds. WARP is an explicit
software device for Windows CI; omitting `--warp` selects a hardware adapter.

From the repository root, with a Windows C++23 compiler:

```powershell
cmake -S tools/bench/d3d11 -B tools/bench/d3d11/out -A x64
cmake --build tools/bench/d3d11/out --config Release
python tools/bench/export_d3d11_helpers.py --output tools/bench/d3d11/out/helpers
tools/bench/d3d11/out/Release/dxbc_geometry.exe tools/bench/d3d11/out/geometry
tools/bench/d3d11/out/Release/d3d11_check.exe --output d3d11-device.json
tools/bench/d3d11/out/Release/jump_context_check.exe --output jump-context-windows.json
tools/bench/d3d11/out/Release/clock_ratio_check.exe --output clock-ratios-windows.json
tools/bench/d3d11/out/Release/graphics_resource_check.exe --output graphics-resource-windows.json
tools/bench/d3d11/out/Release/bytes_equal_check.exe --output bytes-equal-windows.json
tools/bench/d3d11/out/Release/bytes_equal_avx2_check.exe --output bytes-equal-avx2-windows.json
tools/bench/d3d11/out/Release/d3d11_shader_check.exe --fl11-0 --geometry-directory tools/bench/d3d11/out/geometry --output d3d11-shaders.json
tools/bench/d3d11/out/Release/d3d11_kernel_check.exe --fl11-0 --kernel-directory tools/bench/d3d11/out/helpers --output d3d11-helpers.json
tools/bench/d3d11/out/Release/d3d11_kernel_check.exe --fl11-0 --kernel-directory tools/bench/d3d11/out/geometry --output d3d11-geometry.json
tools/bench/d3d11/out/Release/d3d11_buffer_check.exe --output d3d11-buffers.json
tools/bench/d3d11/out/Release/d3d11_draw_check.exe --output d3d11-draws.json
tools/bench/d3d11/out/Release/d3d11_present_check.exe --output d3d11-presentation.json
tools/bench/d3d11/out/Release/d3d11_render_target_check.exe --output d3d11-render-targets.json
tools/bench/d3d11/out/Release/d3d11_render_target_check.exe --fl11-1 --output d3d11-render-targets-fl11-1.json
tools/bench/d3d11/out/Release/d3d11_resolve_check.exe --output d3d11-resolves.json
tools/bench/d3d11/out/Release/d3d11_texture_upload_check.exe --output d3d11-texture-upload.json
tools/bench/d3d11/out/Release/d3d11_texture_swizzle_check.exe --output d3d11-texture-swizzle.json
```

Add `--warp` to the checks for the Microsoft software
driver. Add `--present` to the device check to exercise a real window, repeated
presentation, resize and six connection rebuilds while views are bound. It
compares actual swap-chain backbuffer pixels as well as checking `Present`.
`--present-mode flip-sequential` and `--present-mode discard` force the older
presentation paths; automatic mode also tries these if newer swap-chain
combinations are rejected. A failure report includes each attempted mode and
HRESULT, so a running command processor cannot hide a failed display connection.
`--fl11-0` caps the shader checks at the older hardware
baseline even when a device supports newer features. The render target check
runs at feature level 11.0 by default; `--fl11-1` lets it use pixel shader
stencil output where the device supports it, the path the game takes there.
It also checks transfers between original-resolution and scaled targets in both
directions, including differently sized MSAA layouts, partial clears and retained
snapshots. Individual host-pixel markers catch incorrect resampling phases.
Use `--mixed-only` to run that matrix while working on mixed-resolution transfers.
Direct3D 12 uses the same transfer shader generator.

The device check compares raw compute results and separately owned texture
versions against CPU references at 1x and 2x. The shader check compares native
SM 5.0 HLSL with converted SM 5.1 programs, including resource reflection,
constant layouts, computed words, rendered pixels and malformed input rejection.
With a geometry directory it executes point, rectangle and quad draws too.
The kernel check converts and creates every supplied program, reporting all
failures. Its creation checks do not prove texture or resolve output correctness.

The jump context check exercises the game's native setjmp/longjmp storage over
8,192 round trips on four threads, growing each thread's map and revisiting its
earlier keys. Windows saves XMM registers into this storage with aligned stores;
the buffer must remain 16-byte aligned inside its map node.

The clock ratio check compares native integer scaling with an independent
bit-at-a-time product and long division over 50,900 cases. It covers the
Windows QPC and nanosecond clock ratios, wide products and counter wrap.

The graphics resource check runs without a GPU or game files. It checks automatic
and explicit backend choices, queue priority fallback, adapter ordering, and
80,004 page ownership cases against individual bit reads. It covers partial
bitmap words, sparse owners, nonbinary validity flags, guarded output and invalid
ranges. Its timing output measures ownership bookkeeping on the CPU, not game FPS.
The same source builds on Linux with a C++23 compiler and the SDK include path.
It also checks 24,096 atomic upload-validity cases, including a concurrent
release/acquire handshake publishing the associated data. For a Linux race check,
compile it with `-fsanitize=thread -g -O1 -pthread` and run the resulting binary.

The build also compiles the production shader preparation, guest texture cache,
command processor, presenter and shared geometry/ownership-transfer generators.
Native Windows drivers and more of the game still need validation. See [the
renderer state](../../../simpsons/re/d3d11_renderer.md).

The optional `d3d11_native_memory_check` links the full production runtime, using
the same compiler and C++ ABI as that runtime. Configure
`REXGLUE_RUNTIME_IMPORT_LIBRARY` with its Windows import library (`.lib` or
`.dll.a`), build that target, and place its runtime DLL and dependencies beside
the executable. Run it with `--output native-memory.json`, adding `--warp` for
the Windows software device. It checks native page ownership, deferred mirror
allocation, partial materialization, CPU faults through guest virtual aliases,
overlapping GPU writes, a CPU write injected during publication, trace download
and cache clearing. It also composes three disjoint GPU owners and two CPU gaps
into resolve preimages and texture inputs, requires that no mirror is allocated,
and refreshes a CPU-invalidated page without losing adjacent GPU data. Unretained
GPU writes must remain on the compatibility path. A later compatibility read
must upload CPU gaps previously used only in a temporary native input. It fails
if bytes or preserved padding change.

Index-cache checks require contained reads and frequently reused versions to
avoid another GPU download. They cover page boundaries, eviction pressure,
CPU and GPU invalidation, and immutable bytes held by an earlier caller.
Prepared native outputs must survive intervening allocations and cache clearing
until their caller publishes the write. The fixture reports `index_cache_checks`,
`index_cached_reads` and `retained_output_checks` separately.

Five additional extent checks use a real retained GPU owner. They verify valid
vertex reads beyond the declared stream length, refusal to estimate from stale
CPU vertices or indices, and recovery after CPU ownership returns. These checks
must not allocate the physical memory mirror or publish GPU bytes into CPU memory.

The optional `d3d11_native_texture_check` uses the same runtime import library
and DLL setup. Run it with `--output native-texture.json` (or add `--warp`). It
loads textures through the production cache with a GPU-owned base and CPU mips,
then reverses those owners. It compares all seven native image levels before
and after CPU and GPU rewrites, covering tiled/linear storage, packed/unpacked
mips and all four endian modes. The 32 cases decode 96 images and compare
2,097,024 bytes exactly. The physical-address buffer must remain unallocated;
`native-texture-coverage.json` records the strict dependency check.

The buffer check queues GPU reads of changing raw, index and constant sources,
also queues immutable GPU-to-GPU copies before rewriting their source,
forces a small cache budget, revisits pinned versions after eviction, and clears
the cache before completion. Every output word is compared with its original CPU
snapshot. Cache ownership and frame ownership are checked separately.
Multipart CPU inputs must share the contiguous content cache, compare all input
bytes and zero padding, obey its byte budget, and leave queued versions immutable.
The fixture also covers malformed spans and the short and long hash paths.
The game command processor defaults to a 32 MiB combined CPU/GPU vertex/index
content budget; `d3d11_buffer_cache_mb` selects 1–128 MiB on startup. Compare
budgets with identical saved-frame replay counts and inspect the resource upload
counters as well as timing. The fixture deliberately uses much smaller explicit
budgets to exercise eviction and retained versions.
Scaled resolve checks also retain read/write views, expand their allocations,
evict cached views and verify that queued GPU versions remain independent.

The byte comparison check needs no GPU. It compares an independent scalar
reference with native buffer checks across aligned and unaligned ranges,
mutations and protected page boundaries. Windows baseline and AVX2 checks each
run 774,242 comparisons; Linux uses its optimized host `memcmp` and passes the
same checks in AVX2 and NoAVX2 builds. Compile `tools/bench/bytes_equal_check.cpp` with the SDK's
`include` and `thirdparty/simde` include paths and `-march=x86-64-v3` or
`-march=x86-64-v2` to check either path outside the Windows fixture project.

The draw check executes the production draw context with alternating render
targets, constants, retained 16/32-bit indices, independent mips and array slices,
scissors, depth, stencil, read-only depth alongside stencil writes, color write
masks, blending, texture address modes, rasterization discard and graphics UAV output. Conflicting
resource use and invalid index/output ranges are rejected before submission.
Discard checks alternate vertex-only and geometry pipelines, checking vertex
and geometry execution counters while every render target pixel stays unchanged.
Cached reads are explicitly unbound when they become outputs, so later reuse of
the same view cannot silently miss a binding that the runtime removed.

Constant upload checks queue 2,049 draws on the exact-size discard path, then
another 2,049 when the device supports offset bindings and constant-buffer
`NO_OVERWRITE`. They compare every output word after multiple append-page wraps,
including unchanged inputs, rounded zero padding, dynamic out-of-range reads,
switches between whole-buffer and ranged bindings, trailing unbinds and state
invalidation. Misaligned, oversized and incomplete windows must be rejected.
Large payloads retain the exact-size path, and invalid payloads must fail.
The fixture uses 16 KiB pages to exercise wraps frequently; the game uses 64 KiB
pages within a 4 MiB total append-page budget. A device without both optional
features still runs the exact-size checks. The fixture reports upload modes
separately; these counts are not gameplay FPS measurements.

The texture upload check executes the production GPU buffer-to-image transfer
for ten integer storage families, two mips and 2D, array and 3D views. Arbitrary
source bits, odd pitches, unaligned buffer offsets and partial writes are compared
byte-for-byte, including untouched neighboring texels and slices. This transfer
does not read texture data back to the CPU in the game; readback is used only by
the check. Guest texture decoding and ownership still need separate validation.

The texture swizzle check compares all 1,296 legal channel mappings for ordinary
loads, samples, explicit LOD, gradients and bias, plus signed and unsigned integer
loads. A CPU oracle applies RGBA/zero/one selection before the shader's separate
result swizzle and arithmetic. Pure channel reorderings add no instructions;
constant mappings add one masked move. This checks 9,072 draws at feature level
11.0 and retains each native variant for repeated use. Guest texture decoding
and game rendering are separate checks.

The presentation check exercises the native frame compositor and UI shader,
with three separately owned images, 64 alternating frames, channel mapping,
both guest gamma ramps, retained indices, alpha blending, scaled UI coordinates
and scissoring. It also runs every presentation effect (bilinear, CAS sharpen
and resample, FSR1 EASU and RCAS, with and without dither) and both FXAA
qualities. Flat colors are compared with the stock shaders' own approximate
normalization, computed in the check, so the allowance only covers output
conversion and dither; FXAA must change a diagonal edge. It does not qualify the
complete window/presenter lifecycle.

The render target check executes color/depth/stencil transfers, retained source
snapshots, integer and partial clears, and native EDRAM encoding across guest
formats and sample conventions. With `--fl11-1` on a device with pixel shader
stencil output, depth transfers are also checked in a single pass. The resolve check independently computes tiled
destination addresses and source sample selection at 1x, 2x, 3x and asymmetric scales,
checking every byte in the destination, including untouched neighbors. The
native depth 4x path handles vectors crossing the depth tile's swapped halves.
Full 3x, 2x3 and 3x2 cases also cover native integer sources, array slices,
partial regions, multisample conversion and the original-resolution output.

The texture upload check also tests every 16-bit code in the packed 565, 5551
and 4444 families. Native float storage preserves their source channel precision;
normalized lookup constants avoid driver-dependent reciprocal rounding. Mip and
volume decoding through the game cache still need separate validation.

## Shader inventory from a local game installation

On Linux, the compiler tool links against a source-built SDK runtime:

```sh
python3 tools/shaderkit/build_dxbc_translate.py --output /tmp/dxbc_translate
/tmp/dxbc_translate /path/to/own/ucode /tmp/d3d11-game-shaders
```

The shared render target transfer programs need no game inputs:

```sh
python3 tools/shaderkit/build_dxbc_translate.py --transfer --output /tmp/dxbc_transfer
/tmp/dxbc_transfer /tmp/d3d11-transfers
```

Pass that directory to `d3d11_kernel_check --fl11-0 --kernel-directory` on Windows.
The exporter covers color, depth, stencil-bit and preserved host-depth modes at
1x/2x, including all guest MSAA counts and the 2x-to-4x fallback. Creation checks
qualify native shader acceptance, not executed transfers or complete game frames.

Inputs are `*.vs.ucode` and `*.ps.ucode`, in big-endian order unless
`--endian little` is supplied. Output includes native `.dxbc` programs, original
`.sm51` programs and a compilation report. The inventory exporter checks shaders
without memexport; runtime preparation checks both stages of each real draw.
Do not distribute captured game inputs with the tool.

Copy the compiled output to the machine being checked and add
`--shader-directory path/to/d3d11-game-shaders` to `d3d11_shader_check`. It creates
each vertex/pixel program and compares the native cache output with the export.
This is shader coverage for that inventory, not a complete game rendering test.
