# Direct3D 12 native resource work

The Windows backend is selected with `gpu = "d3d12"`. The 0.0.7.0 work moves
proven draw and texture inputs into resource-local buffers and resolves supported
color and depth attachments directly from host images. Compatibility paths remain; this
does not establish full native rendering coverage.

The October 16 checks add native fractional/reversed inputs, larger retained
GPU indices, sparse/wrapped resolve aliases and transfers between original and
scaled targets. Both binding modes pass the new resolve, retained-depth and
saved-frame checks without compatibility dependencies or allocations. DX12
shares the transfer shader generator with DX11; its 594-case patterned
mixed-resolution fixture exercises the common addressing and packing code.
These checks use the Deck GPU through Proton. The [shared native state](native_renderer_plan.md)
records the evidence and remaining campaign and hardware limits.

The shared input proof no longer stops at 22-bit indices. Both binding modes
pass large-index frame comparisons up to the 24-bit limit at 1x/2x. The shared
extent calculation also permits physically valid reads beyond the declared
stream length. When vertex or index data has a GPU owner, it keeps conservative
attachment bounds instead of estimating them from stale CPU memory.

## Implemented

- CPU-owned texture base levels and mips upload into compact decode-input
  buffers, preserving the existing GPU format conversion, tiling, endian,
  array and volume handling. They bypass the physical-address mirror and retain
  the common texture write watches. Submission completion controls buffer reuse.
- Ordinary CPU-owned DMA indices use the common native index snapshot path.
- Proven CPU-owned vertex streams upload only the words addressed by the draw.
  Index bounds, fetch expressions, strides and stream lengths must prove every
  access. Private fetch constants rebase those accesses into the compact raw
  buffer without changing guest registers or translated shaders. Frame
  completion controls buffer reuse. Unproved expressions, unretained GPU streams,
  pixel-stage vertex fetches, overlapping texture constants, and unproved export
  or tessellation cases keep the compatibility path.
- Bindless shared-memory SRV and UAV bindings use separate root tables, keeping
  native SRVs in the SRV descriptor region. Bindful draws retain separate native
  descriptor groups, including the EDRAM UAV when required.
- Compatible single-sampled RGBA8/RGB10A2 attachments pack directly into the
  tiled resolve destination. The shared host shader preserves endian, channel
  swaps and scaled 16-byte group addressing. This bypasses host-image encoding
  into EDRAM and the following EDRAM resolve copy. Shader compilation or pipeline
  creation failure retains the precompiled general resolve path.
- Matching cached textures can receive a direct image copy from the attachment,
  avoiding another decode from resolved memory. Channel swaps update both SRV
  mappings and binding cache keys; normal texture watches track later writes.
- Original-resolution resolve data stays in page-owned native buffers. Preimages
  and decode inputs combine retained GPU owners and unchanged CPU gaps directly.
  CPU writes invalidate individual pages; compatibility consumers materialize
  their requested pages. Cache clearing retains authoritative GPU data, and a
  write during publication preserves the pending GPU version for the tracker.
- Scaled resolves use compact GPU buffers instead of a sparse physical-address
  mirror. Overlapping allocations preserve their previous bytes on the GPU;
  old buffers survive until their submission fences complete. Independent
  neighboring textures remain separate, and untouched padding starts at zero.
- D24S8/D24FS8 depth and stencil pack directly from native image views, including
  the original float24 conversion and rounding. Native ownership transfers
  preserve sample addresses for MSAA layout aliases and depth-to-RGB10A2 aliases.
- Host render targets allocate EDRAM and the physical-address buffer only for an
  actual compatibility consumer. Bindless shared-memory descriptors use reserved
  slots written once after allocation; queued frames keep their original null
  slots. Bindful draws create fresh copies before binding a newly needed buffer.
  EDRAM compute views are retained for their submission. Native original-resolution
  readbacks copy their retained buffer directly.
- Native depth packing binds depth and stencil through separate tables. One-use
  bindless descriptors can come from independently recycled slots and must not
  overwrite the slot following another view. Changing either bindful descriptor
  heap invalidates the guest root binding cache, so the next draw restores every
  table, following the [descriptor heap state rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps).
  The menu/Springfield frame comparisons pass again in both binding modes
  at 1x and 2x after these lifetime and binding changes.
- `native_gpu_diagnostics` now records Direct3D 12 operations, compatibility
  dependencies and actual EDRAM/mirror allocations. Trace capture invalidates
  compact CPU texture inputs so their data is included in the trace.
- Compatibility memory-export draws allocate their buffer before copying
  bindful descriptors. Accurate CPU readback and subsequent presentation pass
  18 command-processor cases in each binding mode, with 37,748,736 exact bytes
  per mode. This preserves the remaining compatibility export path; it does not
  establish native export ownership.

`d3d12_native_texture_uploads`, `d3d12_native_vertex_buffers` and
`d3d12_native_index_buffers` default to `true` and require a restart.
`d3d12_native_resolve_memory` and `d3d12_native_scaled_resolves` also default to
`true` and require a restart. `d3d12_native_depth_resolves` defaults to `true`.
`direct_host_resolve` enables the supported attachment resolve and texture-copy
paths. `d3d12_bindless = false` selects the bindful binding implementation.

## Rendering evidence

Qualification used LLVM-MinGW and Proton 10/VKD3D on a Steam Deck. All nine
Direct3D 12 graphics translation units and eight Direct3D 12 UI translation units
compiled, and a private Windows runtime and game linked. A private MinGW header
supplied the Microsoft DXC/PIX interface UUIDs missing from the toolchain's
template declarations. The portable device GUID lookup also fixes the
provider's pre-existing MinGW compilation error.

Live captures covered the intro video, storage/save menus, Springfield, walking
and pause. Compact vertex streams, host-image resolve packing and cached texture
image copies executed in the live game.

Two captured frames, a saved-game menu and Springfield, pass strict replay at
1x and 2x in both binding modes. All eight match the previously qualified
frames exactly: 55,296,000 RGB bytes compared, with zero EDRAM, physical-mirror,
scaled-mirror, vertex, texture or resolve dependencies and zero compatibility
allocations. The captures render at 1280x720 and 2560x1440. Four forced general
path comparisons also match, exercising allocation and descriptor setup when
those resources really are needed.

Four more comparisons cover Eighty Bites and the original attack-flash capture
at 1x/2x. Their native frames match the general resolve path exactly and report
zero compatibility dependencies and allocations. Native GPU texture loads also
arm CPU callbacks for their full sampled range, including any CPU-owned input
levels or padding, without marking the physical-address mirror current.
Five successive submissions of the attack capture also match the same repeated
sequence through the general path in both binding modes, with zero native
compatibility dependencies or allocations throughout.

The same 1x bindless frames produced these diagnostics:

| Measurement | Menu compatibility | Menu native | Springfield compatibility | Springfield native |
|---|---:|---:|---:|---:|
| Physical-address mirror uses | 457 | 0 | 2,152 | 0 |
| Resident physical-address mirror memory | 48 MiB | 0 | 192 MiB | 0 |
| Compact native vertex draws | 0 | 215 | 0 | 682 |
| Remaining vertex fallback draws | 215 | 0 | 682 | 0 |
| CPU texture loads bypassing the mirror | 0 | 11 | 0 | 77 |
| EDRAM buffer uses | 4 | 0 | 20 | 0 |

These are resource and dependency measurements from the same frame, not FPS
claims. The shared packing shaders pass the Direct3D 11 GPU fixtures: 240 colour
dispatches and 62,914,560 exact destination bytes, plus 136 depth dispatches and
35,651,584 exact bytes. These include padding, endian/channel combinations,
float24 rounding, stencil and symmetric/asymmetric scales. The shared vertex
address proof passes 1,440 cases and 495,840 independently evaluated float
operations, with 17 unproved inputs rejected.

Four native depth-to-colour alias comparisons also match their general paths
at both scales, including quadrant patterns and multisampled source images.
A private 1x live run covers startup, menus, saves, Springfield, movement,
attacks and pause/resume with zero compatibility dependencies or allocations;
a 2x title run records zero too. These runs close cleanly with no renderer errors.

Local qualification artifacts are under
`/home/deck/simpsons-test/native-d3d12-0070`:

- `game/librexruntime.dll` and `game/simpsons_native_check.exe`: isolated runtime
  and executable; the installed game and runtime are unchanged.
- `game/dx12-*.png` and `game/logs/simpsons_001.log`: live captures and execution
  logs.
- `trace-check/results.json`: exact pixel comparisons.
- `trace-check/coverage-summary.json` and per-case JSON reports: native operations,
  remaining dependencies and allocations.
- `check_traces.py`, `compile_windows.py`, `compile_ui.py` and `link_private.py`:
  private qualification commands.

The newer ownership, depth, alias and strict frame reports are under
`/home/deck/simpsons-test/0070-20261011/d3d12-owned`. The `native-final-check`,
`fallback-final-check-corrected`, `alias-all-backends` and `native-final-live-*`
folders contain the corresponding captures, logs and cumulative diagnostics.

Both binding modes also pass the 42-case shared native geometry check:
88,080,384 exact output bytes and 42 presented frames per mode, with zero
compatibility dependencies or allocations. It covers built-in auto-index
adapters and cached CPU conversions as well as native GPU writers. Vertex bounds
scan the retained CPU index shadow using the converted buffer's byte order.

Retained GPU-produced DMA indices now use immutable compact readbacks for
conversion and bounds proofs. CPU and GPU writes invalidate cached versions.
The geometry check passes 42 cases and 88,080,384 exact bytes in each binding
mode, including repeated GPU writes and CPU ownership changes, with zero
compatibility dependencies or allocations through Proton. Streams without a
retained native owner still need coverage.

## Remaining work

Native resolve coverage currently requires one complete owner, the original
source base and pitch, origin-aligned rectangles, 1x sampling and matching
RGBA8/RGB10A2 or depth/stencil storage after any supported alias transfer.
Partial/multiple-owner resolves, other formats, sample averaging, exponent bias
and array destinations retain the general path.
Proven memory exports and vertex inputs now use retained native buffers, including
GPU-written inputs and CPU gaps. Dynamic fetches, unretained GPU index streams,
modified export addresses and some ownership transfers still need coverage.
Original-resolution native storage is bounded to 128 MiB; unavailable owners or
allocation pressure retain the compatibility path. Scaled allocations preserve
the existing scaled texture semantics and need more mixed CPU/GPU alias coverage.

Native Windows drivers, the rest of the campaign and unsupported format/alias
cases still need qualification. The existing correctness paths remain active
for those cases; native diagnostics must continue to report their dependencies.
