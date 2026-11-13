# Renderer performance and compatibility

## November 2, 2026 native buffer and resolve work

Vulkan's retained vertex/index content checks now select the available x86
vector hash at runtime. The hash values and byte-validation contract are
unchanged. A synchronized, immutable function pointer handles simultaneous
first use, including CPUs without AVX2; other architectures keep their existing
implementation. This improves the buffer-hash throughput on the Deck without
raising the portable build's CPU requirements. The checks cover 84,835 cases
on Linux, with AVX2 disabled, and on Windows through Proton; Android ARM64
compiles separately.

Vulkan can also copy a partial native color resolve into an existing texture
when its channel interpretation already agrees. Only a whole-image replacement
may change that interpretation. Clipped rectangles preserve untouched pixels,
and bounds, separate source/destination images and the original barriers remain
required. The region checks cover 20,165 cases at 1x/2x/3x, including 1,746
accepted partial copies. The captured co-op frame remains pixel-identical.

The Steam Deck 2x Springfield investigation still reproduces the large
one-player to two-player frame-rate drop. Repeated captured-frame timings do
not establish a substantial end-to-end FPS gain from these changes. Command
recording and resolve-mode comparisons also preserve pixels without resolving
that bottleneck. Copy-free resolves and one-shot native buffer write watches
remain disabled. These checks do not qualify the complete campaign or native
Windows/Pixel hardware.

## October 16–17, 2026 development changes

Native resolve publication now reads the shared GPU ownership bitmap once under
one lock on Vulkan, Direct3D 11 and Direct3D 12. A 4 MiB allocation on a host with
4 KiB pages previously acquired that lock 1,024 times. Vulkan also checks retained
owners against the bitmap in one operation, skipping empty words. This keeps a
coherent snapshot of the current owner; it does not cache ownership across CPU
writes or discard the native resource's validity mask.

DX11 immutable uploads use their retained source snapshot directly when the
allocation is already aligned. Padding still uses a separate zero-filled buffer.
The byte comparison, cache keys, original sizes and retained resource lifetimes
are unchanged. This removes one temporary allocation and full input copy on the
aligned path.

DX11 CPU vertex streams now feed that content cache directly as ordered ranges.
Unchanged input skips the temporary zero-filled snapshot and its input copy.
A cache miss copies the ranges once into the immutable version instead of first
assembling a second snapshot. The hash matches the contiguous cache, and reuse
still compares every source byte and all zero padding. LRU behavior and
GPU-owned or mixed-source paths are preserved. This removes CPU
memory traffic at every render scale; it is not a measured gameplay FPS claim.

DX11 and Vulkan CPU snapshot creation initializes only padding before appending
the actual input. DX12 similarly clears the zero prefix, alignment gaps and GPU
replacement ranges, while CPU copies supply their own bytes. Every byte retains
its previous value before the buffer is used. This avoids clearing large vertex
inputs immediately before overwriting them, including Android's Vulkan uploads.

DX11's vertex/index content cache now defaults to 32 MiB, counting both CPU
snapshots and GPU buffer allocations. The previous 8 MiB default repeatedly
evicted the special-attack frame's working set; 16 MiB still did so. The budget
is allocated only as needed. `d3d11_buffer_cache_mb` accepts 1–128 MiB at startup,
including the previous 8 MiB limit for systems needing less cache retention.
Gamma buffers and the shared GPU index snapshot cache keep their separate limits.

DX11 constant uploads append into 64 KiB dynamic pages when the driver supports
constant-buffer offsets and `NO_OVERWRITE` maps. Pages are retained per constant
block and payload size, with a 4 MiB total page budget. Each view is aligned to
256 bytes and its padding is zeroed. Binding state tracks offsets and lengths
as well as buffer identities across vertex, hull, domain, geometry and pixel
stages. Page wraps discard old storage while preserving queued GPU versions.
Unchanged data skips the upload. Unsupported drivers, large inputs and exhausted
page budgets retain exact-size discard uploads. Set
`d3d11_constant_buffer_ranges = false` to compare that path on a supported driver.
Neither path reduces shader precision or effect quality.

DX12's shader-storage writer now completes a pending flush before sleeping.
Previously, consuming the flush request could leave the final cache bytes
buffered until another request or shutdown. A production saved-frame check
reproduced 20,480 bytes visible before shutdown versus 24,944 after it. With the
fix, all 24,944 bytes are visible before shutdown, and the image is identical.
This checks persistence; it is not a shader-compilation timing measurement.

The shared upload-validity fast path now uses lock-free atomic words. Its old
unlocked reads raced locked plain writes, which C++ does not make safe merely
because the writers hold a mutex. All published accesses use atomics, and the
existing writer lock still protects updates. This fixes the race on Vulkan,
DX11 and DX12, including Android's Vulkan path, without adding a lock to the
upload fast path.

Native GPU index snapshots can now serve smaller draws from a valid larger
snapshot without another GPU download or completion wait. Cache hits retain the
most recently used versions, and invalidated entries are reclaimed first. The
8 MiB / 64-entry budget and CPU/GPU write invalidation are preserved. Existing
callers keep immutable bytes even when a later write invalidates the cache.
This common path is used by Vulkan, DX11 and DX12.

Vulkan composed-owner lookup scans only the pages overlapping the requested
range. Large retained textures no longer require scanning every page to answer
a small input request. Partial-page clipping and ownership checks still apply.

DX11 and DX12 cleanup now preserves prepared native outputs while a caller
holds their handle. Previously, cleanup could remove ownership tracking before
that caller published its GPU write. DX11 also keeps these outputs through
cache clearing. Live and queued GPU versions retain their existing protection.

The shared DX11/DX12 native color and depth resolve shaders use constant
division for 1x/2x/3x and shifts for the power-of-two packing groups. Quotients
also provide the subpixel remainder, avoiding a second division. This changes
address calculation without reducing resolution, MSAA, format precision or
effects.

Windows Automatic graphics selection tries each compiled backend in order:
Vulkan, DX12, then DX11. A failed backend is released before the next attempt.
Explicit backend choices stay explicit, and application-provided graphics
systems keep their setup behavior. Headless initialization returns a failure
status when provider or command-processor creation fails.

DX12 initialization now reports failure to its caller instead of terminating
the process, so another backend can be tried. A rejected real-time or high
command queue priority falls back to lower priorities, including normal. Failure
at normal priority still fails initialization. Automatic adapter selection uses
DXGI's high-performance enumeration when available; explicit adapter indices
keep their original enumeration. Vulkan automatic selection prefers usable
hardware over a CPU software device, retains its existing feature preferences,
and prefers a discrete GPU when those preferences are equal.

DX11 remains experimental. These changes do not relax its feature-level 11.0
minimum or make DX12 work on a driver that does not support it.

## Scaled resolve measurements

The saved special-attack frame was replayed 90 times per case on the Steam
Deck. DX11 GPU timestamps, excluding the initial shader and texture setup,
measured these resolve-copy costs:

| Render scale | Previous | Updated | Reduction |
| --- | ---: | ---: | ---: |
| 2x | 22.1 ms | 10.8 ms | 51% |
| 3x | 49.0 ms | 30.3 ms | 38% |

The original-resolution copies also fell from 4.9 to 3.2 ms at 2x and from
5.8 to 4.7 ms at 3x. DX12 uses the same updated shaders; its observed median
replay time at 3x fell from 138.7 to 125.8 ms with bindless resources and from
135.5 to 126.0 ms with bindful resources. These DX12 times include CPU and GPU
work. Vulkan's GPU resolve cost remained around 2 ms at 2x and 4 ms at 3x;
its changes in this pass reduce CPU ownership work and index readback stalls.

These are saved-frame measurements on one GPU. Windows results use Proton.
Gameplay FPS, native Windows drivers and Pixel performance need separate
measurements. Image comparisons use the same replay count on both runtimes:
some recorded effects produce a different image after repeated playback than
after a single playback.

In a separate 120-replay comparison using the same DX11 runtime, changing the
vertex/index budget from 8 to 32 MiB avoided 485 repeated immutable buffer uploads
per steady frame. Buffer creation and rewrite data fell from 12,860.5 to 1,000.0
KiB per frame at both 2x and 3x, with identical output. The stream-preparation
phase fell from 11.8 to 3.9 ms at 2x and from 37.3 to 4.3 ms at 3x in the clean
steady windows. Timing windows with another game process running are excluded.
These phase times include driver work. Long stalls still occurred elsewhere at
3x, so this does not establish stutter-free gameplay or a universal FPS increase.

The constant-upload comparison uses 180 replays per case with the same updated
runtime and 32 MiB vertex/index budget, changing only
`d3d11_constant_buffer_ranges`. The 64 KiB page path measured:

| Render scale | Exact-size mean | Append-page mean | Exact-size median | Append-page median |
| --- | ---: | ---: | ---: | ---: |
| 2x | 52.66 ms | 51.33 ms | 51.32 ms | 50.88 ms |
| 3x | 78.05 ms | 76.23 ms | 61.01 ms | 60.66 ms |

At 2x, steady binding preparation fell from about 2.37 to 2.00 ms. Output is
identical, and no separate game process ran during these measurements. The 3x
maximum was still 259.51 ms with append pages versus 348.74 ms without them.
These are observations from one paired run, not a reliable bound on stalls or
proof of a meaningful 3x speedup. The earlier 16 KiB page experiment also had
large spikes and is not the selected game policy.

## Checks

[graphics_resource_check.cpp](../../tools/bench/graphics_resource_check.cpp)
executes the production selection and page-mask helpers without a game or GPU
device. It checks 76 backend, priority and adapter decisions and 80,004 ownership
cases, including sparse masks, partial words, invalid ranges and guarded output.
It also checks 24,096 atomic validity cases, including concurrent publication
of upload data. Linux ThreadSanitizer reports no race in those checks.
It passed on Linux and in a Windows x86-64-v2 build through Proton without AVX2.
The Windows renderer workflow is configured to build and run it with MSVC.

Its isolated ownership benchmark repeats 1,024-page snapshots 10,000 times. On
the Steam Deck, Linux measured 133.9 ms for individual lock acquisitions versus
4.3 ms for the batched snapshot; Windows through Proton measured 138.4 ms versus
3.7 ms. This is a CPU microbenchmark, not a game FPS improvement or a prediction
for every resource size and driver.

The updated private runtimes pass the DX11 immutable buffer fixture and the
39-transition production ownership fixture, including CPU faults during GPU
publication and 8,773,252 exact bytes. It also checks 173 index-cache cases,
87 reads served without another download, and two retained-output cases across
intervening allocations and cache clearing. The geometry matrix passes 600 cases
with 1,258,291,200 exact bytes across Vulkan vertex and compute exports, DX11, and
DX12 bindless and bindful modes, using a 1 MiB buffer budget and cache clearing.
The buffer fixture adds 223 multipart-input checks, including contiguous-cache
reuse, input mutation, padding, malformed ranges, eviction and queued GPU reads.

The constant-upload change additionally passes 4,098 queued native draw checks
across exact-size and append uploads. They cover multiple page wraps, unchanged
data, padding, out-of-range reads, whole/ranged binding transitions, trailing
unbinds, state invalidation and rejection of malformed windows. The updated
runtime passes the ownership fixture, 120 DX11 geometry cases with cache
clearing, and nine DX11 saved-frame comparisons across the three scenes and
1x/2x/3x scales. Its changed upload and binding sources also compile for
x86-64-v2 without AVX2. These incremental checks do not extend campaign coverage.

Thirty-six saved-frame comparisons also match the previous qualified output byte
for byte: attack effects, a later Medal of Homer state and Colossal Donut at
1x/2x/3x on Vulkan, DX11 and both DX12 binding modes. All their strict native reports
have zero compatibility dependencies and allocations. The changed common,
Vulkan and application setup sources compile for Android ARM64 with NDK r29.

The expanded native resolve fixture passes 4,408 dispatches and compares
3,908,042,752 bytes exactly. It includes full 3x, 2x3 and 3x2 addressing,
color and depth formats, partial regions, array slices, endian conversion and
multisampling. DX11 runs at feature level 11.0; DX12 shares the shader source.

No game was launched for these checks. They cover their recorded inputs; they
do not prove every campaign event or qualify native Windows drivers and Pixel
hardware. The Vulkan changes are shared with Android, but the APK must be
rebuilt before the phone uses them. The development executable, installed
runtime and APK were not replaced by these private builds. Application setup
changes require rebuilding the executable as well as the runtime.
