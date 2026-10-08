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
presentation and resize. `--fl11-0` caps the shader checks at the older hardware
baseline even when a device supports newer features. The render target check
runs at feature level 11.0 by default; `--fl11-1` lets it use pixel shader
stencil output where the device supports it, the path the game takes there.

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

The build also compiles the production shader preparation, guest texture cache,
command processor, presenter and shared geometry/ownership-transfer generators.
Native Windows drivers and more of the game still need validation. See [the
renderer state](../../../simpsons/re/d3d11_renderer.md).

The buffer check queues GPU reads of changing raw, index and constant sources,
also queues immutable GPU-to-GPU copies before rewriting their source,
forces a small cache budget, revisits pinned versions after eviction, and clears
the cache before completion. Every output word is compared with its original CPU
snapshot. Cache ownership and frame ownership are checked separately.

The draw check executes the production draw context with alternating render
targets, constants, retained 16/32-bit indices, independent mips and array slices,
scissors, depth, stencil, read-only depth alongside stencil writes, color write
masks, blending, texture address modes, rasterization discard and graphics UAV output. Conflicting
resource use and invalid index/output ranges are rejected before submission.
Cached reads are explicitly unbound when they become outputs, so later reuse of
the same view cannot silently miss a binding that the runtime removed.

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
destination addresses and source sample selection at five resolution scales,
checking every byte in the destination, including untouched neighbors. The
native depth 4x path handles vectors crossing the depth tile's swapped halves.

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
