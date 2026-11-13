# SToc streams and named resources

The reader and container rebuilder are in `modkit/src/streams.cpp`; the creator CLI is
`modkit_streams`. This corrects the earlier partial notes: header +16 is a relative pointer
to the entry table, not an entry stride. Every entry is 24 bytes. An entry can hold many
independent RenderWare resources.

## Stream header

The first 2,048 bytes are the header. Integers here are big-endian.

| Offset | Bytes | Meaning |
|---|---|---|
| `0x00` | 4 | `SToc` |
| `0x04` | 4 | Version 7 |
| `0x08` | 1 | Entry count |
| `0x09` | 1 | Flags; 4 in the checked installation |
| `0x0c` | 4 | Optional relative pointer to folder metadata |
| `0x10` | 4 | Relative offset of the entry table |

`sub_826C6E48` checks version 7 and relocates +12 and +16. `sub_826C6D20` indexes records
with `index * 24`. `sub_826C3668` reads the 2,048-byte header.

## Entry records

| Offset | Bytes | Meaning |
|---|---|---|
| `+0` | 4 | Allocation pool hash; preserve it |
| `+4` | 4 | Compression codec hash; preserve it |
| `+8` | 4 | Decoded resource bytes |
| `+12` | 4 | Allocation bytes |
| `+16` | 4 | Stored bytes, a multiple of 2,048 |
| `+20` | 4 | Read offset within the allocation |

Payloads start at file offset 2,048, in entry order. Each file offset is 2,048 plus preceding
stored sizes. `sub_826C3708` reads entry[+16] bytes into the buffer at entry[+20].
`sub_826C36D0` computes total size using the same stored-size sum.

`sub_826C6D20` resolves the +0 pool through `sub_826C5548` before allocating +12 bytes.
`sub_826C5978` resolves the +4 codec through `sub_826C58F8` before decoding. The checked
RefPack codec returns `0xB9F0B9EC` from `sub_8281E498`; `0x0EAC15C8` bypasses decompression.
These hashes describe allocation and compression, not the RenderWare chunk's asset type.
The stream tool exports `pool` and `codec`; `type` and `allocator` are retained only as
aliases for older development exports.

### Decoder entry points

`sub_8281E5B0` decodes a complete resource. Its arguments are input address in r3,
output address in r4 and an optional decoded-size pointer in r5. It returns 1 on success
and 0 on overlap failure. Its byte-forward copies require the output cursor below unread
input, and references repeat newly written bytes when their length exceeds their distance.

`sub_8281E8C0` reads the size header through the input-cursor pointer in r3, advances that
cursor and returns the declared decoded size. `sub_8281E9B0` is the asynchronous decoder:
r3 and r4 point to the input and output cursors; r5 is the requested output-byte budget.
It completes the current command before checking that budget, so a call can overshoot by
up to 1,030 bytes. Reaching the budget returns 0; consuming the terminator returns 1.
A zero budget advances neither cursor. Each call spills and restores r29-r31 and the
return address through `__savegprlr_29` and `__restgprlr_29`, without changing r1.

The normal private Linux boot/checkpoint run uses this asynchronous path. Instrumenting
only the whole-buffer routine records no decodes in that run. The creator reference tool
extracts both routines and the register-save helpers unchanged from generated sources;
each report records their source hashes. Both the original and re-encoded streams go
through the whole-buffer and incremental comparisons, including allocation guards.

The checked host implementation is in `modkit/src/refpack.h`, with ABI glue shared between
the oracle and `simpsons/src/refpack_native.cpp`. The asynchronous hook retains the declared
size from the original size-header reader and matches both cursors on subsequent steps.
A unique initial source transfers the header identity when `sub_826C5978` copies its
stack cursor into reader fields +8752/+8756. Ambiguous sources retain the original path.
A bounded generation cache follows jobs across threads; completed, changed or evicted
streams lose their records. Guest-memory restore clears host progress. The hook proves
committed read/write mappings, metadata separation, prior output history and the remaining
resource bounds before decoding. It retains the affected tail in a reusable per-thread
buffer, checks every command, and restores only the bytes actually written before falling
back on a rejected step. Small requests, oversized scratch requests, unsupported mappings
and unproved aliases retain the original routine. This is an asset-decoding optimization, separate from
GPU renderer coverage and typed-resource authoring.

Compressed entries use RefPack. Its size must match the index's decoded size. The codec
checks literal input, output lengths, reference distances and termination. Prefix reads
are for shallow indexing; full reads validate the complete decoded entry.

An unchanged rebuild preserves the complete header, packed entries and padding byte for
byte. Replacements are complete decoded entries. Compressed entries retain RefPack encoding
using bounded hash-chain matching and the three reference commands. Uncompressed
entries retain their original protocol. Stored bytes are sector-padded.
The encoder tracks output relative to consumed input and reserves aligned headroom for
in-place decoding; a fixed 32-byte offset would overwrite unread compressed commands.
Allocation includes the sector-padded input and is rounded to 256 bytes. Their
size/allocation/read-offset fields change; pool and codec hashes remain. Output
must be a separate file. Container editing does not implement typed mesh or level authoring.

## Preallocation catalogs

`StreamTOC` is a named resource in entry zero of the checked `*_global.str` files. Its
payload starts at resource +16 plus the big-endian header length at +12. The payload magic
is `0x9CBA7B28`, version 9. `sub_827285D0` relocates the catalog and `sub_82728420` finds
its 24-byte file records. Relative pointers are based on the payload start.

The payload has a path-prefix pointer at +8, a big-endian 16-bit record count at +16,
an allocation-pool count at +20 and a pool-table pointer at +24. Records begin at +28:
hash +0, relative file-name pointer +4, child/dependency counts +8/+9, block count +10,
flags +11, dependency pointers +16 and reservation-table pointer +20. Prefix plus record
name plus `.str` identifies the child archive. The catalog's own stream deliberately has
no preallocated blocks.

Each block reservation has three bytes. `sub_82728398` reads the first two as big-endian
and shifts them left eight bits; `sub_827283C0` reads the pool index from the third.
`sub_826C9D78` reserves buffers from this metadata through `sub_826C5780` before reading the
child header. Updating only the child's +12 allocation can therefore corrupt the heap.
The private failing text case reserved 149,504 bytes from the old catalog, then read the
new input to 149,888 bytes into that buffer. This happened before decompression.

`modkit/src/stream_catalog.cpp` checks the merged mount's winning headers and produces
bounded read patches for uncompressed catalogs. `modkit_filesystem.cpp` applies them to
complete and partial file reads. It never writes original files. A stock scan checks 20
catalogs and 2,656 block reservations with zero corrections. Entry-count changes and
unknown allocation pools need additional catalog authoring; they are rejected at startup.

## Named resources

RenderWare headers are 12 bytes, little-endian: ID, payload size and version. Walk those
lengths through a decoded entry. Named `0x716` chunks have a name at chunk +20. Nameless
chunks, including `0x722` entity/layer data, must also be retained.

`sub_82711B28`, registered through `sub_82711D38`, reads named resources. After the RenderWare
header it reads a big-endian header length. The payload begins at chunk +16 plus that length.
The padded name length at +16 leads to a 16-byte GUID. Relative fields following the GUID
lead through class/source metadata to an allocation header; its +8 is payload size. Padding
before the payload varies with alignment: size is not at a fixed distance before its magic.
`tools/modkit/translations.py` follows these pointers with bounds checks.

Build paths describe generic assets such as `lod3_model.dff`; they are never output paths.
Both `assets` and `assets_rws` paths occur. Looking only at each entry's first name misses
many resources, including text tables.

## LH2 text

Localized text begins with `32 48 43 4c` (`2HCL`). `sub_8282D618` relocates its string offsets;
`sub_8282D5B8` looks up a string by hash.

| Payload offset | Bytes | Meaning |
|---|---|---|
| `+4` | 4 | Payload size |
| `+8` | 4 | Flags; high bit selects wide text |
| `+12` | 4 | Optional names pointer; nonzero variants are not edited yet |
| `+16` | 4 | Hash count |
| `+20` | 4 | String column count |
| `+24`, `+28` | 4 each | Runtime pointer slots |
| `+32` | `count * 4` | Sorted hashes |
| following | `count * columns * 4` | Payload-relative string offsets, by column |

Checked global tables use one column. Dialogue uses two: text first, identifiers second.
The editor preserves identifiers. Narrow strings use Windows-1252 in the checked English
copy; wide strings use UTF-16 big-endian. Unknown variants are rejected. Rebuilding updates
the wrapper, allocation and LH2 sizes and reparses the result. Container and LH2 replacement
helpers remain experimental: native loader/alias behavior is not fully qualified.

Language packs use `locales/game-text.json` and `simpsons/src/modkit_text.cpp`, leaving archive
resources intact. The table lookup (`sub_8282D5B8`) returns a native-owned translated string
only when the loaded table hash, text ID and source string match. Hashing normalizes the
relocated pointer fields and string offsets to recover the original payload identity. It
happens once per used table, not per frame. `sub_8282D618` clears cached identities when a
table is loaded again; level changes and exact restores also invalidate them. Both narrow
and wide return paths retain their original encoding. Dynamic button tags and placeholders
must survive translation. New fonts and right-to-left layout remain separate work.

## Evidence and limits

The private October 19 scan parsed all 490 containers and decoded entry chunk lengths with
no errors: 2,753 entries and 26,384 chunks. English export found 574 LH2 locations, including
NPC and cutscene text; 555 distinct tables have 17,899 editable entries. These are counts for
the installed copy, not every regional release. No game data is included with the tools.

Synthetic fixtures cover RefPack commands and corrupt input, unchanged rebuild identity,
replacement round trips, multiple chunks, text resizing, identifiers and asset priority.
The optional native reference also checks re-encoding and in-place decoding with the game's
`sub_8281E5B0`: 2,549 packed entries and 2,719,195,380 decoded bytes across 490 local archives
agree byte for byte. The new encoder stores those entries in 1,071,785,984 padded bytes;
the originals use 1,060,644,864. This qualifies the codec against that routine, not the
whole resource loader. The startup failure in rebuilt-container controls was traced to
stale preallocation catalogs. Synchronizing those reservations fixes the private Linux
control; edited and resized global text also load through the native game lookup.
Reading all archives does not prove arbitrary edits load correctly. Live text rebuilds,
fonts and typed writers are qualified separately. Native entity streaming, rigs, meshes,
collision and navigation need their own adapters and acceptance projects.
