# Native renderer: plan and state

Goal: render the whole game natively on Vulkan (Linux / Steam Deck, Windows, later
Android) with none of the Xenia-derived GPU emulation (EDRAM emulation, render target
cache, texture cache, shared-memory mirror machinery) in the shipped build.

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
| D3DDevice_BeginVertices / EndVertices | 0x8244C450 / 0x8244CEA0 |
| D3DDevice_Clear / D3D_InternalClearDraw | 0x82453C30 / 0x82453B08 |
| D3DDevice_Resolve | 0x82455570 |
| D3DDevice_Swap | 0x824544F0 |
| D3DDevice_BeginTiling / EndTiling / SetPredication | 0x824675C8 / 0x82467B50 / 0x82467450 |
| D3DDevice_CreateShaderA / B (container) | 0x82448178 / 0x82448308 |
| D3DDevice_SetVertexShader / SetPixelShader | 0x82445278 / 0x82445578 |
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
3. **Render targets as surfaces** - done, except copy-free resolves. Host render targets are
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
   it. It is still off by default. Static frame replays alone do not catch this kind of
   regression; check live recordings (frame order) too.
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
  13 of the 18 episodes reached gameplay; Bartman Begins, Enter the Cheatrix, Bargain Bin,
  Rhymes with Complaining and Meet Thy Player were still in their intro videos.
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
