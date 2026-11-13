# Native renderer: plan and state

Goal: render the whole game natively on Vulkan (Linux / Steam Deck, Windows and
Android), Direct3D 12 and Direct3D 11. Each backend must preserve game-visible
rendering and synchronization while removing its EDRAM emulation and full
physical-address memory mirror. Native caches still own and reuse host resources;
their remaining compatibility dependencies must be measured explicitly.

## 0.0.7.0 development state (2026-10-16)

Full native rendering is not complete. The backend-specific work shares address
proofs and colour/depth packing where the APIs have the same semantics. It does not
silently substitute an unsupported operation or count a compatibility path as
native coverage.

The [native GPU call conversion](native_gpu_calls.md) separately tracks replacing
the game's graphics API writers. Its bounded draw/constant pilot is disabled by
default; passing resource-local backend checks does not prove that this API
conversion is complete.

| Backend | Current native work | Remaining dependencies |
|---|---|---|
| Vulkan | Resource-local CPU/GPU inputs, retained indices and proven exports; sparse and wrapped aliases, colour/depth/MSAA packing, transfers between original and scaled targets | Unproved addresses or missing owners still use compatibility paths; unseen campaign states and device limits need qualification |
| Direct3D 12 | The shared input proofs and native ownership work, compact scaled resolve buffers, native packing and mixed-resolution transfers in both binding modes | Unproved addresses or missing owners; native Windows drivers and campaign coverage |
| Direct3D 11 | The shared input proofs and native ownership work, independent base/mip owners, native packing and mixed-resolution transfers | Unproved addresses or missing owners; native Windows drivers and campaign coverage |
| Android Vulkan | The Vulkan implementation builds for ARM64, with compute exports where vertex storage writes are unavailable; launcher and touch controls share the app | Pixel hardware qualification; overlapping/tessellated exports and unsupported clipping/fill features remain explicitly guarded |

The latest native checks close the known fractional/reversed input, retained-owner,
resolve-format, wrapped-address and mixed-resolution cases. They retain the
compatibility diagnostics rather than treating an unsupported operation as native.
This is coverage of the generated and recorded cases, not proof of every possible
game state or a promise that every driver is bug-free.

The [renderer performance notes](renderer_performance.md) cover batched native
page ownership, reduced DX11 upload work and Windows initialization fixes. These
changes preserve the coverage checks and do not change shader encodings.

- The input proof includes outward bounds for finite binary32 rounding and
  flush-to-zero, with physical address checks for negative offsets. Its CPU
  check covers 4,080 exact cases, 1,470 rounding cases, and 1,922 larger-index
  cases with 69,144 executed rounding/flush results. The artificial 22-bit
  cutoff is gone; finite arithmetic, signed conversion and physical memory
  bounds determine whether the input is safe. The GPU geometry suite
  passes 600 cases across Vulkan vertex/compute exports, DX11 and DX12
  bindless/bindful modes, with 1,258,291,200 exact output bytes. A further 30
  cases cover inputs larger than 1 MiB, with 251,658,240 exact bytes.
- Native GPU owners survive cache clearing and the soft resolve-buffer budget.
  Sparse live pages are compacted with GPU copies. Publication checks preserve
  the winning CPU/GPU version during overlapping writes; the 39-transition
  memory check compares 8,740,032 bytes, including 2 MiB immutable index snapshots.
  The cache budget limits retained snapshots, not the size of a valid stream.
- Native packing covers ten canonical render-target source families and 21
  destination packing families, from 8 to 128 bits per texel. The independent
  GPU check passes 4,060 dispatches and 3,580,887,040 exact bytes, including
  MSAA, endian modes, conversions, padding and array slices. Forty-three
  consecutive resolves also match the general path at 1x/2x on all three APIs.
- Resolving an aliased view no longer changes its source ownership. Gamma aliases
  preserve raw source bits, and wrapped tile addressing handles partial final
  rows and repeated periods. Thirty-two wrapped-frame checks match an independent
  address/pixel reference without compatibility dependencies or allocations.
- Original-resolution depth and scaled colour targets transfer in both directions.
  Viewports, clears, packers and texture metadata use the actual target scale.
  DX11 and DX12 share the transfer shader generator. The patterned GPU fixture
  passes 594 cases, including MSAA, stencil, retained snapshots and individual
  host-pixel markers. Production replays also exercise retained host-depth
  restoration and original-resolution D24S8/D24FS8 resolves with exact pixels.
- Vulkan's private vertex-fetch constants now carry a signed 31-bit word offset
  and a separate valid flag. Large or negative original indices need no unused
  prefix in the compact input buffer. The offset encoding passes 160 CPU cases.
  Production replays at 22-bit, 23-bit and near-maximum 24-bit indices pass 24
  exact frame comparisons at 1x/2x across Vulkan, DX11 and both DX12 binding modes.
  Vulkan uses an 84-byte input snapshot for each of those draws, with zero
  compatibility dependencies or allocations.
- CPU draw-extent estimation now matches the GPU's physical vertex-fetch bounds:
  a nonzero stream size does not truncate valid reads beyond its declaration.
  GPU-owned vertex and DMA-index pages are not read from stale CPU memory to
  shrink attachment ownership or replace a draw with a clear. Those draws keep
  conservative bounds and run on the GPU. Five ownership/extent regression
  checks pass, including recovery after CPU rewrites, without creating a mirror.
  The refreshed 600-case geometry suite and 32 recorded-frame comparisons also
  pass with exact output and zero compatibility dependencies or allocations.

All 6,660 bundled translated variants have been rebuilt for the private fetch
encoding and pass SPIR-V validation. The shared encoding helper participates in
the translator source hash, so older device caches and shader sets cannot be
loaded with the new ABI. No previously shipped variant was dropped.
Eight additional comparisons disable geometry shaders and vertex storage writes
and restrict storage-buffer ranges to 128 MiB. The rebuilt packaged set matches
fresh translations exactly, records zero compatibility dependencies or allocations,
and serves every used shader without runtime translation. These are capability
checks on the Deck driver, rather than a Pixel hardware qualification.

The refreshed executable/cache inventory contains 383 programs: 217 vertex
programs with static addresses and 166 pixel programs. It finds no unproved
vertex-address programs, pixel-stage vertex fetches, memory exports or dynamic
register addressing in this set. That establishes shader prerequisites; it does
not establish every draw's resource ownership. Later CPU writes, aliases and
hardware allocation limits still require runtime checks.

`native_gpu_strict` and `tools/bench/native_coverage.py` reject compatibility
dependencies and allocations. Captured-frame replay and generated GPU fixtures
check output independently. These tools allow qualification without manually
replaying each level. The local Windows API checks run through Proton on the Deck;
native Windows drivers and the Pixel GPU still need focused runs.

### Earlier 0.0.7.0 qualification (2026-10-15)

The shared vertex-input bounds proof also accepts exact non-power-of-two,
negative and zero address scales. It tracks the binary grid through each add,
multiply and optional rounding operation; products that would lose precision
still keep their compatibility path. Negative scales reverse the proven endpoints
before deriving the native input range. Vulkan, DX11 and DX12 use this same proof.
Its CPU check passes 4,080 expressions and 1,404,880 individual fetches. The expanded
GPU geometry/index suite passes 72 cases on each of the five configurations,
360 cases and 754,974,720 exact bytes in total, with zero compatibility dependencies
or allocations. It includes scaled and reversed fetches, point/rectangle expansion,
GPU index rewrites and later CPU ownership. The earlier proof fails the new strict
GPU check. Eight attack, overlapping-resolve, Eighty Bites and menu comparisons at
1x/2x retain identical pixels and zero compatibility dependencies or allocations.
These checks use the Deck GPU, with Windows APIs through Proton; Pixel and native
Windows drivers and unseen campaign states remain unqualified.

Retained GPU-written DMA indices now have immutable native snapshots on all
three backends, including Vulkan's compute export path. Topology conversion,
restart handling, vertex bounds and memory-export proofs consume the same GPU
version rather than stale CPU memory. Exact ranges are cached until a CPU or GPU
write invalidates them; new versions download only their compact index range.
The cache retains at most 64 entries and 8 MiB. Larger valid streams now live as
uncached immutable snapshots for their consumers; the earlier 1 MiB stream limit
has been removed. Unretained writers and unproved streams keep their compatibility path.

The GPU index check passes 42 cases per configuration on Vulkan vertex/compute
exports, DX11 and both DX12 binding modes: 210 cases and 440,401,920 exact bytes.
It leaves the CPU index memory stale during GPU production and a second GPU
rewrite, then switches ownership back to the CPU. All five runs report zero
compatibility dependencies and allocations. The earlier runtime fails this
strict check. Eight saved attack, Eighty Bites, Donut and menu comparisons also
retain identical pixels at 1x/2x. These checks qualify their generated and
recorded states on the Deck GPU, with Windows APIs through Proton; they do not
prove unseen campaign states or native Windows and Pixel driver behavior.
Twelve further comparisons add title, saves, loading, walking, pause and
super-burp frames at both scales, also with identical pixels and zero
compatibility dependencies or allocations: 20 saved-frame comparisons in total.

Linux audio qualification exposed a separate real-time CPU-budget crash during
load testing. The mixer keeps its scheduling priority and yields at safe buffer,
voice-poll and decoder boundaries before exhausting the host limit. The CPU clock
check is gated to avoid adding a syscall to every busy poll. A 300 ms CPU check
passes with priority 20 and the original 100/200 ms limits; its unguarded control
terminates with SIGXCPU. The combined runtime's 2x, 16-frame audio queue check
passes gameplay, four CPU workers and recovery: 7,680 played/submitted frames,
zero underruns, zero silent submissions and a clean exit. Its 5,599 rendered
frames also record zero compatibility dependencies or allocations. This is one
Linux workload, not a qualification of every audio scene or platform.

The native memory-export paths now keep resource-local GPU buffers and retain
their written pages as inputs to later draws. The five configurations (DX11,
DX12 bindless and bindful, Vulkan vertex exports and Vulkan compute exports)
passed 90 export cases and five GPU producer/vertex consumer chains, including
CPU rewrites of neighboring pages. This work removes the previous blanket
fallback for GPU-written vertex streams and proven memory exports. Dynamic or
modified export addresses still require an independent proof.

Point and rectangle expansion on Vulkan now uses the original guest vertex
range and freezes shader-loaded DMA indices beside the native streams. The
16-bit half-word offset is preserved. Native export buffers receive the same
frozen indices used for their bounds proof. All three backends also accept
built-in auto-index adapters and use the converted stream's actual format and
byte order. Converted CPU indices have an immutable shadow retained with cached
GPU buffers; the default uncached path reuses its CPU storage. These paths are
covered by [native_geometry_check.cpp](../../tools/native-renderer/native_geometry_check.cpp).

The expanded geometry check passes 42 cases on each of the five configurations:
210 cases and 440,401,920 exact output bytes. Each case includes two draws with
different output constants, covering cached conversions and consecutive writes.
Vulkan's vertex and compute export paths also pass 160 consecutive GPU-data
consumer frames each within a 16 MiB native buffer budget. Previously, publishing
a second export could flush its preceding version into the physical mirror;
publication now retains untouched pages and retires wholly preserved versions.
All these checks report zero compatibility dependencies or allocations. Eight
captured-frame comparisons with geometry shaders disabled also have identical
pixels: attack effects, Eighty Bites, Colossal Donut and menus, each at 1x/2x.

The Vulkan effect fix now carries the whole written extent across native
render-target aliases. Texture inputs can combine several immutable GPU resolve
versions and CPU-owned gaps. Pending padding preserves the earlier memory image;
CPU writes invalidate it, and resource generations survive their GPU submissions.
Unknown mixed pages retain the compatibility path. The special-attack frame and
a trace with a 4 KB overlap match their reference at both scales. The two 2x
strict captures report zero legacy dependencies and no EDRAM, physical mirror or
scaled mirror allocation. That is captured-scene coverage, not full-game proof.

The earlier campaign census sampled all 18 episodes, with 16 reaching gameplay
after the longer runs. Its last observed EDRAM and memory-mirror dependency was
the D24S8-to-2:10:10:10 colour alias in Eighty Bites and The Day the Earth Stood
Stupid. The native depth-to-colour transfer added for 0.0.6.5 covers that case.
The current runtime also passes six strict alias comparisons at 1x and 2x:
two uniform patterns and a quadrant pattern, with 30 alias transfers in total.
Every comparison has identical pixels, zero legacy dependencies and zero EDRAM,
physical-mirror or scaled-mirror allocations. This closes the known census case;
it does not establish that every format, alias or draw in a complete playthrough
has native coverage.

The Android portability work makes vertex storage writes optional. A compute
prepass runs the original guest index stream before rasterization and preserves
base offsets, clamps, primitive restart and byte order. It rejects overlapping
input/output ranges and tessellated exports until those cases have an independent
input snapshot and matching domain implementation. Unsupported user clip planes
and non-solid fill are rejected explicitly on devices without those features.
These restrictions still need actual game coverage on the Pixel 9 Pro.

Translated system constants now obey std140 vector alignment. Vector selects
also satisfy the SPIR-V 1.3 rules used by Vulkan 1.1 devices. Both translated
stages pass the Vulkan 1.1 validator without relaxed layout options. The shipped
set has been rebuilt with module version 2 and configuration format 3. It now
contains 6,640 variants, retaining all 6,604 previously shipped variants and
adding observed native point/rectangle and setup-draw modifications. The export helper retranslates the previous
set's modifications and fails if any are missing; it never relabels stale modules.
Native GLSL replacements use the same constant layout and module version.
Their cache records its SPIR-V version too: a driver that cannot consume the
native module version uses a compatible translation instead of loading an
unsupported replacement.

Read-only translated modules can also serve compatible devices with fewer
vertex-stage features or smaller storage-buffer ranges. The loader checks each
program: compact native vertex snapshots stay within the device's storage-buffer
range, so their reads stay in descriptor zero even with a 128 MiB storage limit. A smaller shader
descriptor array is valid with the larger runtime layout ([Vulkan layout rule
07991](https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineCreateInfo.html)).
Memory exports and physical-address vertex inputs retain their configuration
requirements. Source hashes, math controls and every other field still must
match; malformed, duplicate and unknown fields are rejected.

With geometry shaders and vertex storage writes disabled and storage ranges
limited to 128 MiB, all eight sampled 1x/2x frames match fresh translations
exactly, with zero runtime shader translations or compatibility allocations.
The six negative configuration/address checks and 42 memory-export cases also
pass without serving incompatible modules. This qualifies the capability
combinations on the Deck driver, not the Pixel driver. The source cache used for
the bake contains 380 programs: all 214 vertex programs have static addresses,
and none of its vertex or pixel programs use memory exports or pixel-stage
vertex fetches. This inventory describes the cache, not a complete playthrough.

The executable inventory can now run the same shader analysis offline. Merging
its 283 validated containers with 34 existing captured caches produces 380 unique
programs: 214 vertex programs with static addresses and 166 pixel programs. This
set has no memory exports, pixel-stage vertex fetches or dynamic register
addressing. Synthetic programs independently check that the inventory flags all
three address/export exceptions. `native-analysis.json` retains per-program fetch
formats and constant dependencies; it does not prove draw-level resource ownership.

`tools/bench/native_coverage.py` checks strict reports from Vulkan, Direct3D 11
and Direct3D 12 without launching the game. It rejects missing counters,
incomplete runs, nonzero compatibility bytes or allocations, and malformed
numeric counters. The normal frame check still requires native vertex streams
and resolves. Geometry-only checks explicitly require vertex streams instead.
For example:

```sh
python3 tools/bench/native_coverage.py --backend vulkan frame.coverage.json
python3 tools/bench/native_coverage.py --backend d3d11 \
    --require-operation vertex_streams native-coverage.json
```

The refreshed five-configuration geometry suite again passes 210 cases and
440,401,920 exact output bytes with zero compatibility dependencies or allocations.
Four further restricted-capability attack/alias replays match their general-path
reference exactly at 1x/2x; a deliberately enabled memory-mirror consumer fails
the strict guard. These automated checks cover their recorded states and generated
cases. They do not establish unseen campaign behavior or qualify Pixel and native
Windows drivers.

Rendering evidence for this work includes 132 compute GPU cases with
276,824,064 exact output bytes, 18 command-processor cases with 37,748,736 exact
bytes and 18 captured presented frames, and eight exact frame comparisons: the attack and
overlap frames at 1x/2x, with vertex storage writes enabled and disabled. These
checks exercise the Steam Deck's Vulkan driver; they do not qualify a phone or
native Windows drivers. The Direct3D checks and remaining work are in
[d3d11_renderer.md](d3d11_renderer.md) and [d3d12_renderer.md](d3d12_renderer.md).
The integrated cases also cover presentation after accurate memory-export
readback. Readback ends its submission, so swap now opens a new submission before
recording texture loads and presentation; otherwise the exported bytes could be
correct while the frame failed to present.
Android build and device restrictions are in [../../android/README.md](../../android/README.md).
The camera and raster work is tracked in [widescreen.md](widescreen.md).

The shared vertex address proof now covers exact constant offsets and
power-of-two scales on all three backends. Its independent scalar check passes
1,440 cases and 495,840 float operations on Linux and Windows through Proton,
with 17 deliberately unproved inputs rejected. The depth packing GPU check
passes 136 dispatches and 35,651,584 exact bytes, including padding, stencil,
float24 rounding, endian variants and symmetric/asymmetric scales.

Direct3D 12 now passes eight strict menu/Springfield frame comparisons at 1x/2x
in both binding modes: 55,296,000 RGB bytes match, with zero compatibility
dependencies or allocations. A private live run through startup, saves,
Springfield, movement, attacks and pause/resume at 1x also records zero; a 2x
title run does too. Direct3D 11 records zero dependencies and allocations through
the same live 1x flow and a 2x title run. The refreshed Linux dev runtime records
zero through 3,240 frames covering startup, Springfield, attack inputs and
pause/resume at 2x. Both Windows backends pass eight additional native alias
comparisons against their general paths, covering uniform and quadrant data
at both scales. These are bounded checks through Proton, not full campaign or
native Windows driver qualification.

Eight further Windows comparisons cover the original attack-flash and Eighty
Bites captures at 1x/2x on both backends. Every native frame has identical pixels
to its general resolve path and zero compatibility dependencies or allocations.
Direct3D 11 also keeps the unresolved tail of a sampled layout in native scaled
storage, rather than allocating a 512 MiB physical-address buffer for a 64x64
texture. All native GPU texture inputs now arm CPU write callbacks for their
whole sampled range, including CPU-owned levels and padding, without declaring
the compatibility mirror current.

Next work, in order:

1. Expand strict saved-frame coverage across the available campaign captures,
   including effects, loading, co-op, cache clearing and original-resolution
   targets. Record any new fallback reason and reproduce its exact inputs.
   [Checkpoint testing](checkpoint_testing.md) now prepares isolated map starts,
   discovers loaded checkpoint IDs and verifies the active target before taking
   a frame. A copied Medal of Homer test save also resumes its third section
   with the original gameflow; no binary save edits are needed.
2. Qualify remaining guarded cases and hardware limits before extending them.
   The current inventory has no unproved address/export programs, but that does
   not prove every resource sequence or an arbitrary modified shader safe.
3. Run the ARM64 app on the Pixel 9 Pro and qualify its Vulkan limits, input,
   audio and surface lifecycle. The APK's ARM64 libraries and package alignment
   pass 16 KB checks; actual device behavior still needs qualification.
4. Connect widescreen raster allocation, post effects and HUD framing to the
   proven projection hook, preserving video aspect and split-screen behavior.
5. Remove compatibility implementations only after diagnostics and rendering
   checks prove they have no consumers. Check native Windows drivers separately.

## Design

The target architecture. It is reached by converting the existing Vulkan backend piece by
piece (see Approach below) rather than as a separate `graphics/native/` processor, but the
components are the same.

```text
recompiled game + statically linked XDK D3D (untouched)
  -> XDK command segments (PM4) in guest memory, kicked through the primary ring
  -> CommandProcessor base: packet decoding, register file, guest sync contract
     (fences, interrupts, WAIT_REG_MEM, swap, gamma ramp)
  -> native command processing: IssueDraw / IssueCopy / IssueSwap
       surfaces   host render targets keyed by EDRAM base + format + size + samples,
                  overlap model for aliasing (clear or reinterpret on rebind)
       resolves   copies into host textures keyed by guest address + format + size,
                  sampled directly; written back to guest memory only when the CPU reads
       textures   decoded once (untile, endian, format), revalidated by write watches
       shaders    Xenos microcode translated to SPIR-V (SDK translator, AOT + fallback)
       pipelines  exact state mapping, persistent pipeline cache
  -> VulkanPresenter (unchanged)
```

The command stream is the complete, exact source of GPU state for this game: the XDK
writes every draw's state into its command segment, and constants that bypass the D3D
device shadow (shader literals via LOAD_ALU_CONSTANT, GpuBeginShaderConstantF4, inline
SET_CONSTANT) only exist there. Reading it replaces the device-shadow capture of the
XDK-hook design used by other ReXGlue native ports, with the same result.

Invariants: never reorder, merge, cull, drop or substitute a submitted draw; map state
exactly; keep game-visible semantics (pixel centers, PWL gamma, EDRAM aliasing, NaN-as-zero
constants, MSAA sample counts, resolve tiling when the guest reads resolved memory).

## XDK D3D map (xdk_sigs match against the Conan-era signature set: 174 exact)

| Function | Address |
|---|---|
| D3DDevice_DrawVertices | 0x8244CF78 |
| D3DDevice_DrawIndexedVertices | 0x8244D360 |
| D3DDevice_DrawVerticesUP | 0x8244C910 |
| D3DDevice_BeginVertices / EndVertices | 0x8244C450 / 0x8244C8F0 |
| D3DDevice_BeginIndexedVertices / EndIndexedVertices | 0x8244C970 / 0x8244CEA0 |
| D3DDevice_Clear / D3D_InternalClearDraw | 0x82453C30 / 0x82453B08 |
| D3DDevice_Resolve | 0x82455570 |
| D3DDevice_Swap | 0x824544F0 |
| D3DDevice_BeginTiling / EndTiling / SetPredication | 0x824675C8 / 0x82467B50 / 0x82467450 |
| D3DDevice_CreateShaderA / B (container) | 0x82448178 / 0x82448308 |
| D3DDevice_SetVertexShader / SetPixelShader | 0x82445578 / 0x82445278 |
| D3DDevice_SetTexture / CreateTexture | 0x824408E0 / 0x82440578 |
| D3DVertexBuffer_Lock / Unlock | 0x824418A0 / 0x824419A0 |
| D3D_KickOff / RingMakeSpace / RingAlloc | 0x824572F8 / 0x82457CC8 / 0x82457F50 |
| D3D_AddCallsToPrimaryBuffer (INDIRECT_BUFFER) | 0x82456E68 |
| D3D_BlockOnFence / PollGpuProgress / BlockUntilIdle | 0x824574B8 / 0x82452018 / 0x82458080 |
| D3D_LoadShaderLiterals (LOAD_ALU_CONSTANT) | 0x8245F810 |
| D3DDevice_GpuBeginShaderConstantF4 | 0x82444BD8 |
| D3D_BeginVizQuery / EndVizQuery | 0x8244EEA8 / 0x8244EF78 |

Device layout matches the Conan-era XDK notes (command segment write pointer at
device+0x30).

## Approach (since 2026-10-01)

The Vulkan backend is converted in place: one emulation piece at a time is replaced by a
native one, each step verified bit-identical on the captured frames and shippable on its own,
instead of building a separate renderer next to it. Stages, in order:

1. **Shaders ahead of time** - done for the captured scenes. Captured shaders are translated before play;
   the runtime serves `native_shaders/` (hand-written natives) and `native_shaders/translated/`
   (exported translations, used only when the translator source hash and the GPU configuration
   match the ones they were made with). Nothing is translated during play in the captured scenes.
2. **Native resolves everywhere** - done for every resolve in the captured scenes. Resolves
   into textures are drawn directly (lazy memory write-back at 2x); resolves no texture reads
   yet write the memory directly from the render target. The EDRAM buffer dump and compute
   resolve remain only as the fallback for unsupported cases (MSAA sources, exponent bias,
   gamma, non-bitwise-equivalent formats).
3. **Render targets as surfaces** - done. Host render targets are
   the size of what the game draws to (`native_rt_size_by_use`: the rows of EDRAM tiles a
   target is drawn to, grown with a copy when more are needed) instead of covering the whole
   2048-tile EDRAM period: the main color and depth targets are 1280x720 (they were 1280x2048),
   the shadow map 1040x1024 - about 95 MB less video memory at 2x. Each target has its own
   resolution scale (`native_rt_original_resolution_targets`, the launcher's "Shadow
   resolution" option). Copy-free resolves (`native_resolve_copy_free`): a resolve of a whole
   render target into a texture is held back until the next draw, and if that draw provably
   overwrites the whole target, the texture takes over the target's image (and the target the
   texture's old one) instead of copying it. Every color resolve here swaps red and blue, so
   such a texture's views swap them back and its memory write-back too. The three resolves of
   the post-processing chain are copy-free when enabled (about 0.57 ms at 2x); the front buffer
   resolve, read by the swap, is still copied. Live 2x runs used to alternate old and new frames
   with it on: the swap recorded the held back front buffer copy before opening its submission,
   and opening one resets the command buffer, so whenever the frame's last submission had
   already ended the copy was dropped and the swap showed the front buffer from two frames
   before. Held back copies are now recorded only in an open submission. Live 2x recordings of
   the title, Springfield and the pause menu have no frame reversals with it on (438, 32 and
   388 before the fix), and replays of four captured scenes are bit-identical with and without
   it. The launcher turns it on (settings version 4 replaces the old stored default once).
   Static frame replays alone do not catch this kind of regression; check live recordings
   (frame order) too.
4. **Geometry without emulation tricks** - in progress. Rectangle lists, quad lists and point
   sprites without geometry shaders are exact (`vulkan_geometry_shader_primitives = false`, used
   automatically on GPUs without them, such as Mali); quads are split like the geometry
   shader's strip (`quad_list_triangle_order`). Native index buffers (`native_index_buffers`)
   snapshot ordinary CPU-written DMA streams into host index buffers, preserving endian and
   restart handling. Native vertex streams (`native_vertex_buffers`) pack the ranges a draw
   reads into a separate host buffer. The shader keeps the exact vertex fetch decode, with
   private fetch constants pointing into that buffer. Bounds must be proved from the shader
   and the exact uploaded indices; GPU-written pages, memexport, expanded primitives and
   unproved shader addressing keep the existing path. Null optional streams still read zero.
   Address proofs now cover all 107 vertex shaders in the collected set, including
   unconditional fetches before branches and the particle shaders' quarter indices.
   Immutable vertex and DMA index buffers can be retained across draws and frames,
   with their guest bytes compared before reuse and old generations retired only
   after their GPU work finishes. Expanded primitives and unsupported GPU-written
   streams still need the existing path.
5. **Memory** - in progress. Uploads no longer wait for the GPU every draw: the per-frame
   reupload of every buffer (`clear_memory_page_state`) is off, and pages the game rewrites
   every frame are only uploaded when the bytes a draw reads changed
   (`gpu_stream_skip_unchanged`). Native buffer draws bypass those residency scans and read
   their own snapshots. CPU-owned texture inputs can also use compact upload buffers,
   with the same tiling, endian and format decode and watches for in-place changes.
   GPU-written inputs and unsupported draws still use the mirror. Next:
   no full guest memory mirror on the GPU and no write
   watching; textures and buffers uploaded when the game loads or changes them (hooks on its
   resource code).
6. Removing the emulation paths, Windows check, Android port.

## Validation

- Offline A/B: `tools/bench/replay_ab.py` replays captured frames through renderer variants
  headlessly, diffs the images and reports GPU time per category. Build the replayer with
  `tools/native-renderer/build_trace_reference.sh`.
- Captures: `tools/bench/autorun.py` drives the game unattended (injected pad input, engine
  screenshots, perf, GPU clock/power) and `trace <label>` grabs a frame trace.
- New GPU-visible code runs on llvmpipe first (`VK_ICD_FILENAMES=.../lvp_icd.x86_64.json`):
  a GPU fault on the Steam Deck resets the GPU and can black out the session. llvmpipe
  replays 1x and 2x traces.
- `TRACE_STREAM_LEARN=1` makes the replayer's memory writes count as write faults, so pages
  the trace rewrites every frame become streamed like in the running game (needs
  `TRACE_BENCH` of 4 or more).
- `REX_CMD_STATS=1` logs the commands, barriers and uploads per command buffer (`=2` also the
  most uploaded ranges, `REX_CMD_STATS_EVERY=<n>` the interval).
- Debug modes that prove exactness: `native_resolve_debug_reload` (textures reload from the
  memory native resolves write), `native_resolve_debug_memory_only_all` (every resolve writes
  only the memory), `native_resolve_debug_verify_stencil_capture`.

## State (2026-10-02)

- Shaders: `aot_export_path` exports runtime translations in the served format;
  `TRACE_SHADER_STORAGE=<cache>:45410809` makes the replayer load a shader storage the way the
  game does at boot; `tools/native-renderer/build_translated_set.sh` builds
  `translated_shaders.tar.xz` (about 1 MB, both scales, with the plain / level 0 texture
  variants), which the release workflow unpacks into `native_shaders/`.
- EDRAM transfers: transfers into render targets cleared right after binding are held back
  over consecutive clears and done only outside the cleared area (the shadow map pass: 0.7 ms
  at 2x down to almost nothing). A draw proves it overwrites render targets entirely either
  with the XDK clear shader (positions read from its vertex buffer) or with any vertex shader
  the CPU interpreter can run (`native_rt_cpu_vs_overwrite_proofs`), which covers the
  full-screen post-processing passes - the effect chains that reinterpret EDRAM at other
  pitches (the super burp's glow) then skip copying dead data. Proofs are checked with
  `native_rt_debug_poison_overwrites`, which fills each proven area with garbage before the draw.
- Clears: the XDK's clear draws are done as clears of the attachments
  (`native_rt_clear_draws_as_clears`) when the draw replaces everything it writes with the
  constants from its vertex buffer, so AMD GPUs fast-clear compressed targets instead of drawing
  every pixel (about 0.45 ms at 2x).
- Post passes: the native post-processing shaders fetch their texture taps in batches (5 or 4
  at a time, the counts this game uses), so the latencies overlap (about 0.45 ms at 2x).
- Copy-free resolves: textures that can take over render target images are created with their
  own memory and the usages of render target images; `native_resolve_copy_free_debug_writeback`
  writes the memory from each taken-over image and reloads the texture from it, to check the
  write-back. On llvmpipe the shared memory is split into several bindings, so 2x resolves take
  the compute path for the memory there, and the texture is still taken over.
- Stencil: depth resolves of an area whose stencil is known to be uniform (cleared, not
  written since) skip capturing it (`native_resolve_uniform_stencil`, on, about 1 ms at 2x).
- Compression: native resolves write 8_8_8_8, 2_10_10_10 and depth textures through a view of
  their own format (`native_resolve_unorm_views`), so AMD GPUs keep them DCC-compressed like
  the render targets (1.1 to 1.4 ms at 2x).
- GPU time per frame on the Steam Deck replaying captured frames (with the streaming the
  running game does): t09 gameplay about 4.3 ms at 1x and 10.5 ms at 2x, Springfield about
  4.8 ms at 1x and 10.9 ms at 2x (16.7 ms at 2x before the upload and compression work).
  Replays keep the GPU clock low, so only A/B runs made one after the other compare. With the
  batched post passes and native clears, t09 at 2x went from 12.8 to 11.8 ms in such an A/B,
  and copy-free resolves took it to 11.1 ms.
  Live, Springfield at 2x runs at about 59 FPS; the GPU now needs about 1100 MHz there instead
  of 1200 at the same frame rate, which leaves headroom for effects.
- Where 2x GPU time goes (t09, natives): the three post passes about 35% (346B alone 17%),
  resolves about 20% (two depth resolves nothing samples yet about 9%, the post-processing
  chain's color resolves about 6%), world draws most of the rest. Hand-written world shaders
  gain little over their translations (E170: 5%).

## State (2026-10-06)

- The development tree is based on v0.0.6.3, with the native renderer and timing work retained.
  Copy-free resolves stay off by default. Live fullscreen runs at 1x and 2x cover the title,
  menus, Springfield with camera motion and the pause menu. With copy-free off, the pause
  recordings have no repeated old-frame reversals; enabling it again brings them back.
- Overwrite proofs now require depth and stencil tests to pass for every fragment. Coverage
  alone cannot justify discarding the old render target contents when either test may reject
  the draw. This applies to skipped transfers, deferred transfers and attachment clears.
- Native buffers are checked against the mirror path on seven captured scenes at 1x and
  2x: title, slots, level arrival, walking, pause, Springfield and super burp. Every compared
  image is identical. Live fullscreen recordings also cover title, menus, camera movement
  and pause; the new path has no repeated old-frame reversals in those recordings.
- In a paired live 2x run of the same runtime and shader set, the title's median frame time
  drops from 47.6 to 33.3 ms (30 FPS). Renderer CPU time drops from 45.4 to 17.7 ms, with all
  3679 submitted draws retained. The vertex/index residency scans took about 30 ms before;
  supported draws now bypass them. Native uploads still happen: each draw gets fresh bytes,
  so streams rewritten between draws and frames cannot reuse an old snapshot.
- Index bounds are scanned from a CPU copy of the exact uploaded bytes. Reading the mapped
  Vulkan upload buffer one index at a time is very slow on the Deck's uncached memory and
  caused a gameplay regression in the first candidate. That candidate is not installed.
- Springfield remains around 60 FPS in the tested live scene. About half its submitted draws
  use native vertex streams (162 of 355 in the level-arrival trace); branched or predicated
  shaders and other unproved addressing still need the mirror. Native snapshots add CPU
  copying and bounds work there. Retaining static resource buffers and widening the address
  proofs are the next steps; the title improvement does not establish a gameplay speedup.
- The shader set is regenerated for the new translator: 308 translated variants at each
  scale, with the existing hand-written shaders retained. Stored draws preload both native
  and fallback shader modules and driver pipelines. The tested live scenes report zero
  runtime translations. Full-game shader and buffer coverage remain to be checked.
- Trace variants must load the intended runtime. The replayer is linked with RUNPATH so
  `LD_LIBRARY_PATH` can select it, and `replay_ab.py` fails on process errors or rejected cvars
  even if an image was produced. Static replays are followed by live motion tests.
- 120 Hz and unlimited rendering are not qualified. The guest scheduler still uses 59.94 Hz;
  simulation, audio, input and cutscene timing need to be checked as rendering is decoupled.

### Resource buffers (2026-10-06)

- The launcher now uses the qualified native resource runtime on v0.0.6.3. Native texture
  uploads, retained vertex buffers, retained DMA index buffers and bulk state register
  writes are enabled. Copy-free resolves remain off. The previous runtime and config
  are backed up locally.
- The collected shader set has 343 translated modules per scale, with all 32 existing
  hand-written modules retained. All earlier translated modules are byte-identical;
  the extra modules cover shader variants missing from the previous set.
- Vertex address proofs cover all 107 collected vertex shaders. They retain the exact
  fetch decode and prove only the unconditional fetch prefix, excluding every branch
  target that can enter it. The particle shaders' dyadic index expressions have exact
  float and range guards. The address oracle checked 1,372,280 vertex samples; the
  independent index scan oracle checked 300,544 scans and protected-page tails.
- Springfield's 728 submitted vertex draws and the level-arrival trace's 355 draws
  all use native streams at both scales. GPU-written streams, memexport, pixel-shader
  vertex fetches and unproved addresses still fall back. This is captured-scene
  coverage, not proof of full-game coverage.
- Retained vertex and index snapshots compare the source bytes before every reuse.
  Changed data gets a fresh immutable version. Descriptor and upload generations
  survive until the frames using them complete. Forced small-budget retirement is
  included in replay comparisons.
- Index retention passes 54 software Vulkan and 30 Deck GPU replay runs at 1x and 2x,
  including forced retirement. Every image is identical to fresh index uploads.
  Live 1x and 2x title, gameplay and pause recordings have no detected old-frame
  reversals. Audio is present in the 1x title and gameplay captures. A separate 2x
  run without compilation or replays stays around 58 FPS in Springfield; it does
  not establish a controlled gameplay FPS gain over the earlier runtime.
- Compact native resolve outputs are experimental and off in the installed runtime.
  The first prototype passes 36 image comparisons at both scales and 18 readback
  comparisons, with all ten dumped regions matching in each of nine traces. The
  retained output and direct texture-read extension is being checked separately.
  These prototypes still copy back to the existing memory buffers; removing that
  dependency and the remaining EDRAM fallback is unfinished.

### 0.0.6.4 development build (2026-10-06)

- The launcher now uses the qualified 0.0.6.4 runtime locally. The earlier
  native resource runtime and the player's config are backed up. Copy-free
  resolves and the experimental buffer write watches stay off.
- Compact resolve outputs, direct texture reads, deferred memory copies,
  ordered buffer reuse and matching render target image copies are enabled.
  At 2x, resolves that can stay in native textures keep that path, avoiding
  unnecessary packing into raw buffers. Unsupported cases retain the fallback.
- Matching full unorm color resolves copy into separately owned textures.
  Partial updates preserve the texture's channel order; a forced memory
  reload flushes that order before rebuilding its view. Independent 8-bit
  and 10-bit image and memory checks pass at 1x and 2x on software Vulkan
  and the Deck GPU. Thirty-six hardware and thirty-six software replay
  comparisons are identical. No image-exchange shortcut is enabled.
- A CPU write invalidates the overlapping pages of a deferred resolve without
  losing the untouched GPU-written pages. A read-back probe writes through an
  actual guest physical alias and checks both the CPU patch and the remaining
  GPU bytes at 1x and 2x. Both are exact. Software and hardware image, texture
  reload and raw resolve read-back comparisons also pass.
- Index bounds are retained only with the exact immutable index bytes and
  matching endian, restart, base and clamp state. The cache probe checks
  512 distinct state keys against a separate scalar scan at both scales.
- The expanded shader set contains 6,626 SPIR-V modules, retaining all 718
  previous module paths. Eight fullscreen search modules are corrected; the
  other 710 are unchanged. Executable container extraction
  and the common-modification bake widen coverage; patched vertex programs,
  dynamic register counts and other unseen draw states still need collection.
- The hand-written fullscreen search shader now advances its constant-table
  address once per iteration, matching the original program. A separate Vulkan
  pixel check compares original translation, previous native and corrected
  native modules across 16,000 states. The correction is exact; the previous
  modules fail 15 or 29 states per run. Twenty-four software and sixteen Deck
  full-frame comparisons also match at 1x and 2x.
- Linux timer queues can sleep between deadlines instead of spinning. Live
  audio is checked with this enabled. The audio worker spaces callbacks from
  the actual wake time; SDL conversion, device writes and semaphore release
  run outside the queue lock. A simulated device-write failure returns its
  consumed queue credit. Stereo and surround channel mapping match the scalar
  reference and x64 path.
- The installed launcher's 2x test covers title, Springfield, pause and resume.
  At the saved 16-frame audio queue, the longer gameplay recording has sound
  and no queue underruns. The 4-frame stress test catches a rare underrun.
  Screenshots confirm the expected scenes; the game closes cleanly.
- Four further live runs toggle the new image copies and vertex-cache
  comparison order separately. The 2x scene averages 54.4, 56.8 and 56.3 FPS;
  1x averages 59.5 FPS. Every run has no gameplay audio queue underruns.
  The recorded title, gameplay and pause scenes at both scales have no
  detected old-frame reversals. Camera timing and scene variation prevent
  treating these runs as proof of a consistent gameplay performance gain.
- Checking a previously changed vertex stream first still compares every
  source byte before successful reuse. A separate probe mutates and restores
  each of three streams across 18 ordered checks. All changes are rejected,
  but live performance does not establish an additional gain; the option
  stays off in the launcher defaults.
- Changing vertex streams can reuse their latest immutable snapshot within
  the same frame (`native_vertex_cache_refresh`). Every successful reuse
  still compares all source bytes. Versions use the frame's upload pool and
  transient descriptors, preserving the static cache's budget and earlier
  GPU copies. A later frame always gets its own transient descriptor.
  Separate GPU read-back checks cover 4,200 mutations through guest and host
  writes at each scale on software Vulkan and the Deck. Sixteen old and new
  snapshots remain exact, including after the owner frame changes. Twenty-four
  software and sixteen hardware frame comparisons are pixel-identical.
  The live 2x Springfield check reduces vertex uploads from 218.4 to 99.2 KiB
  per frame, about 55%, and averages 56.6 FPS against 55.5 in the comparison
  run. Scene variation prevents treating that FPS difference as a general
  gain. Moving gameplay, title and pause videos have no detected frame
  reversals; gameplay audio has no queue underruns. The Linux launcher
  enables this path.
- Twenty controlled replay comparisons of the earlier mirror paths and the
  new resource paths are pixel-identical. They do not establish a gameplay
  FPS gain: GPU timings are similar and replay CPU time is slightly higher
  with the new resource paths. The earlier menu residency-scan improvement
  remains the measured CPU gain. Further gameplay profiling is required.
- Existing Linux installs receive missing runtime defaults from the launcher
  without replacing saved overrides. The release places its shader set under
  `launcher/ui/native_shaders`, which the older updater already copies.
- Native XMA commits and setters update only their owned bits. The decoder never
  writes the mixer's read cursor back from a stale snapshot. Acquire/release
  publication makes PCM visible with its output position. Independent checks
  cover 495 consumer advances, 3,072 setters, 3,584 getters, 100,000 concurrent
  updates and 100,000 PCM publications. Two cancelled-work checks also pass.
- The DAC's callback at `0x823462F8` enters its mixer/submit loop at `0x823460D0`.
  The two shared RenderWare wrappers also launch non-audio workers. Only the DAC
  callback requests native audio scheduling, using the same helper as the runtime
  audio worker and decoder. Linux SDL includes DBus support for the desktop
  priority broker. Queue size and existing mixer timing remain configurable.
  In the saved 16-frame queue's 2x Springfield load test, four competing CPU
  workers produced 119 underruns and 16 silent submissions before the fix; the
  candidate records neither. Its normal scene averages 57.6 FPS and the load
  scene 41.5 FPS, against 56.9 and 36.2 in the comparison. These scene runs do
  not establish a general FPS gain. The 4-frame queue still underruns under
  this load, although its mixer submissions stay nonzero. The 8-frame queue
  also records no underruns or silent mixer submissions. Only the three native
  audio threads receive priority. The tested runtime and current frame-pacing
  game build are installed with backups; package qualification remains separate.
- The remaining campaign, unsupported GPU-written geometry, resolve formats,
  memory mirror and EDRAM fallbacks, Windows/D3D12 and proper 120 FPS/unlimited
  timing remain unfinished.

### Campaign census (2026-10-08)

- Every episode was started from a new game (the launcher's start-episode patch, applied to
  a hard-linked copy of the game data) and played for about three minutes at 2x, logging
  every resolve, render target transfer and vertex stream that leaves the native paths.
  13 of the 18 episodes reached gameplay in that time; six-minute runs of the other five got
  Bartman Begins, Enter the Cheatrix and Rhymes with Complaining past their intro videos too
  (first-viewing videos can't be skipped). Bargain Bin and Meet Thy Player were still in
  their intros.
- Every draw in every episode used native vertex streams. The only fallbacks were in The
  Day the Earth Stood Stupid, once: a depth target with a different format (D24S8 instead
  of D24FS8) appeared at EDRAM base 0, so the color target there took its data through
  the EDRAM transfer, and the two front buffer resolves of that frame went through the
  EDRAM emulation. No other episode used the EDRAM paths at all.
- The census also showed that the shipped translated shader set had not been served since
  the 2026-10-08 translator changes: its translator hash no longer matched, so about 95%
  of the shaders were translated during play again (`aot_shaders=4 hit / 84 translated`).
  The set is rebuilt from the union of every shader storage (the executable's shaders, the
  player's cache and the census runs: 380 shaders, 508 recorded pipeline states).
- The set's configuration key now holds only what the translated modules depend on (the
  shared memory binding count instead of the exact storage buffer range; no features the
  translator doesn't read or only asserts; sample interlock only for the interlocked render
  target path). A set made on the Steam Deck is therefore served on any device with the same
  float controls, image view swizzles, demote, sample rate shading, 2x MSAA support and
  32-bit draw indices, not only on identical drivers. Rebuilding with the new key changed no
  module; only the two configuration files differ.

### Guest memory mirror (2026-10-09)

- The last emulation stores on Vulkan are the GPU copy of guest memory (`SharedMemory`, a
  512 MB sparse buffer) and, at resolution scale 2, the texture cache's scaled resolve
  buffer (2 GB sparse). `shared_memory_census` (off by default) logs every caller of either
  every 600 frames, with `[mirror-texture]`, `[mirror-resolve-initial]` and
  `[mirror-resolve-write]` lines naming what still falls back.
- The first census (2x) found the main copy touched only by one texture load per level
  start, but the scaled buffer busy every frame wherever several resolves share memory. In
  Eighty Bites a bloom chain resolves 640x360, then 320x184 twice, to 04B94000: the smaller
  resolves got buffers of their own, so the larger one's buffer was copied to the scaled
  buffer (to keep the bytes the smaller ones don't cover) and rebuilt from it next frame,
  about 1,400 times in three minutes. Colossal Donut adds a 1280x720 resolve to the same
  base.
- Resolves now write into the native buffer of a larger resolve with the same base that
  holds their range; marking the range as GPU-written no longer copies that buffer out
  (`PrepareNativeResolveMemoryForWrite` takes it out of the pending set while it's written);
  a resolve whose texture is smaller than its rectangle (320x180 vs 320x184) writes the
  memory into that buffer with a memory-only pass before the texture-only one; and bases that
  get resolves of several extents (remembered once seen) keep their resolves in the buffers
  instead of texture-first. A buffer of only same-extent resolves is dropped without a copy
  when a texture-first resolve replaces it.
- The rest of the fallbacks were initial contents and partial coverage: resolves to memory
  no GPU work has written take the bytes around the rectangle from guest memory (zero when
  scaled, where nothing at the higher resolution exists); textures and resolves only partly
  covered by native buffers are assembled from them and such fresh memory. The native
  resolve buffer budget (`native_resolve_buffer_mb`) is now per scale pixel: at 2x every
  full-screen target is 15 MB, so ten of them exceeded the old 128 MB and pushed resolves
  back to the scaled buffer.
- Both stores are now created only when something first uses them: the main copy always
  (`gpu_memory_mirror_on_demand`), the scaled buffer when the GPU lacks sparse binding (it
  would otherwise be a 2 GB allocation at startup).
- Particle draws that index a few vertices past their stream's declared size (vertices 0..3
  of a stream sized for three) now get native snapshots of guest memory too. The translated
  fetch doesn't bound such reads, so the mirror path read whatever the mirror held there.
- Result (census of all 18 episodes at 2x): 16 never touch either store and never create the
  main copy. The other two (Eighty Bites, The Day the Earth Stood Stupid) hit the same rare
  event during play: a 1280x2048 D24S8 depth target appears at EDRAM base 0 and takes over
  the rows of the color target there, so the next front buffer resolves read from the depth
  target (`[native-resolve-unplanned] ... source render target key differs, sources [depth
  base 0 ...]`) and go through the EDRAM emulation for a few frames. Ownership is taken by
  rows down to the draw's estimated maximum Y, so even a small pass with a full-screen
  viewport claims the whole target. At 1x the bloom chain had used the main copy every frame
  (a 1x resolve with a texture smaller than its rectangle went through the EDRAM copy); the
  memory-only pass now covers 1x too.
- Gameplay traces of Eighty Bites and Colossal Donut replay pixel-identical with the changes
  on and off (`tools/bench/replay_ab.py`, cvars switching the new paths off), with 0.3-0.4 ms
  less GPU time per frame at 2x and about 0.9 ms (17%) less at 1x. A first version that sent
  every resolve with a native buffer through the buffer path cost 3.8 ms more (the
  full-screen resolves packed 15 MB of memory each). Live FPS in Eighty Bites is unchanged;
  the command processor's time per draw is the same or slightly lower.


### Resolve-and-clear effects (2026-10-10)

- Special attacks and other effects could turn the scene black for about 1.2 seconds while
  the HUD remained visible. Seven captured black frames reproduce it through the native
  render target path; the interlocked reference path renders their scenes correctly.
- Some effect passes resolve the scene and clear the source in the same command. A copy-free
  candidate was held until the next draw, after that clear, so even its eventual native copy
  read the cleared image. Pending copies now finish before preparing either resolve clear,
  including ownership transfers for an aliased depth clear. The ordinary post-processing
  image exchanges remain enabled; this adds no emulation fallback.
- The previous runtime fails the replay comparison by 3,567,332 pixels on one captured 2x
  frame. With the fix, all seven failure traces and an ordinary scene match native copies
  exactly at 1x and 2x (16 comparisons). A new live effect trace also matches at both scales;
  its resolve log confirms the two full-screen resolve-and-clear passes were exercised.
- A further 80-second live recording with repeated combos contains no scene blackouts.
  This is a check of the reproduced Springfield effects, not full campaign qualification.
- `tools/bench/copy_free_resolve_check.py` repeats supplied traces with native copies and
  image exchanges, requires identical pixels and retains logs, images and a JSON report.
  Use the same runtime as the replayer was built against. Traces contain game data and
  remain outside the repository. For example:

  ```sh
  python3 tools/bench/copy_free_resolve_check.py /tmp/resolve-clear-check \
    /path/to/effect.xtr /path/to/ordinary.xtr \
    --replayer /path/to/trace_reference --require-exchange
  ```

  NumPy is required, as for `replay_ab.py`. A single playback is insufficient: textures
  retained from earlier frames must exist for the deferred copy path to run. Live recordings
  remain necessary to check frame order as well.

### Character tessellation (2026-10-10)

- Optional Vulkan PN triangles curve supported skinned meshes from their world positions and
  normals. The shader analyzer proves the clip/world matrix relationship before adding the
  host control and evaluation stages. Other shaders, GPU-written strips and unsupported
  stage interfaces keep drawing normally. Off remains the default; Low, Medium and High are
  available in the launcher. This does not finish native GPU coverage.
- The evaluation shader uses clockwise tessellator ordering with Vulkan's upper-left domain
  origin to preserve the original triangle's winding. Counter-clockwise ordering had rendered
  back faces in place of front faces. Edge levels now depend only on their two endpoints,
  including camera-plane checks, so a third vertex behind the camera cannot give neighbouring
  patches different subdivision levels. Curved patches are clipped after evaluation rather
  than rejected using a guessed screen-space margin.
- Software Vulkan and the Deck replay the corrected High path without shader errors. Deck
  comparisons exercise all three levels on Springfield characters and High in Eighty Bites
  and Colossal Donut. The title frame remains pixel-identical with High and Off. These checks
  do not qualify the full campaign or native Windows drivers.
- A 60-second live recording at High contains 1,792 frames with no detected scene blackouts
  while running repeated combos. The live shader cache reports 87 prebuilt hits and no runtime
  translations. Tessellation's helper stages still compile on first use; keeping the host-only
  analyzer out of the guest translator hash preserves the shipped translated shader set.
- `tools/bench/smooth_tessellation_layout_check.cpp` uses synthetic disassembly to check the
  accepted position/normal layout and reject inconsistent matrices, conditional outputs,
  overwritten registers and jumps across the transforms. It needs no game data:

  ```sh
  clang++ -std=c++23 -O2 -Itools/rexglue-sdk/include \
    tools/bench/smooth_tessellation_layout_check.cpp \
    tools/rexglue-sdk/src/graphics/pipeline/shader/smooth_tessellation.cpp \
    -o /tmp/smooth-tessellation-check
  /tmp/smooth-tessellation-check
  ```
