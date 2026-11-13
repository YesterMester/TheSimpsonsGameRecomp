# Direct3D 11 renderer: plan and state

Target: native Direct3D 11 rendering on Windows 10/11, including feature-level
11.0 GPUs. This is a separate native API backend. It does not require Direct3D
12, D3D11On12, Vulkan or rasterizer-ordered views on Windows.

The backend is built with `REXGLUE_USE_D3D11=ON`, which the Windows release
builds use from 0.0.6.4, and selected with `gpu = "d3d11"` (the launcher's
experimental Direct3D 11 choice). Automatic selection tries Vulkan, then Direct3D
12 and Direct3D 11 when the earlier backend cannot initialize. Explicit choices
stay explicit. Guest draw, copy and swap submission, render target
ownership, scaled resolves, a native presenter with FXAA, CAS and FSR1, and the
UI drawer are implemented. It is experimental: it has run the game through
Proton/DXVK on a Steam Deck, but not yet on native Windows drivers, and it is
slower than Vulkan at higher render scales (see Performance).

The October 16 performance work batches native resolve page publication under
one ownership lock and avoids an extra temporary allocation and copy for aligned
immutable uploads. See [renderer performance](renderer_performance.md) for the
shared Vulkan/DX12 changes, Windows startup fixes and qualification limits.

The October 16 native work adds bounded fractional/reversed inputs, larger
immutable GPU index snapshots, retained owners across cache clearing, sparse and
wrapped resolve aliases, and colour/depth packing across the supported format
families. Original-resolution depth and scaled colour targets transfer in both
directions. DX11 and DX12 now use the same transfer shader generator. Its
patterned mixed-resolution fixture passes 594 cases through Proton, including
MSAA, stencil and individual host-pixel markers. Fresh attack, overlapping-resolve,
Eighty Bites and menu frames match at 1x/2x with zero compatibility dependencies
or allocations. See [the shared native state](native_renderer_plan.md) for the
scope and remaining limits; this does not qualify native Windows drivers.

The input proof also removes the earlier 22-bit index cutoff. Six large-index
frame comparisons pass at 1x/2x, including indices near the 24-bit limit. Shared
draw-extent estimation preserves valid reads beyond the declared stream length
and avoids stale CPU data when vertices or DMA indices have GPU owners. Five
focused extent/ownership checks pass alongside the 39-transition native-memory
fixture; CPU rewrites restore accurate estimation without allocating a mirror.

## Implemented components

- Native hardware device creation at feature level 11.0 or 11.1, queried optional
  features, explicit WARP selection for qualification, and completion queries.
- Presentation negotiates flip-discard, flip-sequential and discard swap chains
  when an older driver rejects a newer mode or flag combination. Waitable
  frame latency and tearing are used where accepted; older paths retain a
  bounded queue with synchronized presentation. Resize/reconnect release bound
  backbuffer references and finish deferred destruction before recreating the
  chain. Failure logs retain attempted modes and HRESULTs, and device logs
  include the selected GPU and feature level. The window check passes 32
  presentations, six reconnects and 491,520 exact backbuffer pixels for each
  forced mode through Proton; the reported Windows PC still needs a retest.
- Conversion of the existing DXBC compiler's bindful SM 5.1 programs to native
  SM 5.0 bindings. Constant layouts, shader signatures, ALU and fetch instructions
  are preserved. Resource declarations, register references and reflection are
  rebuilt together. Unsupported resource ranges and features fail explicitly.
- Native render-target helper HLSL compiles directly to SM 5.0 through the
  shared DX11 compiler entry point. Transfer vertices and partial colour,
  depth and stencil clears no longer require a compiler that accepts `vs_5_1`
  or `ps_5_1`. Translated guest/shared bytecode keeps its separate conversion
  path; already-native helpers do not pass through that converter.
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

## Native resource work for 0.0.7.0

Proven vertex streams now use retained, resource-local raw buffers.
The draw's index bounds, fetch expressions and stream lengths prove the exact
range before uploaded vertex fetch constants are rebased into those buffers.
GPU-written streams can compose retained native owners and CPU gaps. Proven
memory exports also write native buffers and retain their written pages for
later consumers. Unproved streams, unretained writers, shared texture-fetch
constants and modified export addresses keep the compatibility path.
Vertex and pixel fetch constants use
separate buffers so rebasing one stage cannot change the other stage's state.

Retained GPU-produced indices use compact staging snapshots instead of stale
physical memory for topology conversion and bounds checks. The immutable copy
is cached by exact range, invalidated by CPU and GPU writes, and shared with
the native vertex and export proofs. The 42-case geometry check passes with
GPU rewrites followed by CPU ownership changes, comparing 88,080,384 bytes
exactly with zero compatibility dependencies or allocations through Proton.

CPU-owned texture data is captured directly into resource-local decode inputs,
without first uploading it into the guest-address buffer. Scaled GPU textures
can decode from the existing compact resolve buffers. One growing decode
workspace and dynamic constants replace repeated intermediate resource
creation; retained destination textures remain independent.

Compatible single-sampled RGBA8/RGB10A2 native attachments now pack directly
into tiled resolve buffers. This bypasses EDRAM encoding and the subsequent
scaled downsample when format, owner, origin and scale are proven compatible.
The original-resolution copy samples the same native attachment.
Depth and stencil also pack directly from native views for D24S8 and D24FS8.
The float24 conversion, rounding and stencil byte follow the general path.
Native transfers readdress colour/depth samples when an MSAA layout changes,
and preserve depth/stencil bits when the game aliases them as RGB10A2.
`d3d11_native_depth_resolves` is enabled by default.
The host packing shaders are shared with Direct3D 12; translated guest shaders
and their hashes are unchanged.

`d3d11_native_resolve_memory` retains original-resolution results in compact GPU
buffers and is enabled by default. CPU-owned initial padding enters those
buffers directly, and repeated resolves reuse their live padding without a
mirror copy. Larger or overlapping resolves compose their padding from the
latest native owner of each page and unchanged CPU gaps. Unscaled texture inputs
can read a matching buffer or assemble those sources into a compact input.
Neither composition publishes a new owner until a resolve actually writes it.
CPU readback consumers can materialize the requested native pages directly;
unretained owners keep their compatibility handling. Trace downloads
and cache clearing preserve all remaining GPU data. CPU writes invalidate pages
independently. A CPU write during publication leaves a correct compatibility
source for the common tracker instead of losing the pending GPU version.
Native storage is bounded to 128 MiB; reclamation first preserves its live pages.
GPU-written pages without a retained native owner still use the compatibility
path. Only those pages are copied through it; adjacent native and CPU pages
stay resource-local.

The guest-address mirror and EDRAM storage are allocated only when a real
compatibility consumer needs them. `native_gpu_diagnostics` records the native
operations and remaining dependencies; actual original-resolution mirror
materialization still counts as a dependency. These changes do not establish
full native coverage or a measured game performance improvement.

The focused native-color fixture passed on the Steam Deck GPU through
Proton/DXVK: 240 dispatches and 62,914,560 exactly compared destination bytes,
including untouched padding. RGBA8/RGB10A2, all four endian modes, red/blue
swaps, original-resolution samples, 1x/2x/3x and anisotropic scales, and offset
views and compact original-resolution origins are included; 320 unsupported
rectangle/origin combinations are rejected. The full-runtime memory check passes
21 ownership transitions and 327,680 exact bytes, including real guest CPU write
faults, overlapping GPU/CPU changes, deferred mirror allocation, partial
materialization, publication races, trace download and cache clearing. Three
disjoint native owners and two CPU gaps supply a larger resolve and a texture
input without allocating the mirror. A CPU overwrite refreshes only its page;
an unretained GPU write is rejected as a native input. Temporary inputs watch
their CPU gaps without marking the mirror current; a later compatibility read
still uploads those bytes. The
production DX11 objects compile with warnings treated as errors, and the full
Windows runtime and current game link. A private 2x game run through Proton
booted, played the intro, navigated saves, loaded Springfield, accepted movement
and attack input, and paused and resumed. Menu and gameplay captures were
checked, and the runtime logged no rendering errors. Native diagnostics recorded
3,146,124 native vertex stream draws, 1,348 CPU texture uploads, 9,815 GPU texture
uploads and 13,758 direct native resolves in that run. Compatibility dependencies
also remained, as listed below; this is a bounded rendering check, not campaign
coverage or a native Windows driver test.

The compact original-resolution path, including mixed-source inputs, also passed
a new 1x Springfield run with movement, attack inputs and pause/resume, plus a
2x title run. Both closed cleanly with no renderer errors; their logs record at
least 5,632 and 768 compact-buffer resolve writes respectively. Springfield also
records native GPU and CPU parts combined without compatibility parts. These
runs qualify the enabled path through Proton/DXVK, not native Windows drivers
or every campaign effect.

The new depth packing check passes 136 GPU dispatches and 35,651,584 exact
bytes, including stencil, float24 rounding, endian variants, scaled/original
outputs and padding; 212 unproved cases are rejected. The shared vertex address
proof passes 1,440 cases and 495,840 float operations on Linux and Windows through
Proton. Four native depth-to-colour alias comparisons match the general path
at 1x/2x with zero compatibility dependencies and allocations, including
multisampled source images and quadrant patterns.

Captured-frame playback now restores snapshot sample words into a native R32
image, preserving their bits and replicating them at higher render scales.
Ordinary ownership transfers recover the formats used by later draws. This
replaces the previous explicit snapshot-restoration failure and makes frame
comparisons available for Direct3D 11 too.

Base and mip inputs may now have different owners. CPU levels use immutable
snapshots with physical write callbacks, and GPU levels keep their native
resolve sources. Later CPU writes reload only the affected levels. The production
texture cache check passes 32 cases and 96 decoded images, comparing 2,097,024
bytes exactly across both ownership directions, tiled/linear storage, packed and
unpacked mips and all endian modes. CPU rewrites and subsequent GPU rewrites are
checked separately; no compatibility buffer is allocated.

Native scaled inputs can extend their allocation to cover a sampled layout's
untouched padding, provided every GPU-written page already has a retained
scaled source. This removes a 512 MiB compatibility allocation for a 64x64
Springfield texture at 2x. The menu and Springfield captures match the general
resolve path at 1x/2x with zero compatibility dependencies or allocations. Four
more comparisons cover Eighty Bites and the original attack-flash capture at
both scales, also matching exactly with zero dependencies and allocations.
Five successive submissions of the attack capture also match the same repeated
sequence through the general path, with zero native compatibility dependencies
or allocations throughout.

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

## Checks (2026-10-08)

These hardware checks run Windows D3D11 executables through Proton/DXVK on the
Steam Deck. They check API and GPU behavior, not native Windows driver performance
or complete game rendering. Windows CI runs the component checks with Microsoft's
WARP software device; native Windows hardware and full-game qualification remain
separate.

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
- The render-target fixture also compiles native vertex, pixel and compute
  helpers through the production compiler, reflects their SM 5.0 version and
  stage, and checks compiler diagnostics and rejection of already-native input
  by the SM 5.1 converter. The October 30 helper fix passes the full fixture
  through Proton at feature levels 11.0 and 11.1: 1,980/2,340 surfaces and
  10,818/12,942 partial clears, including depth and stencil. These are local
  DXVK results, not native Windows driver qualification.
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
- The latest native resource run through startup, saves, Springfield, movement,
  attack inputs and pause/resume at 1x records zero compatibility dependencies
  and zero EDRAM or mirror allocations. A 2x title run records zero too. These
  checks ran through Proton and do not qualify the rest of the campaign.

The shared primitive work also passes 42 command-processor geometry cases on
Direct3D 11: point, rectangle, quad, fan, strip and loop paths with auto, 16-bit
and 32-bit indices, with and without vertex fetches. Each case uses two distinct
writes and accurate readback before presentation. All 88,080,384 output bytes
match the independent CPU reference, with zero compatibility dependencies or
allocations. Converted index buffers retain CPU shadows with their cache entries;
native vertex bounds no longer scan GPU upload mappings.

## Remaining work

1. Run the full game on native Windows hardware drivers, and qualify
   in-engine cutscenes and more of the campaign.
2. Expand strict game coverage of retained writers, sparse/multiple owners,
   wrapped origins, format conversion and sample averaging. The generated
   checks cover these native paths; unseen game sequences still need coverage.
3. Qualify decoded guest textures more widely: dirty CPU inputs, signedness,
   tiling, endian, mip tails and cube/array/volume addressing.
4. Cover unproved vertex fetches, unretained GPU-written indices and other writers, and
   modified memory-export destinations with exact resource ownership proofs.
   Check CPU-visible GPU writes, resource aliases, memexport and tessellated
   draws with game workloads. Native snapshot restoration now allows captured
   frame checks without allocating the EDRAM buffer; extend those checks to
   more game workloads.
5. Check resizing, fullscreen changes and device loss with a live window.

## Tools

See [the DX11 checks](../../tools/bench/d3d11/README.md) for standalone Windows
build and qualification commands. Shader inventory tools consume the user's own
microcode; game data and captured shader inputs are not included in the repository.
