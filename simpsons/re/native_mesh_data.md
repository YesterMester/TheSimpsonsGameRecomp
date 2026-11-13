# EA preinstanced mesh data

Named chunk `0x716` wraps an `EARS_MESH` resource. Its body is a little-endian
RenderWare clump (`0x10`), containing a GeometryList (`0x1A`) and Geometry (`0x0F`)
records. The big-endian named wrapper is shared with the
[MMdl reader](model_definitions.md). Generic RenderWare DFF documentation does
not describe the EA native plugin used by these Xbox 360 files.

## Native allocation and relocation

`sub_8282F970` registers geometry plugin `0xEA33`; its plugin offset is retained
at `0x82E06F7C`. The registered reader, writer and size routines are respectively
`sub_8282F618`, `sub_8282F788` and `sub_8282F848`. The reader consumes a 12-byte
little-endian allocation header: alignment, main size and secondary size. The
main and secondary bytes follow. Unknown contents are preserved.

The main header is big-endian:

| Offset | Field |
| --- | --- |
| 0 | Magic `0xBFBFBFBF` |
| 4 | Version `0x01000000` |
| 8, 12 | Main and secondary sizes |
| 20, 24 | Relocation and section counts |
| 28 | Eight-byte relocation records, followed by twelve-byte section records |

Each relocation gives a field offset and target block kind, 0 for main and 1
for secondary. `sub_82831280` adds the selected base to the field's stored
offset, including zero offsets. `sub_828312D0` copies both blocks and rebases the
same fields from their old allocations to the new ones. The host reader bounds
all tables, rejects duplicate relocation fields and preserves secondary data.

`tools/modkit/mesh_reference.py` extracts those routines without changing their
operations. The fixture's memcpy and synthetic heap are explicit boundaries.
Full block bytes and surrounding guards are compared for both operations.

## Reviewed position stream

The current writer accepts a single `0x3C43A23D` section of 56 bytes. Its +12
pointer selects the combined vertex/index allocation record. The original
initializer `sub_8273B760` uses that record to create the vertex buffer at +56,
index buffer at +88 and declaration at +48. It does not iterate the adjacent
section words as arrays of allocation records. Other drawing layouts still need
their own decoder and acceptance.

| Allocation offset | Field |
| --- | --- |
| 0, 4 | Vertex byte size and stride |
| 8, 12 | Declaration count and pointer |
| 16 | Vertex bytes pointer |
| 20, 24, 28 | Index byte size, format and bytes pointer |
| 32 | Native sphere, four big-endian floats |

A declaration entry has twelve bytes: stream/offset, native type and semantic.
Stream 0, semantic 0, type `0x002A23B9` supplies three big-endian position floats.
The native vertex count is byte size divided by stride. It may differ from the
Geometry structure's earlier source count. Unreviewed attributes are preserved.

The reviewed index path uses 16-bit big-endian indices, the supported strip
record and restart value `0xFFFF`. Preview triangles preserve strip parity and
omit degenerates. Other draw counts, sections or indexing layouts stay read-only.
The Geometry structure supplies its little-endian sphere; plugin `0xEA16`
supplies the reviewed box. Plugin `0x116` marks skinning and prevents live edits.

Requested position, normal and UV bytes can change. Vertex counts, indexing,
bounds, materials, skinning and unknown bytes retain their original layout.
Position and normal editing remain restricted to reviewed static geometry.

## Packed static normals

Stream 0, semantic `0x00030000`, declaration type `0x002A2187` supplies a
big-endian packed normal. XYZ occupy signed ten-bit fields starting at bits
0, 10 and 20. Decode each with division by 511 and clamp the alternate -512
encoding to -1. The upper two bits are preserved. This agrees with the signed
normalized `k_2_10_10_10` fetch representation in `graphics/xenos.h` and
`graphics/pipeline/shader/interpreter.cpp`; `sub_82701BD8` forwards the original
declaration to the native vertex declaration constructor.

The writer requires the reviewed non-overlapping stream, static geometry and
unit source normals. Authored directions allow 0.5 percent length tolerance for
quantized source values. Components stay within [-1, 1]. Changed components are
rounded to signed ten-bit values; unchanged decoded components retain their
original encoding, including the second representation of -1. A no-op export,
OBJ import and native write of the real coffee table preserves every byte.

All 1,881,360 normals across 2,825 supported static geometries in the private
inventory round-trip bit-exactly. The coffee-table probe also compares decoded
directions with all 498 exported triangle windings; every alignment is positive.
Other declarations remain read-only. The optional OBJ normal path preserves
existing hard-edge splits and rejects conflicting face-corner directions for a
shared vertex. It does not change tangents, UVs, collision or topology.

## Primary texture coordinates

Stream 0, semantic `0x00050000`, declaration type `0x002C23A5` supplies two
big-endian float32 texture coordinates. The format's low six bits select
`k_32_32_FLOAT`, as decoded by the native fetch paths. Before exposing UV edits,
the reader checks the full declaration: the eight-byte slot must fit the stride
and remain separate from every other attribute. Unknown widths, overlapping
slots and nonfinite source UVs keep that stream read-only.

UV-only writes do not require position or normal editing. They can therefore
use reviewed skinned geometry while preserving all bone weights, alternate
position streams, culling data and material bindings. Input values stay finite
float32, without a 0–1 clamp or orientation conversion. Other UV channels remain
unchanged. Optional attribute arrays must agree on vertex count and use the
original geometry IDs; unknown attributes reject during package validation.

The actual writer round-trips 6,436,832 UV pairs across 15,321 geometries and
3,313 resources with complete resource-byte equality. Of those geometries,
12,176 are skinned. Existing position and normal acceptance counts remain
3,145 and 2,825. The real Frink OBJ/native no-op also preserves every byte.

A private Linux Vulkan Frink edit shifts U for 763 vertices. The live native
record has stride 48 and UV offset 16. All 36,624 bytes match an independent
expected stream: only the requested UV bytes differ. Positions, normals,
weights, indices and other data remain intact. The owned NPC binds that same
geometry, renders in two views and retains its input stream after simulation.
Removal restores 798 to 797 objects. The initial close hit the existing audio
pause stall; a repeat with the corrected audio worker exits 0. That audio-only
test uses the same pinned renderer, with no installed build changes.

## Loader integration

The existing strong `sub_82711B28` wrapper applies a prepared mesh edit before
calling its original named-resource reader once. The input stream has cursor at
+12 and buffer at +20. At this point the cursor selects the resource header,
twelve bytes after the enclosing chunk start. The adapter verifies committed
readable/writable storage, the complete chunk, GUID, exact source SHA-256 and
all edits before copying the validated same-size result. Optional failures are
contained outside the original call and preserve normal name observation.

The game then creates and binds its own original resources. No replacement GPU
allocation or borrowed package buffer is retained. A private Linux Vulkan run
verified all 363 edited coffee-table vertices in the registered native block,
the same geometry in an owned spawned prop, rendered views, balanced removal
and exit 0. A second nonuniformly reshaped table run verifies all 363 native
positions and authored normals, including preserved W bits, rendered views,
balanced removal and clean exit. The offline inventory covers 5,533 meshes and
20,490 native blocks.
Broader deformation, multipart draws, native Windows and Pixel acceptance remain
separate work.
