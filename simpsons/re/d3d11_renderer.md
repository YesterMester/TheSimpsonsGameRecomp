# Direct3D 11 renderer: plan and state

Target: native Direct3D 11 rendering on Windows 10/11, including feature-level
11.0 GPUs. This is a separate native API backend. It does not require Direct3D
12, D3D11On12, Vulkan or rasterizer-ordered views on Windows.

The backend is built with `REXGLUE_USE_D3D11=ON`, which the Windows release
builds use from 0.0.6.4, and selected with `gpu = "d3d11"` (the launcher's
experimental Direct3D 11 choice). Automatic selection still uses Vulkan with the
Direct3D 12 fallback. Guest draw, copy and swap submission, render target
ownership, scaled resolves, a native presenter with FXAA, CAS and FSR1, and the
UI drawer are implemented. It is experimental: it has run the game through
Proton/DXVK on a Steam Deck, but not yet on native Windows drivers, and it is
slower than Vulkan at higher render scales (see Performance).

## Implemented components

- Native hardware device creation at feature level 11.0 or 11.1, queried optional
  features, explicit WARP selection for qualification, and completion queries.
- A flip-discard swapchain with frame latency control, optional tearing and
  resize handling that releases the old backbuffer references first.
- Conversion of the existing DXBC compiler's bindful SM 5.1 programs to native
  SM 5.0 bindings. Constant layouts, shader signatures, ALU and fetch instructions
  are preserved. Resource declarations, register references and reflection are
  rebuilt together. Unsupported resource ranges and features fail explicitly.
- Immutable native shader objects with exact source comparisons before reuse.
  Failed compilation and creation are retained so they cannot repeat every draw.
- Immutable native vertex/index snapshots, with exact source comparison and an
  LRU cache. Frames can pin earlier versions after cache eviction; later source
  changes cannot replace the GPU storage used by an earlier draw.
- Guest constants use one dynamic constant buffer per block and exact size. A
  block whose bytes changed is rewritten with `WRITE_DISCARD`, which leaves queued
  draws their own copy; unchanged blocks keep their buffer and binding. Shader
  constant, fetch and context register ranges are copied as ranges, as on Vulkan.
- Game shader analysis and register-based modifications for stage linkage, clip
  planes, point size, generated pixel parameters, centroid sampling and float24
  depth conversion. Both stages are analyzed before treating shared memory as
  read-only; a shader that writes that memory prevents the SRV alias optimization.
- Shared point, rectangle and quad expansion bytecode, preserving the existing
  Direct3D 12 generator. The Direct3D 11 path creates native geometry shaders from
  it. Graphics UAV bindings reserve the four output slots used by render targets.
- Native draw submission with cached constant, resource and sampler bindings,
  immutable raster/depth/blend states, retained index buffers and explicit
  transitions between shader reads and output writes. Cached state is kept across
  draws: the shared context lock records when another thread (presentation, UI or
  a screenshot) used the context, and work on the command processor thread that
  changes bindings resets the cache itself. Unchanged outputs are not rebound. Independent subresources
  and read-only depth aspects stay usable; overlapping writes and invalid output
  dimensions are rejected before submission changes the context.
- Guest register state feeds the native draw's viewport, clipping, culling,
  polygon offsets, depth/stencil, blend factors, sample mask and color write masks.
  The same viewport conversion supplies the shader's NDC system constants.
- Native sampler objects with bounded cache ownership and retained references,
  plus fetch-constant and instruction override mapping for filtering and addresses.
- A physical-address buffer with raw and typed native views, streamed CPU page
  snapshots and GPU-written trace downloads, using the existing page tracker.
  Vertex and index pages the game rewrites every frame are uploaded on use
  without write watches, as on Vulkan.
- GPU buffer-to-image transfer using typed integer compute stores. Normalized,
  signed and float views retain the source texel bits. Mips, array slices and 3D
  slices are supported without a GPU-to-CPU-to-GPU upload path.
- Native shader variants compose guest/host texture channel mappings with each
  load or sample's original result swizzle. Reorderings and replicated channels
  add no instructions. Zero/one channels add one masked move per fetch, with
  float and integer constant types preserved. Size and LOD queries are unchanged;
  unsupported comparison/gather operations fail rather than being misinterpreted.
- A guest texture cache port creates typeless native images and signed/unsigned
  views, uses the common residency/watches, and runs the stock GPU decode shaders
  before native image transfer. It handles stored and packed mip tails, endian,
  tiling, array layers and volumes. This code cross-compiles; decoded output,
  CPU write invalidation and complete game behavior still need qualification.
  Packed 565/5551/4444 storage and 3D-as-2D views are implemented. Scaled
  resolves are stored in sparse GPU ranges and decoded into scaled textures, with
  an original-resolution copy kept for CPU, vertex and memexport reads.
  Compressed and subsampled textures use the stock GPU decompression paths
  because D3D11 cannot copy a buffer directly into an image.
- Shared render target ownership-transfer pixel bytecode, preserving the existing
  format reinterpretation, MSAA mapping, depth precision and stencil-bit paths.
  Native color and depth/stencil images, retained ownership transfers, partial
  clears, GPU EDRAM encoding and resolve copies have executed pixel checks.
  Where the GPU supports pixel shader stencil output, depth transfers write
  stencil in the same pass instead of one pass per stencil bit. Depth and stencil
  options follow the same settings as Direct3D 12 and Vulkan: float24 depth is
  converted on copy by default, keeping early depth testing.
- Native command processor, primitive conversion and packed guest constants,
  including built-in hull/domain stages and rasterization discard. Vertex
  memexport requires feature level 11.1 and currently synchronizes CPU visibility.
  Actual tessellated draws and memexport still need game qualification.
- Native gamma table and piecewise-linear gamma composition, a three-slot guest
  output mailbox, flip presentation and an immediate UI drawer. Letterboxing and
  overscan use the common layout. Normal and extreme FXAA run on the guest output;
  bilinear, CAS and FSR1 (EASU and RCAS), with or without dithering, run in
  presentation using the stock shaders.

## Performance (2026-10-07)

Steam Deck, Proton 10 with DXVK, Springfield bus stop at the start of a saved
game, FXAA and CAS on. Frame times come from the `gpu_wait_stats` log, which
also reports DX11 command processor phases and GPU time per category.

| | Before | After |
|---|---|---|
| 2x frame time | 74.6 ms (13.4 FPS) | 44.2 ms (22.6 FPS) |
| 2x DX11 command processor work | 55.2 ms | 12.7–14.0 ms |
| 2x GPU time | 69.3 ms | 44.1 ms |
| 1x frame time | not measured | 17.8–18.1 ms (55–56 FPS) |

The fixes: vertex streams use the streamed page path (7.5 MB of page uploads
and 264 write-protect changes per frame fell to about 0.4 MB and 7–30); the
command processor no longer resets the whole context before every draw;
constants use dynamic buffers with 64-bit hashing instead of new buffers;
register ranges are copied in bulk; depth transfers write stencil in one pass
instead of nine; and depth conversion follows the shared settings.

At 2x the GPU is the limit. The largest remaining costs are resolves (about 11
ms: encoding samples into the EDRAM layout, copying, and the original-resolution
copy) and decoding the resolved textures again (about 8 ms). The Vulkan path
resolves directly into textures and buffers; porting that is the next DX11
performance step.

## Checks (2026-10-07)

These hardware checks run Windows D3D11 executables through Proton/DXVK on the
Steam Deck. They check API and GPU behavior, not native Windows driver performance
or complete game rendering. The Windows WARP workflow is prepared separately and
has not run yet.

- 379 shaders extracted from the owner's executable compile at 1x and 2x: 758
  variants, with no failed translations or native vertex/pixel shader creations
  at the capped feature-level 11.0 baseline. This inventory has no memexport;
  shaders loaded later and unseen draw modifications still need coverage.
- All 122 built-in renderer and presentation helper shaders create at feature
  level 11.0: 89 compute, 17 pixel, six vertex and ten hull programs. Creation
  alone does not establish correct texture conversion or resolve execution.
- 1,200 point, rectangle and quad configurations produce exactly the same
  bytecode as the original generator. All convert and create as native geometry
  shaders at feature level 11.0; identical programs share native objects.
- Native SM 5.0 HLSL provides the independent shader-binding reference. Two
  compiler optimization settings, dynamic constant indexing, raw buffer loads,
  read-only vertex/pixel UAV aliases and native compute output are checked.
  24,576 computed words and 122,880 rendered pixels match exactly, including
  actual point, rectangle and quad draws. Seven malformed/unsupported shader
  cases are rejected; repeated failures are cached.
- The production shader preparation and shared geometry code cross-compile for
  Windows with warnings treated as errors. The installed Vulkan runtime and
  current game executable are unchanged by this work.
- Native buffer checks queue 64 changed source snapshots across raw, 16-bit index,
  32-bit index and constant buffer kinds, plus 64 GPU-to-GPU snapshots followed
  by GPU source rewrites. Both paths revisit 16 old versions after eviction.
  All 20,640 computed words are exact, including after the cache is cleared
  before GPU completion. Unchanged snapshots reuse their native object.
- Draw-context fixtures compare 64 alternating frames, independent mips/arrays,
  depth/stencil and scissor results, blending, address modes and graphics UAV data.
  151,424 rendered pixels and 768 output words match exactly, including
  rasterization discard followed by restoring the ordinary draw path. The read-only depth
  fixture writes stencil while sampling depth. Invalid draws leave the preceding
  valid state intact; the Windows debug-layer check also rejects warnings.
- Buffer-to-image transfer compares 182,850 bytes across ten native format
  families, three dimensions and two mips: 60 uploads and 180 rejected invalid
  layouts. Partial writes preserve every neighboring texel, mip and slice.
  An additional 196,608 packed texels exhaust all 65,536 input codes in each of
  565, 5551 and 4444; all normalized float components match exactly. Together
  these checks compare 3,328,578 bytes across 63 uploads. They qualify the
  transfer, not the guest decoder or a complete game frame.
- All 1,296 valid RGBA/zero/one channel mappings are checked for five float
  load/sample operations and signed/unsigned integer loads: 9,072 native draws
  and 1,388,016 exact pixel comparisons. The reference applies the mapping before
  a separate shader result swizzle and arithmetic. 1,792 color-only variants add
  no instructions; 7,280 constant-channel variants add one move. Native objects
  are reused, and 21 invalid mapping cases are rejected. This is a correctness
  check, not a game FPS benchmark or native Windows driver qualification.
- 12,096 render target transfer configurations produce byte-for-byte matches
  with the original shader algorithm at 1x/2x and with native/fallback 2x MSAA.
  All convert and create as native DX11 pixel shaders at feature level 11.0;
  identical bytecode shares 4,904 native objects. Color, depth, stencil-bit and
  retained host-depth source modes are represented. Shader creation
  alone does not prove correctness; execution is qualified separately below.
- Native render target execution covers 14 format families, 1x/2x scale, both
  native/fallback 2x MSAA and source/destination 1x/2x/4x samples. It compares
  122,572,800 pixels, including 225,792,000 exact integer components, 1,080
  ownership transfers and 504 retained snapshots. Float/normalized results use
  their specified conversion tolerance. EDRAM encoding compares 2,150,400 written
  words and 1,098,854,400 untouched words over 168 dispatches.
- Native resolve copies compare the entire destination against an independent
  CPU tiling/sample/endian reference: 725 dispatches across five scale pairs,
  all sample selections and four endian modes, including partial color/depth
  copies. All 760,217,600 bytes match; 2,175 invalid dispatches are rejected.
  The native depth path fixes a 4x MSAA copy that crossed depth-tile halves.
- Native presentation image checks execute 133 gamma/composition/UI draws and
  compare 52,992 pixels. Table gamma is exact; piecewise-linear gamma and UI
  blending allow a single output quantization step. 18 effect draws compare
  21,120 pixels of bilinear, CAS and FSR1 output against the stock shaders'
  flat-field results, computed with the same approximate reciprocals; the
  allowance is one 10-bit step for the hardware's output conversion, plus half an
  8-bit step where the effect dithers. Six FXAA passes keep flat colors and the
  two edge passes change 64 pixels.
- Render target execution at feature level 11.1 adds 72 single-pass depth and
  stencil transfers with pixel shader stencil output, compared exactly.
- The integrated game boots, plays the intro videos, shows the menus, loads
  saves and plays Springfield at 1x and 2x, including movement, camera and pause.
  Normal and extreme FXAA, CAS, and FSR1 upscaling were checked in window
  captures of live play.

## Remaining work

1. Run on native Windows drivers and the Windows WARP workflow, and qualify
   in-engine cutscenes and more of the campaign.
2. Resolve directly into native textures and buffers, as the Vulkan path does,
   to remove the EDRAM encode/decode round trip that limits 2x.
3. Qualify decoded guest textures more widely: dirty CPU inputs, signedness,
   tiling, endian, mip tails and cube/array/volume addressing.
4. Check CPU-visible GPU writes, resource aliases, memexport and tessellated
   draws with game workloads; complete EDRAM snapshot restoration for trace
   playback (it currently stops with an explicit error).
5. Check resizing, fullscreen changes and device loss with a live window.

## Tools

See [the DX11 checks](../../tools/bench/d3d11/README.md) for standalone Windows
build and qualification commands. Shader inventory tools consume the user's own
microcode; game data and captured shader inputs are not included in the repository.
