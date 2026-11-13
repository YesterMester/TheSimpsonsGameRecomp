# Native GPU call conversion

The resource-local renderers and the game's graphics API boundary are separate
pieces of work. Native textures, vertices and resolves do not by themselves
replace the game's command writers. This conversion replaces verified writers
with C++ producers and routes their arguments to the existing host backends.
It requires a restart and is enabled by default in 0.0.7.0
(`native_gpu_calls = true`). Unconverted paths retain their compatibility
fallbacks; the conversion is still incomplete.

## Converted producers

| Function | Contract | Native representation |
|---|---|---|
| `824626B8`, `82464590` | Bounded auto-index draw builder | Fetch descriptor, coherency, flush state and draw arguments |
| `82462810`, `824646E8` | Bounded DMA-index draw builder | The same state plus index address, size and endian mode |
| `8245EC68` | Aligned groups of VS or PS float constants | Raw constant bytes and a validated register range |
| `82456B58` | Owned default-heap temporary allocation and command chunks with a ready GPU fence | Original alignment, reservation limits, ring wrap, generation changes and failure effects |
| `82458E60`, `82456D60` | Fixed-recording storage and bounded default/fixed temporary allocation dispatch | Retained owners, capacity reduction, wrapped byte accounting, budget throttling and sticky failure |
| `824578C0` | Default/fixed command-chunk growth with proved allocation and marker readiness | Original publication, scratch fallback, private frames, pending-marker clearing and 64 KiB ready clamp |
| `82457210` | Completion records in an owned physical default-heap command span | One native invalidation and two native shader events, preserving payloads, offsets and the original CPU-mirror mode |
| `82457B40` | Bounded normal, fixed-recording, reserved-list and failed-buffer chunk closure | Completion before real ring publication, owned descriptors, original alignment/return ABI and preplanned native growth |
| `82457CC8` | Shared recorded/reserved chunk dispatch without a blocking wait | Native closure/growth, completed-marker early return and the existing command-storage guard |
| `82457F50` | Owned word reservations, including a completely planned close/growth continuation | Original word count, returned cursor, private frames and preserved argument high halves |
| `82457050` | Submission coherency state with ready default-heap or fixed-recording storage | Atomic dirty-range consumption and native cache-flush records with original operands and editing offsets |
| `82457748` | Descriptor-list expansion through a retained default/fixed allocator, including scratch failure | Original allocation size, encoded links, ordered header publication and list metadata |
| `8245DA50` | Bounded CPU command-cache range and its original early return | Constant-time final guest stack/register state and the same host memory barrier |
| `824565A8`, `82456E68`, `824572F8` | Ready primary-ring space, bounded publication and queued/nonqueued submission | Deferred buffer records, real SDK queue locking/recheck, original wrapped slots, barriers and MMIO publication |
| `82456A90` | Linked command-buffer append with retained node/tail owners | Encoded physical links, ordered metadata, cache barriers and volatile tail publication |
| `824564D0`, `824574B8` | Allocation-fence and marker wait orchestration over proved writeback storage | Live completion predicates and wrapping arithmetic, retaining real timing, sleeping polls, abort/timeout handling, callbacks and required dispatch |
| `8245F810` | Complete bounded shader-descriptor constant loop with ready command capacity | Planned native indirect records, deferred physical-source reads and original per-record publication |
| `8245E330` | Bounded depth/surface compatibility preparation | Original shadow updates and pending flags, native event-initiator scalar or ordered wait-port write |
| `8245F6B0` | Ready-buffer inline resource-shader setup with separate code, metadata and patch owners | Native reserve proof, invalidation/binding, code copy, export/declaration patches, barriers and ordered publication |
| `8245F3D8`, `8245F4A8`, `8245F560` | Bounded shader-export linkage and descriptor merge | Native C++ instruction patches, original list order and export-count bits |
| `8245EDC8` | Bounded vertex-declaration fetch-instruction patching | Native format/swizzle conversion, original pairwise ordering and fetch coalescing |
| `8245F8C0` | Bounded shader-resource version/cache and in-flight fence handling | Native cache reuse, declaration rebuild, shared atomic version allocation and ordered snapshots |
| `82445278`, `82445578`, `824408E0`, `82445798` | Bounded pixel/vertex shader, texture and declaration binding | Native shadow patches, dirty bits, sampler/address conversion and owned resource retirement |
| `82445240`, `82445480`, `824457F0` | Bounded shader/declaration resource initialization | Original headers, clear sizes, variant instruction templates, attributes and stream masks |
| `8245E8C8` | Prepared dirty render/bool/loop state runs with sufficient command capacity | Inline state ranges with original lengths and payload positions |
| `8245EB08` | Prepared dirty vertex/texture fetch runs with sufficient command capacity | Inline fetch descriptors, preserved padding and the original flush payload |
| `8245E988` | Sampler-state flush with up to 30 slots and sufficient command capacity | Ordered native coherency/wait and aligned four-word sampler ranges |
| `8245E530` | Bounded conditional state fixups and per-stream mask selection | Preserved mask halves, selected state writes and original restoration order |
| `82444700` | Initialization shader/state buffer with a separate, reserved output span | Original 24-word VS, 9-word PS, invalidation mask and thirteen state ranges |
| `824449E8` | Bounded initialization GPR-management buffer | Ordered wait-port update, eight repeated GPR-port writes and the final GPR update |
| `82444A80` | Initialization buffers from the original game heap, with separate backing spans | Native setup, 97 GPR buffers, 24 warmup draws and unchanged descriptors/alignment |
| `82463E60` | Retirement chunk construction in a separate, bounded 384-byte ring block | Fetch/flush ranges, five-point draw, short coherency wait and shader-completion event |
| `824617D8`, `824618F8` | Shader counter events in a ready normal ring, including the slot helper's no-event branches | Ordered native completion event, original counter/endian operands and slot arithmetic |
| `8244C450` | Normal-mode BeginVertices with supported state flushes and the default non-wrapping allocator or fixed-owner recording | Ordered state prefix, deferred vertex state and bin-predicated draw |
| `8244C8F0` | EndVertices cursor publication | Publish after the caller has filled the returned vertex memory |
| `8244C970`, `8244CEA0` | Normal-mode indexed Begin/EndVertices with supported state flushes and two default or fixed-recording allocations | Ordered state prefix, deferred vertex/index buffers, fetch state and bin-predicated indexed draw |
| `8244CF78`, `8244D360` | DrawPrimitive/DrawIndexed with supported shadow flushes, at most 65,535 vertices/indices | Ordered constants/state/fetch/bool values, starting vertex and auto/DMA draw arguments |
| `8244D7B8`, `8244DC30` | Bounded vertex-ID draws, including supported indexed expansion and shadow flushes | Original lookup-table values, ordered state and explicit-major auto/DMA draw arguments |
| `82444A48` | Initialization helper building 24 one-vertex draws | Bare auto-index records, preserving the returned size |
| `8246AEF0` | Prepared triangle-list draw | Whole-call bin predication and unchanged index-count truncation |
| `824529C0` | Rectangle clear utility, 1–21,845 rectangles, ready default allocator and sufficient ring space | Native vertex data, six inline state ranges and a predicated rectangle draw |
| `8246AF58` | Copy utility with sufficient capacity at every growth boundary | Inline shader binding, state/fetch ranges, predicated rectangle draw and ordered coherency/wait |
| `82455570` | Prepared rectangle draw after the original clear geometry, with normal draw-header flags | Fetch/flush ranges and a three-vertex rectangle draw; original setup/restoration retained |
| `8246C750` | Bounded emission of each original copy-draw split iteration | Unconditional vertex offset and predicated point draw, retaining the split loop and ring growth |

The draw hooks share the existing attribution hooks, avoiding duplicate strong
overrides. The constant hook falls back to the whole original routine when its
preconditions are not met. Preflight checks happen before modifying memory or
the PPC context. Draws and constants share backing-range checks for physical
aliases and the XEX views, so different virtual addresses cannot hide an
overlap with the writer's own inputs. The runtime capability handshake prevents
production during startup or shutdown without a consumer.

BeginVertices retains the real allocator chain (`82456D60`, `82456B58`,
`824564D0`, or `82458E60` for fixed recordings) and its allocation failures,
counters and fence behavior. The
native branch requires a ready default heap or proved fixed recording and
sufficient ring space. Prepared
calls keep a short path; supported dirty shadows use the shared whole-flush
planner. Shader/declaration/resource fixups, custom heaps, wrapping allocations
and the special bin/screen-extent path retain the original call before modifying
memory or allocating. The draw stays unpublished until the caller finishes
writing the returned memory.
Indexed Begin retains both allocator calls and both output pointers. It checks
the two allocations together, including a budget transition that could reset the
heap between them. Fixed recordings preflight their downward allocator and the
resulting reduction in command capacity, including both indexed allocations.
Unsupported cases return before either allocation. Caller
vertex/index data stays unpublished until indexed End. Prepared range draws do
not change fetch constants, flush state or coherency; large draws that split
according to topology retain the original routine.
EndVertices preserves the original cursor, register and empty-tail contract;
the qualification tool rejects a change that gives that tail new behavior.

Dirty Begin plans the entire state prefix and deferred draw together with every
allocation, fence and output-pointer range. A buffer that only overlaps the
prefix must still be rejected. The preflight also protects the allocator's
deeper private frames. Auto Begin discards the old temporary vertex-fetch dirty
bit; indexed Begin flushes it. Supported flushes publish in the original order
before allocation. An actual allocation failure preserves that completed prefix
and returns the original error; it never retries the original routine. A
successful Begin still leaves the draw deferred until fill/End. Existing
prepared calls remain usable when an older runtime lacks the new state or
sampler capabilities.

Fixed recordings keep their original storage and active BeginRecording state.
Their command span, metadata, temporary vertex/index buffers, output pointers
and private frames must have proved, separate backing ranges. Both allocations
are checked before any prefix is flushed. The real downward allocator still
handles alignment, exhaustion, the failure flag, budget wrap and ring-limit
updates. A failed second allocation preserves the first allocation's effects
and returns the original failure without publishing a draw. Callback storage,
unproved owners and heap transitions between indexed allocations retain the
original routine before side effects. The existing native vertices consumer
handles the resulting calls on all three host backends.

Begin and range flushes also handle the conditional state helper. It keeps
per-stream HIZ state, surface information and colour-mask fixups, both halves
of the bin mask, the original signed stream-count behavior and restoration
order. Up to 453 source words fit within the verified device object; larger
positive counts and ring growth retain the original helper. A compact snapshot
keeps the native plan at 1,976 bytes without rereading mutable command data.
The entire conditional prefix and final draw are preflighted together.

The rectangle utility retains its real allocation, half-pixel source, rounding
steps, colour/depth conversions and dirty-state updates. It produces three
28-byte vertices per rectangle, then preserves the original 88-byte command
span. Allocation failures emit no draw. Unsupported allocator/ring conditions,
overlapping backing ranges and counts beyond the 16-bit vertex limit retain the
original function before side effects. This converts the bounded utility;
other branches of the main clear/resolve API remain separate work.

Ordinary range draws can also flush supported dirty shadows. One preflight
plans both float-constant banks, render-state pages, fetch descriptors, supported
samplers and bool/loop values before publishing any of them. It retains the original helper
order, alignment, flush payload and dirty-flag clearing. Resource-dependent
state rewrites, shader/declaration fixups and ring growth keep the whole original
call. Samplers require an additional runtime capability; without it, the entire
call retains the original path. The final draw and index metadata are included in the
same overlap/capacity checks, so a later rejection cannot leave a partial flush.

The vertex-ID producers use that preflight after planning their forced shadow
updates. They read the game's own primitive tables and preserve the separate
16-bit, 32-bit and expanded indexed layouts. Expanded counts must fit 16 bits.
Their normal-mode branch keeps the explicit-major draw bit, index endianness,
physical-address conversion and original operand offsets. Special bin/extent
records, large split draws and unsupported state retain the original routine.

The copy utility keeps its inline 15-word vertex shader, copy mode, pitch,
physical addresses and size-rounding behavior. Mode 3 keeps the original
destination-only update; other modes retain the real coherency wait. Its
complete span is checked against every growth boundary, the device, private
stack and aliased shader source before any write. Cursor publications and
operand offsets remain intact. Ring growth retains the original function.

Retirement keeps the original ring scheduling, wrap, polling, lock/CAS,
submission, fence and retry code. A small hand patch calls the native writer
after the original wait and before lock/submission. It preflights the entire
384-byte block against its descriptor, owner, private stack and global pointer
backing ranges before writing. Unsupported spans continue the original
construction at that point, without repeating the wait or any allocation.
Accepted calls retain all 28 command words, five float values, completion
operands, padding and the full context expected by the continuation.

The main clear and copy routines also retain their orchestration. Their small
construction hooks run after geometry/allocation or ring growth. The clear
writer keeps the 52-byte fetch/flush/draw span and supports normal header flags
with either bin-predicate value. The copy writer keeps each 16-byte offset/draw
span, including zero-count behavior, the 65,534 split size for counts above
65,535, offset wrap and the original loop. State remains unconditional when a
draw is rejected. Register-locals builds refresh the copy loop's branch field
after the checkpoint. Other state, shader, resource and restoration writes in
these large routines are separate conversion work.

The shader-resource constant helper now plans its whole bounded descriptor loop
before publishing any records. It handles canonical float indices and lengths
within the 2,048-word register bank, matching physical source views and separate
command/metadata/frame ranges. Each four-word record retains its pointer,
index and length offsets and its original cursor publication. Source bytes
remain deferred command-thread reads through the existing ALU range writer and
trace-memory capture. Ring growth, larger tables and unsupported operands retain
the original function before effects; its per-record checkpoint remains available
to convert supported records on that fallback path.

The original resource-setup fallback and the copy routine's vertex/pixel setup
also use native program prefixes. They reserve the complete inline-code span, preserve the
original stages, lengths and invalidation masks, and leave memcpy, synchronization,
shader fixups and publication in the original caller. The resource path checks
the metadata it will reread and the physical code-copy source against the whole
output span. Preflight includes memcpy's private eight-byte stack spill. The
copy routine keeps both its 525/933-word vertex variants and 15-word pixel code
at their original offsets. Oversized, unaligned, overlapping or unproved spans
retain the original construction after the real reservation.
The complete ready-buffer resource path is now converted and qualified
separately below; these checkpoints still serve its fallback and the copy routine.

## Ordering and recorded buffers

An immediate host draw on the calling game thread would break asynchronous
submission and recorded command buffers. A temporary side queue also fails when
the game copies or replays a buffer. The pilot puts the arguments inside the
buffer that already carries their ordering and lifetime:

- `0x7D` is the versioned full draw call (`DRW1`). Its 24- or 27-dword span
  preserves every original operand offset used by patch tables. Old packet
  headers become validated native metadata; it emits no original draw packet.
- `0x7C` identifies a float-constant range in the header itself, preserving
  alignment padding, payload bytes, patch offsets and buffer advancement.
- `0x7B` is a bare auto-index call for an already prepared state. Its consumer
  is qualified by fixtures; the four game draw builders use the full record.
- `0x7A` is the deferred vertices call (`VTX1`/`VTX2`). Its 16/18-word spans keep the
  original operand offsets. State updates are unconditional, while only the
  draw obeys bin predication. Rejecting the draw must still apply that state.
  A separate runtime capability bit guards this producer.
- `0x79` is the prepared range call (`RNG1`), with five or seven words for
  auto/DMA indices. Its index offset is unconditional and its draw is bin
  predicated. Separate capabilities guard indexed vertices and ranges.
- `0x72` through `0x78` identify render-state pages `0x2000` through `0x2300`,
  fetch descriptors `0x4800`, bool/loop constants `0x4900`, and flush state
  `0x5000`. The ordinary type-3 word count stays intact. Header bits 1–7 and
  15 encode the starting register within its page; bit 0 stays clear because
  these updates are unconditional. Register bounds are validated per page.
  The consumer uses the backend's invalidating range writer, with sequential
  writes for flush state. No command-processor object layout changes are needed.
- `0x71` is the 13-word coherency call (`COH1`). It keeps the original destination,
  status, size and base operands and fixed register-wait contract at their
  original offsets. The consumer writes those four registers and calls the
  same wait implementation as the original command path. It remains
  unconditional when bin predication rejects the following draw. A separate
  capability prevents new producers from using an older consumer.
- `0x70` identifies inline shader bindings and state-invalidation markers.
  Canonical tags preserve the stage, size, code and mask offsets, and reject
  predicated or reserved headers before changing backend state. The shader
  consumer uses the real backend loader/cache and updates the active shader;
  it does not remove runtime translation of an uncached shader. Invalidation
  retains the original mask and the existing backend semantics. Both operations
  are unconditional and require a separate producer capability.
- `0x6F` identifies unconditional low/high bin-mask updates and a selected
  state write. It preserves every operand offset and validates its canonical
  header and supported register. The selected write uses the real bin predicate
  and backend range callback; a rejected write changes neither state nor the
  guest write epoch. Mask updates preserve the other half. An independent
  capability guards this producer.
- `0x6E` identifies the initialization register ports. It retains the original
  two- or nine-word spans and calls the shared per-register writer for every
  operand, including repeated updates to the same port. Headers are validated
  before bin predication, and the writes remain unconditional. A separate
  capability guards these producers; this does not introduce a new wait policy.
- `0x6D` identifies the shorter retirement coherency call and shader-completion
  event. The eleven-word coherency span preserves the status/size/base offsets
  and real wait, without changing the destination register. The four-word event
  uses the same event writer as the original path, including all endian modes,
  counter selection, memory tracing and the guest-visible write epoch. Both
  calls are unconditional, validate their headers before bin predication and
  require a separate runtime capability.
- `0x6C` identifies canonical indirect ALU constants. It validates the fixed
  four-word layout, aligned physical source and register/memory bounds before
  reading source bytes or changing state. It is unconditional, preserves raw
  float bits and leaves the guest write epoch unchanged. Its independent
  capability protects producers from an older consumer.

The state/fetch writers plan every run before writing. Slow buffer growth,
unsupported register ranges and overlapping backing views keep the original
routine. The fetch writer preserves the original alignment and the fast path's
exact flush payload (`0, 0x25000, 0`); its middle word is data. Copied command
buffers retain all of their arguments, lengths and patch positions.

The sampler writer preserves its coherency preamble before copying aligned
four-word sampler slots. Its standalone branch supports only ranges within the
validated register page. The whole-flush planner also handles the six dirty
sampler bits used by the four ordinary/vertex-ID range families. Slow growth,
unsupported bits and backing-view overlaps retain the original routine before
publication. The shared wait retains its polling, trace, shutdown and timing
behavior; its command decoding is separated from that implementation. No
command-processor fields or virtual functions are added.

The consumer updates the shared backend state with the existing invalidation
callbacks and invokes `IssueDraw` through the same dispatch used by normal
draws. The new transport is intermediate: it still uses the existing command
buffer submission infrastructure and backend state representation. It is not a
complete replacement of the graphics API or command processor.

DMA addresses retain two-byte alignment for 16-bit indices and four-byte
alignment for 32-bit indices. Native calls and constant updates do not advance
the guest write epoch merely because they have new opcodes. Actual backend
copies and writes retain their existing epoch changes.

Trace disassembly, draw boundaries, draw-list extraction and the offline frame
replayer recognize the new transport. Full-call state must be applied before
taking a draw snapshot. The general packet census counts native draws as well;
the `native_draw_call` and `legacy_draw_packet` diagnostic operations distinguish
the two paths. A passing backend strict report alone does not prove that all
game API calls have been converted.

## Qualification

The CPU checks execute reference bodies extracted from the current generated
sources, with their real save/restore helpers. Reports contain source and
reference hashes and reject changes made during a run:

```sh
python3 tools/native-renderer/check_gpu_calls.py --output-dir /tmp/draw-call-report
python3 tools/native-renderer/check_gpu_constants.py --output-dir /tmp/constant-report
python3 tools/native-renderer/check_gpu_vertices.py --output-dir /tmp/vertices-report
python3 tools/native-renderer/check_gpu_vertices.py --indexed --output-dir /tmp/indexed-report
python3 tools/native-renderer/check_gpu_state.py --output-dir /tmp/state-report
python3 tools/native-renderer/check_gpu_clear.py --output-dir /tmp/clear-report
python3 tools/native-renderer/check_gpu_special.py --output-dir /tmp/special-report
python3 tools/native-renderer/check_gpu_special.py --samplers --output-dir /tmp/sampler-report
python3 tools/native-renderer/check_gpu_begin_state.py --output-dir /tmp/begin-state-report
python3 tools/native-renderer/check_gpu_utility.py --output-dir /tmp/utility-report
python3 tools/native-renderer/check_gpu_conditional.py --output-dir /tmp/conditional-report
python3 tools/native-renderer/check_gpu_initialization.py --output-dir /tmp/initialization-report
python3 tools/native-renderer/check_gpu_initialization_frame.py --output-dir /tmp/initialization-frame-report
python3 tools/native-renderer/check_gpu_retirement.py --output-dir /tmp/retirement-report
python3 tools/native-renderer/check_gpu_resolve_draws.py --output-dir /tmp/resolve-draw-report
python3 tools/native-renderer/check_gpu_indirect.py --output-dir /tmp/indirect-report
python3 tools/native-renderer/check_gpu_shader_setup.py --output-dir /tmp/shader-setup-report
python3 tools/native-renderer/check_gpu_shader_linkage.py --output-dir /tmp/shader-linkage-report
python3 tools/native-renderer/check_gpu_vertex_declaration.py --output-dir /tmp/vertex-declaration-report
python3 tools/native-renderer/check_gpu_shader_resources.py --output-dir /tmp/shader-resource-report
python3 tools/native-renderer/check_gpu_recorded.py --output-dir /tmp/recorded-editor-report
```

The draw oracle passes 73,728 cases, 14,400 patched replays and ten alias or
view-boundary fallbacks with unchanged memory. The constant
oracle passes 74,816 cases, 4,173 codec ranges, 18,801 patched replays and 2,338
copied-buffer replays. It also checks 4,695 unchanged fallbacks and 489,408
preserved padding words. These checks prove the bounded writer contracts, not
full-game output or performance. Both oracles pass as Linux executables and as
Windows executables through Proton. Their optional first argument writes a JSON
report directly when a Windows console is unavailable.

The Begin/fill/End oracle passes 7,936 cases on Linux and Windows through
Proton, including 1,984 failures from the real allocator, 2,976 copied/patched
operand replays and 14 unchanged fallbacks. Seven additional descriptor cases
check high physical and XEX addresses without dereferencing their data. The
normal cases fill the actual returned memory and check publication afterward.
Accepted native calls never execute the original BeginVertices body.

The indexed/range CPU oracle passes on Linux and Windows through Proton:
20,480 indexed Begin/fill/End cases,
3,840 prepared auto ranges, 10,240 indexed ranges, four initialization cases
and 24 bare triangle cases. It includes 5,120 failures from the actual allocator,
15,360 copied/patched indexed records and 38 unchanged fallbacks. It compares
all guest memory outside the private call frame after normalizing only the
native transport headers. The original builders never execute on an accepted
native path.

The expanded mixed-call fixture passes all five configurations below: 600 cases
and 1,258,291,200 exact output bytes. Each configuration reports 564 native call
operations: 189 vertices calls (109 indexed), 188 range calls (105 indexed),
40 bare calls and 147 full calls. These include four rejected vertices/range
probes; a fifth probe rejects a whole bare call. Rejections preserve conditional
draw/DMA state and the write epoch while applying only the required
unconditional state. The runs include 80 cache resets each, GPU-owned indices,
affine addresses and a 16 MiB buffer budget, with zero legacy draw packets,
compatibility dependencies or compatibility allocations. Windows APIs run
through Proton 10 on the Deck. Nine trace cases cover indexing/disassembly,
including indexed vertices, both range formats and predicated bare draws.

`native_geometry_check` accepts `--native-calls` and `--native-constants` with a
matching rebuilt runtime. It exercises the production command processor and
real GPU output rather than substituting an accepting backend. Vulkan vertex
and compute exports, DX11, and DX12 bindless and bindful modes pass 120 cases
each with GPU-owned indices, affine addresses, a 16 MiB buffer budget and 80
cache resets. Each run compares 251,658,240 exact output bytes, executes 560
native draws and 240 constant updates, and records no legacy draw packets or
compatibility dependencies/allocations. The full-call checks clear the vertex
descriptor first and require the call to restore it for the GPU output to match.
A negative-control runtime fails the epoch assertion. Trace checks cover four
native-only traces without a swap and preserve raw constant bits, including
NaN payloads.

`check_gpu_call_backends.py` runs the current mixed vertices/range/full-call
matrix with explicitly supplied private executables and matching runtimes. It
checks exact case/output totals, per-format coverage, the real backend reports,
source/binary hashes and the disk reserve. Use a new output directory for each
run. Its `--help` describes Linux and Windows/Proton paths. The previous pilot
and VTX1-only results below remain useful as earlier, separate qualifications.

With `--native-vertices`, the same five configurations pass another 600-case
matrix and 1,258,291,200 exact output bytes. Each configuration submits 240
vertices calls, 320 full indexed calls and one deliberately rejected vertices
call. The rejection probe checks that state is applied, the draw control is
untouched and the guest epoch is preserved. The runs include 80 cache resets
each and report no legacy draw packets, compatibility dependencies or
allocations. Five native-only trace cases now include the vertices record.
These trace checks cover indexing and disassembly; the offline prototype
replayer is not qualification of complete bin/query predication or game output.

The common consumer and game hooks compile for Linux, Windows and Android
ARM64. GPU results above use the Steam Deck driver, with Windows APIs through
Proton 10. They do not qualify Pixel or native Windows drivers. The installed
game remains unchanged.
Rebuild all consumers together: the diagnostics layout changed, so an old
executable or fixture must not be combined with a new runtime by copying a DLL.

The state/fetch oracle passes 12,128 cases on Linux and Windows through Proton:
11,484 state writes and 644 fetch writes, covering 76,292 native ranges. It also
passes 150,938 header ranges, 8,930 copied/patched replays and 28 unchanged
fallbacks. It executes the original save/restore helpers and verifies that the
generated page-protection helper remains an empty operation. Raw payloads,
padding, return registers and guest memory outside the private frame are checked.

With `check_gpu_call_backends.py --native-state`, all five backend configurations
pass 600 cases and another 1,258,291,200 exact output bytes. Each run processes
3,087 native state calls and 553,997 register values. Valid state is deliberately
poisoned before submission so omitted updates cannot pass. Partial runs exercise
high header offsets, page ends and the original fetch-flush value. The five runs
report zero legacy draw packets or compatibility dependencies/allocations, and
the state operations preserve the guest write epoch. Sixteen trace cases cover
all seven pages and retain the raw constant/state bits. These are API/fixture
results; they do not establish whole-game coverage or an in-game FPS gain.

The clear utility oracle passes 1,281 cases on Linux and Windows through Proton,
including 320 real allocator failures, 164,031 generated vertices, 6,727 native
records and 16 unchanged fallbacks. It checks non-finite/subnormal values,
rounding/flush modes, both half-pixel sources, sample controls and the
65,535-vertex boundary. Reserved fixture address space supplies the inputs at
the original virtual addresses without loading game assets. The actual
generated allocation/save/restore helpers execute.

The 28-byte vertex layout also passes the five-backend geometry matrix with
`--native-state --vertex-stride 28`: 600 cases and 1,258,291,200 exact output
bytes, including GPU-owned indices, affine input addresses and cache resets.
This qualifies that input layout and the shared transports. It does not
qualify the clear utility's complete in-game pixel output or all clear/resolve
branches. The producer remains experimental and disabled by default.

The ordinary and vertex-ID range oracle passes 41,328 cases on Linux and as a
Windows executable through Proton, with 10,332 cases per family. It checks
92,016 native state ranges, 38,304 float ranges, 93,600 padding words, 41,328
copied/patched draws and 72 unchanged fallbacks. Eight fallbacks specifically
check that a valid complete flush remains unpublished when only the final draw
would cross a capacity or growth boundary. Every supported dirty bit and
combined/alternating masks are compared with the actual generated draw and
flush helpers. Accepted native paths execute none of those original bodies.
The checks cover alignment, both index formats, all endian modes, expanded
indexed counts, query selection, return registers and memory outside the
original private frames. Synthetic lookup tables occupy the original virtual
addresses without loading game assets. The hooks compile for Android ARM64;
these CPU results do not qualify Pixel GPU output or full-game visual behavior.

The shared consumer also passes the five-backend matrix with
`--native-state --explicit-major`: 600 cases and 1,258,291,200 exact output bytes.
Each configuration requests 400 explicit-major draws, plus 160 implicit GPU
index-producer draws. The Vulkan observer independently sees all 400 explicit
draws with unchanged mode bits; Windows APIs execute through Proton. Each run
retains the same state/constant coverage, cache resets and bin-rejection probes,
and reports zero legacy draw packets or compatibility dependencies/allocations.
This validates the shared transport's explicit mode, not all vertex-ID shaders,
tessellation settings or native Windows/Pixel drivers.

With `--samplers`, the oracle passes 42,192 draw cases and 1,868 standalone
sampler cases on Linux and Windows through Proton. It compares 96,876 state
ranges, 38,304 float ranges, 95,798 padding words and 3,308 coherency calls,
including copied/patched operands. All contiguous sampler ranges up to the
register-page boundary and alternating masks are checked. The 72 unchanged
draw fallbacks include eight final-draw failures after a valid full-flush plan;
12 additional standalone sampler fallbacks include physical A/C/E aliases.
Eight malformed coherency payloads are rejected without changing decode output.

The five-backend matrix with `--native-state --native-coherency --explicit-major`
passes 600 cases and 1,258,291,200 exact output bytes. Each configuration executes
121 coherency calls, including one while the bin mask rejects draws. Every call
checks the completed register wait, destination/size/base and unchanged write
epoch. Draw/state/constant coverage, GPU-owned indices, cache resets and zero
compatibility dependencies/allocations remain the same as the previous matrix.
An older-runtime negative control fails at the capability check. Seventeen
native-only trace cases retain four coherency register actions and reject
malformed headers/contracts. Linux/Windows SDK builds and Android ARM64 source
compilation pass; complete sampler/game output on native Windows and Pixel
hardware remains unqualified. These tests do not measure an in-game FPS gain.

The dirty Begin/fill/End oracle passes 9,380 cases on Linux and Windows through
Proton, including 4,690 indexed cases, 2,346 real allocation failures and 14,070
calls to the actual allocator. It compares 12,136 state ranges, 8,516 float
ranges, 322 coherency calls and 7,034 copied/patched deferred draws. All supported
dirty bits, combined/alternating masks, buffer alignments, allocation modes and
the 65,535-vertex boundary are covered. The caller fills the real returned
buffers, then End publishes the draw. Failures preserve any original completed
state prefix while leaving the draw unsubmitted.

Forty additional rejection cases verify unchanged memory/context and zero
allocations. They include final-draw capacity/growth failures after a valid
flush, allocations/fences/output pointers overlapping only that prefix, deeper
allocator-stack overlaps, unsupported fixups/capabilities, heap reset and
unready fences. Two allocation-failure cases distinguish a multi-register
state run at `2102` from the scalar starting-vertex draw prefix. Accepted native
paths execute no original Begin or flush bodies. The hooks compile for Linux,
Windows and Android ARM64. This producer qualification reuses the unchanged
shared transports/SDKs from the GPU matrix above; it is not a new game run or
new Pixel/native Windows GPU result. The feature remains off by default.

The copy-utility oracle passes 1,728 cases on Linux and Windows through Proton,
using the actual generated writer, memcpy and save/restore bodies. It compares
24,480 state ranges, 1,440 coherency waits and 1,728 copied/patched buffers.
Seventeen capacity, growth, alignment, stack and aliased-source rejections
leave memory and context unchanged. Tests cover width/height overflow,
mode truncation, physical-address conversion and the original return value.

The five-backend matrix with `--native-state --native-coherency --explicit-major
--native-shaders` passes 600 cases and 1,258,291,200 exact output bytes. Each
configuration binds 440 vertex shaders through the new consumer and executes
one native invalidation, including bin-rejected probes. State/draw/coherency
coverage and zero compatibility dependencies remain unchanged. An older
runtime fails at the new capability gate. Twenty native-only trace cases
include both shader stages and invalidation; extracted VS/PS hashes match the
original inline code bytes. These results qualify the shared consumers, not
the utility's complete game pixel output or native Windows/Pixel drivers.

The conditional-state oracle passes 11,264 cases per platform, comparing
114,525 mask updates, 89,986 selected writes and 15,287 ordinary state writes.
Fourteen unsupported cases leave memory and context unchanged. It executes
the original helper and save/restore bodies and covers all eight state modes,
mask restoration, stream counts through the device boundary, signed counts,
scratch-word behavior and copied operands.

With conditional fixups enabled, the complete CPU draw checks pass 42,192
ordinary/vertex-ID cases and 9,380 Begin/fill/End cases on Linux and Windows
through Proton. The Begin checks retain all 14,070 real allocator calls and
2,346 allocation failures. Eight additional final-draw rejections and four
deferred-Begin rejections retain a valid conditional prefix without publishing
it or entering the allocator. Previously unsupported shader/declaration and
other resource-fixup branches remain excluded.

Adding `--native-predication` to the five-backend matrix passes the same 600
cases and exact output totals. Each configuration applies 1,332 selected state
writes, skips six and updates eight mask halves. Three required draw-state
values are poisoned and restored through the conditional consumer in all 440
snapshots; separate probes check high/low selection and preservation. Trace
extraction now follows partial masks and predicates, preserving unconditional
vertices/range state while omitting rejected draw/DMA updates. Its disassembly
output resets stale predicates/actions between packets. Twenty existing trace
cases and an additional mixed conditional sequence pass; rejected draws vanish
from extraction while an unpredicated warmup draw retains the expected state.
These are fixture results with known starting state, not full-game trace or
native Windows/Pixel qualification.

The initialization-helper oracle passes 1,176 cases on Linux and Windows through
Proton: 128 setup buffers and 1,048 GPR buffers. It compares 1,664 state ranges,
256 shader bindings, 3,144 port calls and 10,480 individual writes. It executes
256 calls to the original memcpy and real save/restore helpers, checks 3,528
copied/patched records and ten unchanged fallbacks. It does not execute or
replace the parent initialization routine's allocator.

The parent initialization oracle compares 136 complete call states and memory
images at the original allocator boundary. It separately compares 128 native
continuations, four null results and four compatibility continuations against
the real generated continuation, with eight unchanged pre-allocation rejections.
The native cases preserve 42,368 records, all descriptor flag bytes, physical
addresses, alignment gaps and backing-view behavior. Both Linux and Windows
through Proton pass; the game hooks also compile for Android ARM64. These tests
provide identical heap results to both continuations; they do not execute or
qualify the allocator. The runtime still calls the real game heap exactly once.
An unusual output span keeps the original builders after allocation rather
than retrying the parent and leaking a second block.

The retirement writer passes 896 cases on Linux and Windows through Proton,
including physical views, alignment, sequence rollover and event endian bits.
It compares the real construction segment's complete context and memory after
normalizing only native headers, covering 4,480 native records. Fourteen unsafe
spans fall back unchanged; seventeen malformed synchronization records are
rejected. The scheduling and lock code is outside this CPU fixture. Its source
is checked unchanged except for the single construction hook; context-register
and local-register variants compile on Linux, Windows and Android ARM64.

The five-mode GPU matrix with `--native-sync` passes 600 cases and the same
1,258,291,200 exact output bytes. Each mode checks one shorter coherency wait
and eight completion events under a rejected draw-bin mask. Events preserve
neighboring memory, all four endian modes, literal/counter selection and one
guest write-epoch advance. Coherency leaves the destination register and epoch
unchanged. Twenty-five native-only traces, shader hashes, conditional trace
state and rejection of an older consumer also pass. These results qualify the
shared GPU consumers, without claiming a live ring-contention or campaign test.

The clear/copy checkpoint oracle passes 160 cases on Linux and Windows through
Proton: 64 clears and 96 complete original copy loops. Eighty cases use the
release build's register-locals rewrite. The loops cover 524,488 split records,
including the full `UINT32_MAX` count, physical views, predicate flags and offset
wrap. Twenty unsafe inputs fall back unchanged. Four suspended growth-boundary
calls show that both versions reach the original growth helper before the new
checkpoint. Complete context matches for context-register builds; local builds
compare all context except ABI-volatile CR0/CR6 and still have to finish the real
loop correctly. Geometry, allocation, resource setup and growth implementation
are outside this fixture. Their original source is retained. All game hooks
and both full routines, with and without register locals, compile for Linux,
Windows and Android ARM64. No GPU consumer changed in this producer-only step;
the preceding five-mode GPU proof applies to those same source/binary hashes.

The indirect helper oracle passes 1,056 cases on Linux and Windows through
Proton, including 528 register-locals cases. The complete original helper,
descriptor parsing and real save/restore bodies execute in both versions; all
context and memory match after normalizing the native header. It covers 528
native records, 384 unchanged legacy records for unsupported contracts, twelve
unchanged checkpoint rejections and four suspended growth-boundary calls. The
CPU fixture does not read or qualify the source GPU data. All game hooks,
context/local helper variants and the common consumers compile for Android
ARM64 as well as the desktop targets.

The five-mode GPU matrix with `--native-indirect` passes 600 cases and
1,258,291,200 exact output bytes. Each mode performs 248 indirect loads and
7,946 words, routing all 240 fixture constant updates through indirect loads
for the GPU output checks. Eight additional probes cover raw NaN/signed-zero
bits, partial words, the VS/PS boundary, maximum range, repeated writes to one
source address, neighbors, rejected draw bins and an unchanged write epoch.
Twenty-six native-only traces plus the conditional trace and old-consumer gate
pass, including captured indirect source memory and four exact disassembly
actions across the stage boundary. Windows GPU checks use Proton on the Deck;
Pixel and native Windows drivers remain separate qualifications.

The inline shader setup oracle passes 400 cases on Linux and Windows through
Proton: 256 resource setups, 128 copy vertex setups and sixteen copy pixel
setups. It runs 800 calls to the real memcpy, checks the entire context, code,
metadata, stages, operand positions and padding, and rejects 27 unsafe spans
without changes. Lengths include the game's 525/933-word variants and the
16,382-word transport limit. Allocation and shader patchers are outside this
prefix/copy fixture; the complete resource path is checked separately below. Twelve game-hook checks and
twelve full caller checks, including register-locals variants, compile on
Linux, Windows and Android ARM64. The shared program consumer and GPU fixture
did not change in this step; their preceding five-mode qualification is reused
after verifying the same source and private-runtime hashes.

The five-backend matrix with `--native-ports` passes 600 cases and the same exact
output totals. Three port calls preserve all ten writes, neighboring registers
and the write epoch while the bin mask rejects draws. All 441 shader bindings
check the active stage, raw microcode and unchanged opposite-stage binding;
one binds identical code under the pixel stage after the GPU output checks.
This checks pixel binding/cache identity, without claiming new pixel-rendering
coverage in the memexport fixture. Twenty-three trace cases preserve repeated
port actions, alongside the conditional trace and shader-hash checks.

That opposite-stage test exposed a Vulkan shader-cache bug; Direct3D 12 had the
same hash-only lookup. Both now use the raw data hash and shader stage as the
cache key. Cached pipeline lookups, failure cleanup and Vulkan precompile
requests also retain the stage. The raw shader hashes and storage formats stay
unchanged. Four additional CPU checks cover equal code in different stages,
precompile requests with equal modification values, deliberate bucket
collisions and mandatory stage selection. Direct3D 11 already distinguishes
stage and source. Changed cache-header dependencies were rebuilt together, and
the Vulkan dependency closure also compiles for Android ARM64.

The shared pointer-based shader cache also validates the current source hash
before reusing a program. Address, length, stage and guest epoch alone are not
proof that CPU-written code is unchanged. A production Vulkan fixture reproduced
the old behavior: two valid shaders with equal length at one address left the
first shader active after the source changed. The same fixture binary then
passes with the corrected runtime. `--shader-memory-cache` checks the sequence
A/B/B/A, the active code/stage, unchanged opposite-stage binding and an unchanged
epoch. All five GPU configurations pass those checks plus the 600-case,
1,258,291,200-byte matrix above. Identical content can still reuse the recent
program after validation. This uses the existing backend shader hashes and
does not change shader encodings, transport formats or command-processor layout.

The primary pointer-based shader binding in `8245FA00` now has a native
reference writer after the original resource/declaration fixups and ring
growth. It retains the resource version/fence write and the caller's buffer
publication. The shared `6B` consumer reads code on the command thread, uses
the content-validated cache above, and supports whole-binding bin predicates.
The CPU oracle passes 432 cases on Linux and Windows through Proton, comparing
the full context and memory, including both stages and the version write.
Thirteen unsafe spans are rejected without changes. Twelve hook checks, six
full-caller checks and three Android ARM64 consumer checks compile.

With `--native-reference`, all five GPU configurations pass the same 600 cases
and 1,258,291,200 exact output bytes. Each configuration applies 445 referenced
shader bindings, including the four changed-source cache probes, and rejects
one predicated binding without reading its invalid source. This checks actual
rendering with referenced programs as well as binding identity. It does not
qualify the other conditional shader branches in the game caller.

The trace inspector now applies memory snapshots in recorded order and waits
for each indirect shader/constant load's own snapshots. Its old whole-frame
memory pre-pass could attribute later code to an earlier binding at the same
address; indirect constants also lacked a memory base. A changing-source trace
reproduces the old shader-order failure. Both legacy and native traces now
preserve the A/B/B shader sequence, raw ALU constants and a rejected pixel
binding. Draw records include an ALU-constant hash. The preceding 26 trace
scenarios, conditional trace and old-runtime capability rejection also pass.

The other pointer-shader branches in `8245FA00` now have bounded native
writers too: the pixel binding after declaration fixups, and the conditional
shader/render-state batch after ring growth. The batch retains each operand
position, partial bin-mask update, both resource version writes, mask
restoration and the caller's publication. It stages only the changed CPU
registers and command words; it does not copy the entire guest context or
capture shader code on the game thread. Unsafe aliases retain the original
code without partial writes.

Selected native state now supports validated ranges on render-register pages.
Its additional capability prevents sending those forms to older consumers.
The complete game-code comparison passes 6,460 cases on Linux and Windows
through Proton: 60 pixel references, 6,400 conditional batches and 59,196
native records, with all context, memory and version checks intact. Twenty-four
unsafe inputs are rejected unchanged. The earlier conditional builder also
passes its 11,264-case Linux regression. Twelve hook checks, six full-caller
checks and three Android ARM64 consumer checks compile.

All five GPU configurations pass 600 cases and the same exact output total.
Each also checks ten selected/rejected ranges, 524 source words, neighboring
registers, an unchanged epoch and four applied predicated shader references.
The trace fixture checks a two-register selected program/context update;
extracted draw state preserves both raw values. The ordinary trace scenarios,
changing-source regression and old-consumer rejection pass as well. This does
not qualify declaration patching, arbitrary resource layouts or campaign use.

Typed indirect constants now use the native `6C` consumer too. The additional
form retains raw address/type/index/length operands, including the original
ignored-bit masks and zero-length semantics. It supports ALU, texture-fetch,
boolean, loop and render-register ranges with validated source and destination
bounds. The same backend bulk writer retains binding and constant-buffer
invalidation. Reads still occur on the command thread. A separate capability
keeps the new form away from older consumers.

The descriptor builder now also converts valid masked/wrapped fields after its
original growth call. Its real descriptor loop and save/restore code pass
7,104 comparisons on Linux and Windows through Proton, including 3,552
register-locals cases. Those checks contain 2,232 typed records and 432
canonical records; deliberately out-of-range inputs retain 3,960 unchanged
compatibility records. Twelve unsafe spans are rejected unchanged and all four
growth-boundary calls stop at the original allocator. No GPU source data is
read by the CPU producer fixture. Twelve game-hook checks and three Android
ARM64 consumer checks compile.

With `--native-typed-indirect`, all five GPU configurations pass the same 600
cases and 1,258,291,200 exact output bytes. Each applies 3,348 typed loads and
563,900 words across all five classes. This includes 3,080 live context/fetch/
boolean/loop restorations before draws, all 248 ALU updates and twenty raw-bit,
zero-length and boundary probes. Neighboring registers, current source bytes,
callbacks and the guest epoch are checked. Two legacy/native traces also match
all twenty draw states after changing the source at one address, with a zero
bin mask and ignored operand bits. Trace output now includes boolean and loop
hashes alongside ALU state. The earlier trace cases and old-consumer gate pass.

Six Begin/range families now also lower their final draw records after the
original setup, fixups and allocation. Twelve specific header stores can use
native `6A` direct arguments when the packet is unpublished in the current
ring or in an active recording with proved fixed storage. The guard checks
alignment, capacity, backing-view boundaries, device overlap and special device
modes. Recorded storage must contain the complete draw, keep the 184-byte
recording object separate from commands/device, and have the active state set
by the original BeginRecording. The owner is its original storage/size pair at
offsets 152/156; physical aliases use their actual backing ranges. The original
reservation can leave the published cursor one word before that storage,
including at a view boundary. Callback storage, other recording states and
capture modes retain their original encoding. The API entry refreshes support
on its own thread from the restart-only setting and the runtime capability.
This does not replace the recording allocator or prove arbitrary custom heaps.

Five additional header stores cover the final clear draw, standalone clear
triangles, utility triangles and the copy/resolve split loop. They use the same
proved reservation and ownership guard after the original state and geometry
work, including when a larger native construction plan declines that branch.
The existing full native construction still takes precedence. These checkpoints
change only the final header value, preserving the surrounding instructions,
guest registers, predicate and payload words.

Both old argument layouts retain their length, predicate, query operand and
patch offsets. Auto-indexed records may retain unused index operands without
changing DMA state. Indexed calls retain raw base/size values and the existing
16/32-bit address alignment. The consumer calls the shared native backend
submission method. Query behavior is unchanged; this does not add query
execution support.

The actual seventeen store statements pass 29,376 context/memory comparisons on
Linux and Windows through Proton, including 4,624 converted headers and 24,752
unchanged disabled/unsafe cases. There are 4,080 converted recorded draws,
including 816 at backing-view boundaries. Recording checks cover copied owner
metadata, physical aliases, object overlap, inactive states, missing storage,
short owners, alignment, view crossings and address overflow. Source checks verify
that the eleven original callers differ only at those store values. All eleven callers compile in context
and register-locals forms on Linux, Windows and Android ARM64, including the
complete generated source unit. The earlier twelve game hook and three Android
consumer checks also compile. These are bounded store
and source checks, not execution of every parent allocation branch.

With `--native-direct`, all five GPU configurations pass 600 cases and the same
1,258,291,200 exact output bytes. Each executes 560 direct draws, including 320
indexed draws, 280 query-layout records and 120 auto-indexed records with
unused operands. Another predicated call is rejected before decoding its
unsupported arguments. Control/DMA state, backend arguments, outputs and epoch
checks pass. Two legacy/direct traces match all 48 accepted draw states and
their skipped predicates. That comparison exposed an older inspector bug:
legacy draw decoding omitted control/DMA register actions, and its no-query
form assumed auto indices despite runtime support for DMA indices. The
inspector now models both layouts and exports raw DMA fields. The earlier
trace scenarios, source-order regressions and old-consumer rejection pass.

The original command-buffer iterator now exposes a logical editing view of
one-to-one records and eight aggregate draw/coherency layouts. Address
invalidation, resource relocation and patch builders can find their draw,
shader, constant, fetch and state operands at the original offsets. The
underlying buffer keeps its native encoding throughout copying and editing.
This changes no GPU consumer, submission order or resource lifetime.

The view validates each transport header before exposing it. Float constants
retain the iterator's sixteen-word groups; fetch descriptors retain six-word
groups and the constant flush retains its three-word group. Partial fetch/flush
groups remain opaque. Aggregate layouts validate their complete payload,
version and reserved fields before exposing the original inner operations.
Their continuation uses a relative word offset in the iterator's unused high
predicate bits; bit 0 retains the game's predicate. The state survives iterator
copies and buffer relocation without a host map. The range editor also carries
the origin through its reset to an inner draw. Invalid lengths, overlapping
control storage, unowned flags and unproved layouts retain the original view.
Existing ring and special-mode guards remain active.

The recorded-editor oracle executes the real linked-buffer copy, iterator,
address invalidator, physical-view relocator, resource claims, draw patcher and
ten patch-builder families. Its 75 layouts pass 54,609 complete context/memory
comparisons in each Linux and Windows-through-Proton build, with both context
and register-locals forms. Each run includes eight address invalidations, four
linked-buffer copies, 304 patch checks, 1,534 continuation checks, 35 copied and
relocated iterators, and 111 malformed or unproved aggregate rejections. All ten
additional patch builders must exercise matching records. The six Linux/Windows/Android ARM64 source
configurations also compile. These are CPU editing-contract checks, without a
game launch or a new claim about full-game GPU output or performance.

The Begin/fill/End oracle now also executes the real fixed-recording allocator.
Each Linux and Windows-through-Proton build, in context and register-locals
forms, passes 12,516 cases. These include 3,136 recorded calls, 1,120 recording
allocation failures and 224 failures after the first indexed allocation has
succeeded. Forty-six unsafe recording inputs and forty existing rejection cases
leave memory and context unchanged, without flushing or allocating. Ownership
also protects the device tail read by conditional state fixups. Return ABI,
private-frame bounds, full memory outside those frames, original patch offsets,
caller-filled data and deferred publication are checked. The real game wrappers
and oracle compile on Linux, Windows and Android ARM64. The backend consumer is
unchanged; its earlier exact GPU output checks remain separate evidence. This
does not qualify callback allocators, capture modes or complete campaign use.

The shader counter-event writers retain their original counter selector,
physical-address conversion, endian bits, poison value and 16-byte command
span. The slot helper preserves 32-bit wrapping arithmetic and does not emit
an event when the original helper would return without one. Ownership checks
cover the complete device, private stack frame, command span and destination.
Unsupported growth, recording/capture modes and overlapping storage fall back
before modifying state. The existing native completion-event consumer performs
the write in command order; these producers introduce no new GPU-query semantics.

The counter-event oracle compares the complete generated routines against the
native producers in Linux and Windows-through-Proton context and register-locals
builds. Each run passes 10,399 accepted cases: 1,759 emitted events and 8,640
no-event cases, plus 147 unchanged rejections. Full memory outside private frames,
return/stack/link ABI and publication match. The six Linux/Windows/Android ARM64
oracle configurations and all three game wrappers compile. Run it with
`python3 tools/native-renderer/check_gpu_fences.py --output-dir /tmp/counter-report`.
This is a producer-contract qualification, without new game, native Windows or
Pixel execution evidence.

The shader-export linkage helpers (`8245F3D8`, `8245F4A8`) and the bounded
descriptor merge (`8245F560`) now patch instructions directly in C++. They retain
neutral instruction words, component linkage, merge order, repeated destinations,
shared terminated lists and the original export-count bits. The complete plan is
validated before writing. Metadata, stack, selected descriptors and output state
must have separate backing from patched instructions; A/C physical aliases are
included. Unterminated or oversized lists, wrapping arithmetic and a merge that
needs an unowned sentinel keep the original path without partial writes.

The bounded vertex-declaration patcher (`8245EDC8`) constructs up to 32 fetch
instructions, matches declaration semantics, converts formats and swizzles, and
preserves the original pairwise ordering and fetch coalescing. It reads the real
swizzle/format-size tables and preserves the original private-stack seed bit.
Repeated instruction destinations publish in order. Unsafe metadata, declaration,
stream-map, table and stack aliases are rejected before publication. The caller's
resource binding and command-buffer publication remain separate contracts.

These CPU oracles execute the complete generated routines, real twelve-byte copy
and save/restore helpers. They check code bytes, neighboring memory, return values,
stack/link ABI and unchanged rejection state. Each Linux and Windows-through-Proton
context/register-locals configuration passes 6,713 linkage cases and 3,090
declaration cases, with 45 and 23 unchanged rejections respectively. Successful
code/resource cases cover the writeback, both XEX and A/C/E physical views, including
Windows' extra E-view host page. All twelve oracle source configurations and the
three Linux/Windows/Android game wrappers compile. A private Linux executable
links with the current runtime; this step has no new live game or Pixel evidence.

On this host, repeated CPU-only calls with seven or 31 descriptors are 1.22–1.42
times as fast as the register-locals originals. Single-item cases are about
3–10% slower because of the ownership checks. These are helper timings, without
a measured gameplay FPS gain. GPU consumers and shader formats are unchanged;
this does not establish new campaign coverage.

The bounded resource-preparation helper (`8245F8C0`) now performs the version
comparison, masked stream-state comparison and unsigned in-flight fence test in
C++. Cache hits preserve their two 64-bit snapshot publications. Busy resources
return the original zero result without modifying code, versions or snapshots.
Rebuilds use the same declaration planner above, with the original child-frame
seed and additional ownership checks for the full device, caller frame, fence
word and version counter. Every fallible check finishes before the first write.

Declaration planning and publication are separate so a resource alias cannot
cause a partially applied rebuild. The shared counter retains the real guest
interrupt nesting counter and runtime global critical-region mutex. Big-endian
atomic updates preserve sequential ordering, skip zero and `FFFFFFFF` at wrap,
and restore the original interrupt-state bits. There is no separate lock or
host-only replacement version sequence. Code publication precedes declaration
and variant version/snapshot updates, followed by device snapshot publication.

Its oracle executes the complete generated resource and declaration routines,
real copy and save/restore helpers, and the actual SDK mutex implementation.
Each Linux and Windows-through-Proton context/register-locals mode passes 2,375
reference comparisons and 31 unchanged ownership rejections. Cases cover both
snapshot halves and masking, fence arithmetic around wrap, zero/nonzero versions,
nested interrupt regions, unused invalid pointers on cached/busy branches, and
successful writeback/XEX/A/C/E code and object views at 4/8-byte alignments. Each
mode additionally validates 12,288 allocations across original-only, native-only
and mixed four-thread counter contention, including wraparound. These checks do
not prove campaign coverage or a gameplay FPS improvement.

On this host's repeated CPU-only calls, resource preparation adds roughly 1–3 ns
to cached/busy cases and 25–35 ns to single-instruction rebuilds. Rebuilds with
seven or 31 fetch instructions run about 1.10–1.62 times as fast as the
register-locals originals. These measurements were taken during private builds
and describe these helpers only. The current private Linux runtime/executable
build and loader relocation checks pass. The declaration regression retains all
3,090 comparisons in each Linux/Windows context/register-locals mode. The actual
resource wrapper compiles for Linux, Windows and Android ARM64; this step adds
no live game, native Windows-driver or Pixel qualification.

### Resource binding and initialization

The bounded pixel/vertex shader setters (`82445278`/`82445578`), texture setter
(`824408E0`) and vertex-declaration setter (`82445798`) now update the native
device shadow in C++. Pixel resources use the 40-byte prefix and device slot
`12684`; vertex resources use the 872-byte prefix and slot `12688`. This corrects
the reversed shader names in the historical method table. The setters preserve
the original dirty bits, literal and masked constant-patch ordering, texture
address conversion, sampler fields and LOD clamps.

Retirement stamps the original resource fence or appends its exact two-word
reference to the currently owned 2008-byte retirement block. The capture bit
comes from the original private frame. Special retirement modes, full blocks,
unowned ranges and unsafe aliases retain the original routine before effects.
Shader tables are bounded to 64 KiB and 128 patch runs; repeated shadow targets
apply in order. The declaration setter preserves its leaf-function ABI.

The pixel-resource initializer (`82445240`), vertex-shader template initializer
(`82445480`) and vertex-declaration initializer (`824457F0`) also have bounded
native implementations. They preserve the original object headers, exact clear
sizes, twelve-byte instruction/attribute copies, variant versions and stream
mask. The vertex initializer handles both code variants with up to 32 fetch
instructions each. Declaration initialization proves the terminated input and
up to 256 attributes across sixteen streams before clearing the destination.

The oracle executes all seven complete generated routines with the real copy,
clear and register-save helpers. Each Linux and Windows-through-Proton
context/register-locals mode passes 6,793 comparisons and 86 unchanged ownership
rejections, including 5,396 real copy calls, 424 clears, 2,183 queue appends and
2,048 fence stamps. Cases include all 32 texture slots, null/self bindings,
overlapping constant patches, table limits and writeback/XEX/A/C/E aliases.
Linux, Windows and Android ARM64 source configurations compile. These shared
producers feed the existing Vulkan, Direct3D 11 and Direct3D 12 consumers; no
backend format changes or new device/campaign qualification are implied.

Repeated CPU-only calls on this host put texture binding at about 1.27–1.37
times the speed of the register-locals reference. Vertex/declaration resource
initializers are about 2.28–3.74 times as fast in the tested sizes. Empty shader
binds add roughly 4 ns for ownership checks; patched shader binds are close to
the original cost. These are direct-helper timings, without capability-gate or
GPU time and without a measured gameplay FPS gain. The separate private Linux
executable links and passes loader relocations. Its fourteen retained caller
modules route through the new public hooks, with every other function body
preserved and the ThinLTO summaries rebuilt.

### Complete ready-buffer inline shader setup

The ready-buffer path through `8245F6B0` now performs the whole inline shader
setup in C++. One preflight proves the original reservation, physical code
source, command span, export linkage, vertex-declaration patches and every
publication target. Ring growth and insufficient capacity retain the original
routine before effects. The resulting invalidation and program records use the
existing native consumer format on Vulkan, Direct3D 11 and Direct3D 12.

Export planning and publication are separate so the caller can prove both
patch families before copying code. Every instruction patch must fit in the
reserved shader payload. A linkage result cannot alias later declaration
inputs, device state, the copy source or command storage. Patches retain their
original order, including shared/repeated targets where declaration patches
replace earlier export patches. The original dirty byte, both snapshots and
last-word cursor publish in order. The original two PPC barriers remain around
the code copy; empty host cache-flush loops are removed. Declaration planning
uses the original private-frame seed 736 bytes below the caller's entry stack.

The oracle executes the complete generated caller, reservation, copy,
cache-barrier, linkage and declaration routines with their save/restore helpers.
It validates the two native transport headers and compares every other byte
outside the private frame. Each Linux and Windows-through-Proton
context/register-locals mode passes 3,801 comparisons and 40 unchanged
rejections. Cases include both shader variants, 4–65,528 code bytes, 32 fetches,
31 exports, 256 declaration attributes, overlapping patches, unused invalid
arguments, all six object/command views and seven source-address conversions.
The separate linkage regression retains all 6,713 cases and 45 rejections in
each mode.

The actual wrapper compiles for Linux, Windows and Android ARM64. Compiled ARM64
control-flow checks retain two sequential hardware fences around the copy on
every successful path and none on rejected paths. The private Linux executable
links with the unchanged runtime and passes loader relocations; calls from
`8245FA00` reach the strong native hook. This adds no live game, native Windows
driver, Pixel or campaign qualification.

On this host, repeated CPU-only calls run about 1.29–5.15 times as fast as the
register-locals original across the tested payload/patch sizes. The linked
1 KiB/seven-fetch case improves from about 674 to 521 ns; the 48 KiB case with
32 fetches and 31 exports improves from about 7.85 to 3.29 microseconds. These
measurements exclude the capability gate and GPU work and establish no gameplay
FPS gain.

### Shader constant and depth coordination

The complete bounded `8245F810` loader plans up to 128 descriptors from at most
64 KiB of metadata. Zero-count markers stop before reading a source offset.
Every source must match its physical backing view, and every record must fit
the original capacity and ready-cursor comparisons. The whole command span is
checked against all sources and metadata, including a first source that aliases
only a later output record. Failures leave the original CPU/memory state intact.
An empty loop preserves the full incoming device register; a nonempty loop
returns the final zero-extended cursor. No constant values are copied or
converted on the game thread.

The `8245E330` preparation helper now derives both depth/surface shadow updates
and pending-state flag changes in C++. It preserves both original stores to
device offset `10560`, all unrelated bits and the full returned dirty mask.
When a pending flag clears, the original `EVENT_WRITE(15)` has a one-word
initiator and no memory payload: the existing consumer masks it to six bits and
writes `VGT_EVENT_INITIATOR`. Its native scalar targets that same register
(`21F9`) with value 15. The other branch retains the existing native
`WAIT_UNTIL` port callback (`5C8`, value `20000`). These conversions preserve
current consumer behavior; they do not implement an occlusion result, a new
completion fence or previously missing hardware query semantics.

All four Linux/Windows-through-Proton context/register-locals modes pass 8,842
complete-routine comparisons and 42 unchanged rejections each. Per mode, the
original routines execute 12,420 constant records, 586 event-flag writes,
550 wait-port writes and 384 pending-flag sets. Cases cover depth modes and
hazard fields, shader flags, both constant banks and their boundary, full-bank
loads, terminators, 128 records, unused invalid operands and physical aliases.
Seventy-two object/command view cases include Windows' extra E-view host page.
The actual Linux, Windows and Android ARM64 wrappers compile. The private Linux
build links, passes loader relocations and routes `8245FA00` calls through both
new strong hooks. This adds no live game, native Windows-driver or Pixel result.

Repeated CPU-only measurements put the whole constant loader at about 1.29–2.64
times the speed of the previous checked per-record native path. Seven records
improve from about 193 to 86 ns and 128 from 3.20 to 1.21 microseconds. Both
measurements exclude capability gates; the old checkpoint uses its actual
validator and its output is compared byte-for-byte. Nonempty checked loads
remain more expensive than the unchecked generated loop. Depth preparation
adds roughly 3–6 ns for ownership proof. These are helper timings without a
measured gameplay FPS improvement. The combined ready-buffer coordinator is
covered below; buffer growth and recording retain separate contracts.

### Combined ready-buffer shader coordinator

`8245FA00` now coordinates the complete bounded shader/state sequence in C++.
Its plan includes control preparation, null-vertex early return, pixel/vertex
variant selection, deferred constants, pixel binding, depth/program shadow
changes, conditional-mode caching, cached/rebuilt/busy vertex resources, inline
fallback, loop masks and final references or conditional state. These producers
use the existing Vulkan, Direct3D 11 and Direct3D 12 consumer formats. Android
uses the same producer through Vulkan.

The child helpers separate planning from application. All fallible checks run
before code patches, shared version allocation or command publication. The
complete output span is checked against metadata, deferred constants, device
state and the full nested private frame. Resource patches cannot change a later
planned input. Referenced raw patch targets must name the same backing owner as
the physical shader consumed by the GPU. Export linkage uses the original
private result slot and initial vertex mask without writing it during preflight.
The conditional writer exposes a memory-only application so the outer routine
preserves its nonvolatile registers.

Ring growth, debug-trap branches, unsafe aliases and unproved possible outcomes
retain the original routine before effects. Unused code, declaration, surface
and command operands are not validated as active outputs. `native_gpu_calls` remains
disabled by default.

Each Linux and Windows-through-Proton context/register-locals mode passes
11,063 comparisons and 27 unchanged rejections against the whole generated
routine and its real child call chain. Per mode, that chain executes 12,546
constant loaders, 4,850 resource preparations, 2,528 inline setups, 4,189
declaration patchers and 3,073 control preparations; 919 compared cases allocate
a shared version. The checks validate 68,921 native records, then compare their operands,
code, dirty flags, snapshots, versions, neighboring bytes and preserved CPU ABI.
Late rejections include insufficient final capacity after valid earlier plans,
debug traps, malformed metadata and unsafe alternate outcomes. No growth
or trap is replaced with a successful test stub.

An unversioned second variant now stays native after the first variant allocates
the declaration's ID. The real allocator excludes zero and `UINT32_MAX`, so a
second variant holding either value is a proved cache miss. Its plan reuses the
actual ID published by the preceding rebuild and performs no second allocation.
No counter value is predicted. Ninety-six comparisons cover both excluded IDs,
counter wraparound, four fetch counts and 48 busy-fence cases.

Retained nonzero IDs now use the same real allocation. A possible cache hit is
planned alongside the miss, and application chooses from the declaration's ID
after the first rebuild publishes it. A hit skips the second code patch and
version update. When a miss would be busy, both final conditional batches are
proved before allocation; the selected batch preserves its own cursor, shader
flags, loop masks, reference predicates and version stamps. Capacity and alias
checks cover the larger possible output. No allocation is rolled back and no
counter result is guessed.

The retained-ID checks add 196 comparisons, including 60 actual cache hits and
28 hits that bypass the pending miss's busy fence. They cover wraparound, high
IDs, matching/mismatching stream masks, literal uploads, depth changes and the
alternate loop-mask sentinel. Three more cases leave invalid alternate metadata
unused when a hit is impossible. Two additional original-only runs prove that
the original selected branch succeeds while the unproved alternative requires
a native fallback. Rejections preserve all memory, context and the shared
counter before effects.

Planning constructs large child plans only when needed and avoids copying the
full vector/FP register context. Cached/busy resources skip rebuild-write checks.
Declaration patchers capture validated destination backings once for later
checks; disjoint ranges skip the exact instruction scan. Inline and standalone
patching avoid retaining these backings when no later proof needs them, while
the general overlap checker still handles such plans. Unused instruction slots
are not cleared during plan construction.

Paired CPU-only measurements use pinned prior headers, the same harness on one
CPU, alternating run order, real helpers and byte-compared output. The earlier
range-check optimization remains, but retained-ID planning adds overhead on
short paths: cached setup is about 279 ns versus 261 ns and the two-variant case
619 ns versus 581 ns. The 32-fetch rebuild remains about 1.32 microseconds.
The combined ownership proof still costs more than the per-helper native route.
These medians include identical state resets and exclude capability gates and
GPU work. More optimization is needed before enabling the combined path by
default; these results establish no FPS improvement.

The actual wrapper compiles for Linux, Windows and Android ARM64. The separate
Linux executable links and passes loader relocations with the unchanged runtime.
Its eight calls from the retained draw module route through the strong public
hook; all 1,118 function bodies in that module are preserved except the explicit
callee references, and its ThinLTO summary is rebuilt. This is routine-level
qualification, with no new live-game, native Windows-driver, Pixel, campaign or
gameplay FPS result.

### Fixed-recording allocation

`82458E60` now has a native fixed-storage implementation. It hands out the
recording's retained command storage once, or allocates temporary vertex/index
data down from its end. The native path preserves dword rounding, alignment,
the 164-byte command guard and the corresponding capacity reduction. An
already-initialized command allocation fails without dereferencing unused
recording metadata or the size parameter, as in the original routine.

The fixed-owner branch of `82456D60` also runs natively. It preserves the
original stack writes, 32-bit byte-budget wraparound, budget throttling, heap
tail reset and sticky allocation-failure flag. The requested bytes are accounted
even when storage runs out; subsequent calls with the failure flag set skip
allocation and accounting. Preflight proves the recording object, storage,
size parameter and private frame are separate in normalized backing memory.
An unproved call returns to the original before any effect. Custom allocators
retain their original implementation; ready default-heap allocation is described below.

The full original routines are extracted into private test includes. Each of
four Linux and Windows/Proton context/register-locals runs passes 3,110
comparisons and 47 unchanged rejections. Coverage includes 624 failed
allocations, 512 budget throttles, 768 budget wraps, one-time initialization,
a 68-call recording lifecycle and four physical/XEX backing views. Alias
rejections include A/C, A/E and the two XEX views. Unsupported allocator
branches throw in the reference harness; they never fabricate storage.

Actual wrappers compile for Linux, Windows and Android ARM64. The private
Linux executable links with distinct public/native hooks and original
implementations, and passes loader relocations with the unchanged runtime.
The linked-symbol audit also found and fixed missing C linkage on the existing
constant writer and both EndVertices hooks. Their earlier wrappers compiled
under C++ names, leaving the original public functions selected. The new
`check_gpu_hook_linkage.py` checked the then-current 29 handwritten GPU hooks in the final
artifact: each must be a strong, unmangled definition with an address distinct
from its original implementation. The earlier executable fails this check;
the corrected executable passes. The constant-writer and Begin/End reference
checks are rerun for the newly connected hooks.

Only the two affected wrappers and the allocator's generated definition module
are rebuilt; the other qualified objects are retained. No game, native Windows-driver or
Pixel run, FPS improvement or full-game completion is claimed. The
experimental `native_gpu_calls` option remains disabled by default.

### Default-heap allocation and ring wrap

`82456B58` now has a native path for temporary vertices/indices and 32-byte
aligned command chunks within the device's retained default heap. It preserves
dword rounding, requested/preferred chunk sizes, reservation generation limits,
temporary-data wrap, command-chunk wrap and the original size output. A failed
temporary allocation leaves both cursors unchanged. A failed command wrap can
still advance the generation and publish the wrap flag; those partial effects
are retained.

Ownership and the exact non-waiting predicate of `824564D0` are proved before
effects. The native implementation preserves both original private frames and
their saved registers. Calls needing a GPU wait return to the original allocator
before changing memory or context. No fence completion is invented. Unproved
heaps, wrapping guest addresses, custom allocators and physical aliases between
heap, device, size parameter, stack or completed-fence storage also retain the
original path.

The default branch of `82456D60` uses the same native plan. Its byte budget wraps
at 32 bits, failure still accounts the requested bytes, and throttling uses the
post-allocation wrap cursor before deciding whether to reset the temporary tail.
Fixed recordings and the sticky-failure early exit continue through their
previously qualified native implementation.

Four Linux and Windows/Proton context/register-locals runs each pass 20,443
complete-original comparisons. Per mode, these cover 3,408 command wraps,
954 temporary wraps, 1,920 command-wrap failures retaining partial effects,
64 fixed-recording dispatches and twelve physical/XEX view cases. Another
14,205 inputs reach the real wait-constructor branch in the reference and
leave the native memory/context unchanged. Fifty-seven malformed cases also
reject unchanged. Reference wait branches throw; they never simulate a
successful wait or an allocation. Accepted cases execute the complete original
allocator and ready-fence child chain for comparison.

The actual wrapper compiles for Linux, Windows and Android ARM64. The private
Linux executable links, passes loader relocation checks and has strong,
unmangled definitions for all 30 handwritten GPU hooks. Only the allocator
wrapper and its generated definition module are rebuilt against the pinned
previous application and unchanged runtime. No new backend format, live game,
FPS, native Windows-driver or Pixel result is claimed.

CPU-only medians use the same byte-checked harness, one CPU, five alternating
rounds and identical state resets. The normal dispatcher is about 40.5 ->
36.1 ns, and wrapped dispatch 41.0 -> 38.2 ns. Standalone temporary allocation
is about 25.7 -> 29.1 ns; its additional ownership proof has a cost. A failed
standalone allocation is also slower (14.3 -> 22.9 ns). These numbers exclude
capability gates and GPU work. They establish no gameplay FPS improvement and
do not change the experimental default.

### Command-chunk growth

`824578C0` now has a native path combining the proved default/fixed allocators
with command-chunk publication. It preserves request-size wraparound, recorded
preferred sizes, allocation failure effects, cursor/capacity updates, temporary
tail and byte-budget reset, and the existing 64 KiB ready-limit clamp. Failed
allocations use the retained 4,800-byte scratch buffer allocated by `82466EC0`.
An already-failed call skips allocator and marker metadata as in the original.

Pending markers use the original `824574B8` early-return predicate, including
unsigned counter wrap and the reserved-current-marker case. Proven ready calls
preserve the helper's private save area and clear the pending marker. Waiting
markers, recursive growth, custom allocation callbacks, small chunks whose
clamp cannot be proved within owned storage, and unproved aliases retain the
original routine before any native effect.

The existing writable helper was already a no-op because the runtime recovers
protected guest pages. Its policy now lives in `native_gpu_command_storage.h`
and is shared with the original wrapper. The native path retains its argument
span and call order; it does not force page permissions or remove GPU write
tracking. A protected-page check confirms that the shared helper leaves host
protection intact. All 670 generated function bodies in module 17 are unchanged.

Each Linux and Windows/Proton context/register-locals run passes 28,136 complete
original/native comparisons: 8,863 successful chunks, 19,273 scratch fallbacks,
10,987 ready-marker calls and 7,947 clamped limits. Eight cases use physical/XEX
heap views; A/C, A/E and XEX aliases are rejected. Another 8,769 calls reject
unchanged, including 7,188 actual wait branches and 948 recursive-growth branches
in the reference. The reference never fabricates successful waits or allocation
callbacks. Guard arguments and the device state visible before publication are
also compared. The default-allocator regression passes its 20,443 comparisons.

The actual wrapper compiles for Linux, Windows and Android ARM64. The private
Linux build retains the previous runtime and unrelated objects; its generated
caller is retargeted to the public growth hook with all other function bodies
preserved. Final-symbol auditing checks all 31 handwritten GPU hooks. No game,
FPS, native Windows-driver, Pixel or whole-campaign result is claimed.

### Completion records and bounded chunk closure

`82457210` now writes its ten-word completion sequence using the existing native
program-invalidation and shader-event formats. The mask, event initiators,
physical destinations, endian bits, cursor/generation value and sequence value
keep their original offsets. The CPU-mirror branch and 32-bit sequence wrap are
preserved. The common native and legacy consumers already discard the same
invalidation mask and call the same shader-event implementation. This adds no
new hardware wait or completion guarantee.

`82457B40` now closes fixed recordings, reserved-list segments and failed scratch
buffers when ownership is proved. Reserved descriptors retain their original
header/address and publication order. An empty segment leaves unused queue
metadata unread. Descriptor blocks may share the command heap, but must remain
separate from the current command reservation, saved frames, recording metadata,
completed-fence storage and any newly planned chunk.

Required growth is planned before descriptor publication. Closure then applies
the qualified growth plan with its original caller state and stack writes.
An unproved allocation or wait returns to the original close routine before any
native effect. Normal ring submission, custom recording heaps and descriptor
block expansion retain their original implementation. Normal closure can still
use the native completion writer through its public entry point.

Each of four Linux and Windows/Proton context/register-locals runs passes 1,152
completion comparisons and 2,514 full closure comparisons. These include 349
descriptor publications, 977 growth continuations, physical-view equivalence,
counter wrap, opaque callback presence and preserved high halves of pointer
arguments. All 3,456 native headers per mode keep the original logical editing
view. Another 568 calls reject unchanged, including 432 actual wait branches.
Reference paths requiring ring submission or descriptor allocation throw rather
than fabricate those effects. The growth regression passes 28,136 comparisons
after its argument-planner refactor.

The wrapper compiles for Linux, Windows and Android ARM64. The private Linux
executable links against the unchanged runtime with all 33 handwritten GPU hooks
strong and unmangled. Existing caller objects are retained; only the allocator
wrapper and generated definition module are rebuilt. This is CPU/ABI and command
representation qualification, with no new game, native Windows-driver, Pixel
or gameplay FPS result.

### Shared chunk dispatch and word reservations

`82457CC8` now connects the bounded native close and growth paths for recorded
and reserved command buffers. Its synchronization branch retains the original
unsigned completed-marker comparison, sequence wrapping and zero-target early
return. A pending completion falls back before descriptor publication or other
native effects. The global synchronization flag, completed storage, descriptors,
retained owners and nested save areas are checked for aliases before preflight
values are used. No completion is fabricated.

`82457F50` now reserves words in a retained command allocation and can close and
grow first when that complete continuation is proved to satisfy the request.
Both entry points preserve their guest save areas, caller registers, pointer
high halves and existing no-op permission-guard policy. A second allocation
requiring a different request, custom recording storage, normal ring submission
or an unproved span retains the original routine before effects. Reservation
does not advance the command cursor; callers retain that responsibility.

Each Linux and Windows/Proton context/register-locals run compares 1,017 complete
shared-dispatch calls and 4,304 complete reservations with the original routines.
It also checks 2,819 unchanged rejections, including 490 real wait branches.
Accepted paths include 578 growth continuations, 865 reservations after closure,
and 176 synchronized closures per mode. The completion/closure regression still
passes after factoring its explicit-argument planner. The actual wrapper compiles
for Linux, Windows and Android ARM64.

The private Linux build retains the previous runtime. Generated callers in
module 16 are retargeted to the public hooks while preserving every other
function body and rebuilding the ThinLTO summary. Module 65 only mentions the
entry point in a comment; its object has no such reference and is retained.
All 35 handwritten GPU hooks
have strong unmangled definitions distinct from their originals. These checks
qualify the CPU/ABI contract and linkage; they do not measure gameplay FPS or
qualify native Windows drivers, the Pixel or the complete campaign.

### Submission coherency state

`82457050` now builds the dirty-range and whole-heap cache flushes used before
submission. The native sync format keeps each original eleven-word span and
operand offset. Its consumer already writes the same status, size and base
registers and uses the same real register-coherency wait. Neither path advances
the guest-completion epoch for that wait. This introduces no new backend opcode
or completion shortcut.

The range drain uses a host atomic exchange of the original big-endian empty
sentinel, inside the generated producers' shared critical region. Interrupt
nesting and restoration follow the original protocol. A range arriving before
the exchange is consumed; a later publication stays pending. A call that saw an
empty range initially still leaves a newly arriving range pending when it only
flushes the heap wrap. Allocation failures leave the dirty range untouched.

Default and fixed-recording allocations are planned before publishing anything.
Their existing transient allocators now expose reusable planning/application
steps. The current hand-patched allocation floor remains **128 dwords** with
32-byte alignment; it has not been reduced to the emitted packet size. Sticky
failure, empty state, wrapping allocations, byte-budget wrapping and throttling
retain their original effects and private frames. Unused output addresses,
default heaps and fence pointers are not dereferenced on paths that do not need
them. Unknown recording callbacks, actual allocation waits and unproved aliases
retain the original before any native effect.

Each Linux and Windows/Proton context/register-locals run passes 20,347 complete
original-routine comparisons, including 6,913 fixed-owner cases and 12,330 native
flush records. All 36,990 logical header checks per mode preserve the recorded
editors' view. Another 2,706 calls reject unchanged, including 2,700 actual wait
branches. A contention check covers 8,192 original-producer updates per mode;
allocation-time publications and nested critical regions are also compared.
The allocator regression passes 20,443 comparisons after factoring its planning
steps. The reference probe observes the actual captured reservation in both
context and register-local builds, without changing original guest effects.

The actual wrapper compiles for Linux, Windows and Android ARM64. The private
Linux build uses the previous runtime and retains unrelated objects; only the
native wrapper and generated definition module need updating. All 36 handwritten
GPU hooks have strong unmangled definitions distinct from their originals.
These are CPU/ABI, command-contract and linkage checks, with no new gameplay FPS,
native Windows-driver, Pixel or full-campaign result.

### Primary-ring submission

`824565A8` now proves available primary-ring space with the original circular
read/write predicate. Zero-sized reservations retain the original wait branch.
`82456E68` publishes bounded buffer descriptors into the retained physical ring,
including wrapped slots, then uses the original fences and real MMIO doorbell.
The CPU-mirror mode with no callback keeps its original no-publication behavior.
`824572F8` now handles submission when no work is queued, preserving its inline
descriptor, queue-counter publication and full-width return value.

The three-word native buffer record carries a physical source and word count.
The shared receiver enters the existing indirect-buffer traversal when the
record executes, so later source updates remain visible. Its separate capability
bit prevents an older runtime from receiving the new record. Buffer counters
remain separate from constant-source counters. Recorded-buffer editors see the
original logical header and payload offsets. Vulkan, Direct3D 11 and Direct3D 12
use this same shared submission receiver.

Preflight checks retain the default or fixed-recording owner, descriptor snapshot,
device, synchronization allocation and private save areas. Supported rings have
a power-of-two size with a mask no larger than `0xFFFFF`; a call accepts at most
527 descriptors and each buffer contains 1–`0xFFFFF` words. Physical aliases,
caller pointer high halves and the original counter wrapping are preserved.
Actual waits, locked queued-work submission, callbacks, custom storage and
unproved overlaps retain the original routine before native effects.

Four Linux and Windows/Proton context/register-locals runs pass 16,624 complete
original/native comparisons and 32,064 unchanged rejections. The latter include
12,428 real wait branches, 10,368 locked-queue branches and 6,888 callback
branches; none is replaced by a fabricated success. All 40,536 emitted buffer
records retain their original logical headers. The CPU comparison observes the
external MMIO store boundary; it does not execute hardware MMIO.

A separate check executes the actual shared Linux runtime receiver. It compares
16 deferred-source cases, two nested buffers and eight malformed records while
preserving the completion epoch. Backend draw methods are not invoked. Actual
legacy/native traces also pass extraction: nested buffer attribution is retained,
buffer records are not mistaken for constant loads, and omitted legacy trace
lengths are not read from the next trace command. Historical zero-length buffer
snapshots report only the executed span inferred from their own packet addresses.

The fixtures and actual wrapper compile for Linux, Windows and Android ARM64;
the three changed shared-runtime sources also compile for Windows and Android.
The private Linux runtime rebuilds three objects and the application rebuilds
two, retaining unrelated inputs. All 39 handwritten GPU hooks are strong,
unmangled definitions distinct from their originals. These checks qualify the
bounded submission contract and linkage. They add no gameplay FPS, native
Windows-driver, Pixel or complete-campaign result.

### Normal closure and submission orchestration

`82457B40` now also connects completion records and native ring submission for a
retained normal command chunk. A single preflight covers the submitted commands,
completion destinations, ring, device and the entire nested save area before
the first completion word is written. It preserves completion sequence wrap,
the original CPU-mirror behavior, full-width device arguments and the returned
cursor. Existing recorded, reserved and failed-buffer paths remain available.

The normal path can align within the current chunk or grow into a fully proved
next allocation, including a ready heap wrap. Planning growth before submission
is allowed only when an existing allocation tail selects the new chunk, no
pending marker uses the changed sequence and CPU mirroring cannot change the
fence. These restrictions prevent a plan from using state that completion or
submission will overwrite. Other growth cases, real waits, locked queues,
callbacks and unproved aliases retain the original close routine before effects.
The wrapper requires the completion and native-buffer consumer capabilities.

Four Linux and Windows/Proton context/register-locals runs compare 9,296 complete
normal closures with the original call chain, including 644 growth continuations,
384 wrapped allocations and 5,192 CPU-mirror cases. They also check 11,596
unchanged rejections, including 6,448 actual wait branches, 96 locked queues and
24 callbacks. At the MMIO observation boundary, the emitted completion payload,
sequence, completed value, cursor, generation and logical ring state match.
All 31,992 logical header checks preserve the original editing view. Storage
guard arguments and the state visible at each guard call are also compared.
No hardware MMIO is executed by these CPU comparisons.

The actual wrapper and both fixture variants compile for Linux, Windows and
Android ARM64. Private Linux integration retains the qualified shared runtime
and all generated caller objects; only the native wrapper is rebuilt. The
39 strong handwritten hooks remain distinct from the original definitions.
This stage adds no game, FPS, native Windows-driver, Pixel or campaign result.

### Descriptor-list expansion and CPU cache ranges

`82457748` now expands descriptor lists through the retained default or
fixed-recording allocator. It handles the seven embedded device lists and
separate list objects, first and subsequent blocks, ready heap wrap, byte-budget
wrap, throttling and the original scratch result after allocation failure.
The allocator receives 4,224 **words**, so the native path retains its original
16,896-byte reservation even though a descriptor block uses only 4,224 bytes.
Failure clears the list head and publishes the retained scratch block without
initializing its payload or inventing a successful allocation.

Old block headers are updated before allocation; new headers and the previous
cursor link precede list publication. Preflight proves retained block ownership,
physical view conversion, separate nested save areas and allocator inputs. The
original read/write order, 128-byte allocation alignment, encoded physical links,
pointer high halves and return value are retained. An actual allocator wait,
unknown recording owner or unproved alias returns to the original routine before
the first old-block header is written.

`8245DA50` now computes the final state of its bounded cache-range loop directly.
The reference already treats `dcbf` as a no-op; the native version preserves the
same `REX_PPC_SYNC` barrier, eight guest stack words and general-register results.
Its original special-address early return still touches no stack memory.
Reversed or ambiguous signed ranges retain the original implementation. This
does not replace GPU coherency waits or completion tracking.

Four Linux and Windows/Proton context/register-locals runs pass 12,096 complete
descriptor comparisons, including 5,616 successful allocations, 6,480 scratch
results, 5,184 fixed-owner calls, 9,072 linked blocks, 2,304 throttled allocations
and 432 ready wraps. Another 3,476 calls reject unchanged, including 3,456 actual
wait branches. The same runs compare 880 complete cache calls, including 320
early returns; eight reversed-range cases reject unchanged without executing
their enormous original no-op loops. Full guest bytes and caller ABI are checked
throughout. Cache checks also compare every general/vector register and the rest
of the context; context-mode references additionally compare the condition flags
that register-local builds deliberately keep private.

The wrapper and both fixture variants compile for Linux, Windows and Android
ARM64. The private Linux build retains the shared runtime, recompiles the wrapper
and definition module, and retargets the retained module-16 callers while
preserving every other function body and rebuilding its ThinLTO summary. All 41
handwritten GPU hooks are strong unmangled definitions distinct from their
originals. No installed build, gameplay FPS, native Windows-driver, Pixel or
whole-campaign result is added by these checks.

### Locked submission and linked tails

`82456A90` now appends a retained command buffer to a linked list. It preserves
the encoded end pointer, current/last list fields, both cache-range calls and
their original private stack writes. The first barrier and cache operation
precede the volatile old-tail store; the second barrier and tail cache operation
follow it. Preflight checks physical node/tail ownership, list metadata, aliases,
separate save areas and the original full-width return value.

`824572F8` now also handles an initially queued submission in C++. It uses the
actual SDK spinlock implementations, preserving the owning PCR, contention,
IRQL rules and release ordering. The queued count is rechecked after acquisition:
a concurrent drain releases the lock and follows the original direct-publication
path. A still-queued buffer writes its complete node and publishes the linked
tail before incrementing the queued counter and releasing the lock.

Entry rejection happens before frame or lock effects. After acquisition, an
unproved append owner uses the original append leaf under that same lock;
a drained submission requiring a real GPU wait or callback uses the original
publication leaf after release. It never retries the whole submission after
effects, substitutes a host-only mutex or fabricates a completed wait. Thus this
converts queued orchestration and its proved native leaves while retaining
unsupported owners, waits and callback behavior explicitly.

Four Linux and Windows/Proton context/register-locals runs pass 10,228 complete
original/native comparisons: 144 standalone appends and 10,084 queued calls,
including 3,168 calls drained at the lock boundary. Another 864 real-wait and
2,880 callback boundaries compare observable memory and publication order
without inventing a completed return. Sixteen malformed entries reject
unchanged. The comparisons use an explicit atomic-owner/IRQL boundary model;
they compare the full return ABI only for completed calls.

A separate headless Linux runtime check executes the real SDK lock and the
native queue code: four initial IRQL cases, two forced-contention scenarios
(including a drain), and 128 submissions from two threads. All 128 linked nodes
are traversed exactly once, all counter increments remain, and publication is
checked while the actual lock is owned. These cases require no original leaf
fallback. The GPU doorbell is observed; no GPU backend or completion is faked.

The actual wrapper and both fixture variants compile for Linux, Windows and
Android ARM64. Private Linux integration retains the shared runtime and unrelated
objects and recompiles only the wrapper and definition module. The new append
hook brings the strong handwritten hook count to 42. These checks add no
gameplay FPS, native Windows-driver, Pixel or complete-campaign result.

### Allocation-fence and marker waits

`824564D0` and `824574B8` now implement their wait loops in C++. Allocation waits
keep the original two-bit generation and cursor predicate. Marker waits retain
unsigned sequence wrapping, zero-target and reserved-marker early returns, and
the required dispatch when waiting on the current unsubmitted marker. Every
recheck reloads the live writeback pointer and submitted sequence. Notification
alone never satisfies either predicate.

The existing wait constructor, sleeping poll hook, timeout/abort handler,
profiling cleanup/callback and dispatch entry remain in use. Allocation waits
poll once after construction; marker waits recheck before their first poll.
An abort stops the loop through the real poll result and leaves fence values
untouched. Required dispatch, hang handling and callbacks retain their real
entry points. Arguments, private save areas, full-width returns and helper-call
order are preserved; unproved storage rejects before effects.

The shared command receiver now notifies sleeping progress waiters after a
shader-event writeback, for both legacy and native event records. Previously
that write did not notify the existing sleeping poll hook. The same word is
written at the same point, before notification; memory encoding, completion
timing and predicates are unchanged. Vulkan, Direct3D 11 and Direct3D 12 use
this shared receiver. This avoids waiting for the poll timeout solely because
the completion arrived through a shader event.

Four Linux and Windows/Proton context/register-locals runs compare 10,324 complete
calls, including 3,840 waiting calls and 960 abort exits. They also compare 240
required-dispatch, 960 hang-handler and eight profiling-callback boundaries
without fabricating successful returns. Twelve malformed calls reject unchanged.
The original constructor, poll and cleanup bodies run with an explicit scripted
clock/progress model; guest frames, timing statistics, physical views, helper
arguments/order and completed return ABI are compared. The matrix checks 960
writeback-pointer changes and 960 first-poll distinctions.

A separate headless check runs the real shared receiver, actual sleeping poll
hook and original timing helpers with the native wait roots. Five completion
cases cover legacy/native shader events and both wait types. An unrelated event
wakes the sleeper but cannot complete its target wait. Two abort cases leave
both completion words unchanged. Actual receiver writes wake a progress waiter
before its 250 ms timeout; no GPU backend, draw or completion is fabricated.

The fixtures and wrapper compile for Linux, Windows and Android ARM64; the
changed shared receiver also compiles for Windows and Android. Private Linux
integration rebuilds one runtime object and two application objects; the
retained module-16 marker callers already use the public entry point. Constructor
and cleanup still bind to their originals, and the sleeping poll hook is retained.
The native hook count is 44. Native Windows drivers, Pixel runtime behavior,
gameplay FPS and complete campaign coverage remain separate qualification work.

### Buffer range coherency

`824415D0` and `8244E208` now write typed native short-coherency records in C++.
They preserve physical-view offsets, page rounding, 32-bit wrapping lengths,
the eleven-word span, operand offsets, saved frames and return registers.
Unproved command/frame aliases and growth boundaries reject before effects.

The flush block in `82441238` uses the same native consumer after the original
resource wait, submitting-thread check and optional chunk growth. The resource
updates and atomic unlock remain ordered around that block. Its failed
preflight continues through the original block; it never repeats the whole
resource operation after side effects or invents GPU completion.

Linux and Windows/Proton comparisons pass in both context and register-locals
modes: 5,280 complete leaf calls and 2,200 unlock-block continuations per mode,
with 57 unchanged rejections and two observed growth boundaries. The unlock
block compares the complete context and all guest bytes after expanding only
the native record's three structural words. These CPU checks render no frames.
The wrappers and fixtures also compile for Android ARM64.

In a private 1x Linux Vulkan startup/title run of 1,086 frames, synchronization
command dispatch is 99.75% native and state dispatch is 83.10% native. A stable
title census changes from 3,685 retained waits to six per frame and removes
7,358 old register packets while preserving the register-write total. These
are measured command shares, not campaign completion or an FPS claim. Two
standalone leaf conversions did not account for that busy title traffic; the
unlock block does.

The production trace player renders the same captured frame with native
coherency records and their exact original expansion. Both images match
byte-for-byte, with zero legacy resource dependencies in the strict native
renderer. The SDK runtime is unchanged; Vulkan, DX11 and DX12 share this
coherency consumer. Native Windows drivers and the Pixel still require their
own runtime qualification. The strong handwritten hook count is 46.

### Scalar vertex-index offsets

Seven remaining scalar `2102` stores in BeginVertices, DrawRange and the copy
resolve draw now use native state records when their complete span is owned and
reserved. The unconditional state write remains separate from the predicated
draw. Capture mode, queued-ring mode and unproved recorded owners retain the
original header. The draw and state writers share one ownership check, with
64-bit span arithmetic; operand locations, CPU registers and publication stay
unchanged. State and draw conversion have separate capability checks.

The original 17 draw stores and seven new state stores pass 30,888 cases each
on Linux and Windows/Proton. This includes 26,026 unchanged fallbacks, recorded
owners across physical aliases, and 858 headers at backing-view edges. Eight
source checks cover Linux, Windows and Android ARM64. These store checks do not
render frames.

A separate private 1,087-frame 1x Vulkan startup/title run measures 99.54% native
state dispatch and 99.75% native synchronization dispatch. Its stable title
census retains 77 type-0 packets per frame, down from 11,114 before these state
and coherency conversions, with the same 30,769 register writes. Production
replay of the captured native frame and its original coherency/state expansion
matches byte-for-byte. This establishes the tested paths, not whole-game,
Windows-driver, Pixel or gameplay-FPS qualification. At that milestone, native
presentation serialization, rare shader/state branches and campaign coverage
remained explicit work.

### Native frame presentation

The host `VdSwap` import can now emit a typed native presentation request and
native front-buffer fetch binding into its existing 64-word reservation. It
keeps the payload offsets, padding, physical-address translation and mapped
dimension-read order. Conversion requires the renderer capability and the
application's existing `native_gpu_calls` option. The default remains off.
Dimension pointers overlapping the output retain the original format.

The native receiver validates the header, span and version, then calls the
shared host frame handler directly. The legacy parser calls that same handler.
Its swap, profiler, census and counter body is unchanged; argument parsing now
precedes the shared handler. No class fields or virtual methods were added.
Trace capture and disassembly recognize native presentation as a swap, so frame
boundaries and offline replay remain available. Vulkan, DX11 and DX12 share this
producer/receiver; backend swap implementations are unchanged.

Linux and Windows/Proton each pass 5,440 serialization cases and 74 schema
cases. The matrix checks host/endian-aware writes, original-format opt-out,
all 4,096 pairs of aliased output-word dimensions, dimension-read order,
malformed headers, spans and versions. These checks render no frames. Ten
source checks cover the actual shared receiver, producer, disassembler,
capability registration and fixture on Windows and Android ARM64.

A private 1,079-frame 1x Vulkan startup/title run dispatches all 1,079 swap
requests natively, with no reported compatibility resource dependencies. State
dispatch measures 99.55% native and synchronization 99.75%. Host refresh still
reports seven startup attempts without refreshed output, as before; native swap
dispatch does not imply an image was displayed. The production trace player
compares the same captured native frame against the old runtime and exact
original command expansion at 1x, 2x and 3x. Images match byte-for-byte at every
scale. This does not qualify the campaign, native Windows drivers, Pixel
runtime behavior or a gameplay FPS gain.

Reproduce the hardware-free serialization check with
`python3 tools/native-renderer/check_gpu_presentation.py --output-dir /dev/shm/presentation-check`.
The paired GPU trace checks are separate evidence.

## Remaining API conversion

The [historical function inventory](d3d_method_table.md) is not a complete API
boundary. The current telemetry identifies 18 draw-header emitters; the four
bounded writers above are only one family, and their live frequency has not
been measured. State and shader orchestration, resource binding outside the
bounded setters, remaining recorded-buffer modes,
queries, resolves, presentation and synchronization still need individually
verified contracts and conversion. Shared fast render/fetch shadow writers are
now converted; that does not convert every dirty-state orchestration branch,
shader binding sequence or buffer-growth path that calls them.

The normal BeginVertices and indexed Begin branches now handle the supported
dirty-shadow flushes as well as prepared state, with default allocation or the
proved fixed-recording allocator. Shader/declaration/resource
fixups, allocation/wrap outside the proved default/fixed heaps, blocking waits,
wait-helper internals, unproved writeback owners, custom queued-buffer owners and callbacks,
closing outside the bounded normal/recording/reserved paths,
growth outside the proved chunk path,
custom heaps, special bin/screen-extent
records and heap resets between indexed allocations retain their original
orchestration. Their final draw can use direct arguments when the live-ring or
fixed-recording contract above is proved. Topology-split ranges retain their original loop and
can use that same bounded final-header conversion. Ordinary and
vertex-ID ranges handle the shadow and conditional fixups described above. With the
rectangle, copy-utility, initialization, retirement, clear/copy checkpoints and
vertex-ID families, there are now bounded draw-emission paths for all 18
telemetry emitters. This is an inventory count, not a native percentage or a
measured share of rendering traffic. In particular, `82455570` and `8246C750`
retain their other state/resource writers and orchestration; their inline shader
prefixes, pointer bindings and bounded ready-buffer orchestration in `8245FA00`
are converted. Growth, recording and ambiguous shared-version branches in that
coordinator still retain their original orchestration. Unsupported
branches within every bounded family, other constant-load call sites,
shader/declaration orchestration outside the bounded patchers, resource
preparation and ready-buffer setup above, recorded templates, queries,
resolves, presentation and synchronization
outside the converted contracts still need work. The 18-site milestone does
not mean the entire graphics API has been replaced.

Finish those API families and resource ownership contracts, then qualify
campaign states and device limits. This pilot supplies no full-game native
percentage and no measured FPS improvement.
