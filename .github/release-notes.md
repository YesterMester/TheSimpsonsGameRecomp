You need your own copy of The Simpsons Game for Xbox 360. No game content is included; the
launcher installs the game from your own ISO.

### Performance and renderer

- Vertex and index data can use native GPU buffers instead of scanning and uploading the Xbox memory mirror for every draw. Unchanged buffers are retained; changed data gets a fresh immutable copy, and old copies stay alive until the GPU finishes with them.
- Changing vertex data can reuse its latest immutable copy within a frame. Every reuse still checks the source bytes. This reduced vertex upload traffic by about 55% in the tested 2x Springfield scene.
- Verified index bounds are retained with their exact buffer version instead of scanning the same indices again. Native texture uploads also bypass the full memory mirror for supported CPU textures.
- Native buffer reuse checks use Linux's optimized byte comparisons and explicit SIMD batches on Windows, where larger ranges use AVX2 in the regular package. NoAVX2 builds keep their compatible path. Every source byte still has to match before a GPU buffer is reused.
- The title and menus recover their 30 FPS target in the tested 2x scene. Renderer CPU time there fell from 45.4 ms to 17.7 ms after removing the vertex/index residency scans. This is a menu measurement; it does not establish the same gain throughout gameplay.
- Matching render target transfers use GPU image copies. Native resolves can keep their results in their own GPU buffers, let texture reads use them directly, and update the memory mirror when something actually needs it. A partial CPU write preserves the untouched GPU-written pages.
- Matching full color resolves copy into separate texture images. Image ownership stays stable across frames. Partial updates and forced memory reloads preserve channel order, including 10-bit color.
- Copy-free resolves stay off by default. That experimental shortcut caused the development build to alternate between old frames, including in menus. The other native renderer work remains enabled.
- On Linux, the timer queue sleeps between deadlines instead of continuously spinning. This frees CPU time for the game and audio. It has been checked with sound enabled.
- More shader programs can be extracted from the executable and compiled before play. The bake now includes common shader modifications without requiring a recorded pipeline for each one. Missing draw variants and other Vulkan device configurations still use runtime translation.
- The native fullscreen search shader advances its constant-table address once per sample, matching the original program. The previous version advanced it twice and could sample the wrong offsets.
- Renderer comparisons cover menus, level arrival, Springfield and super burp at 1x and 2x. The compared images are identical. Full-game native GPU coverage is still unfinished.
- The latest live Springfield checks average 59.5 FPS at 1x and 54.4–56.8 FPS at 2x on the Deck. These are short scene checks, not a full-game benchmark or proof of a consistent gameplay gain.

### Frame timing

- Game time runs at the right speed. Every read of the game's clock rounded a little time away and took a lock, and the game reads it constantly, so its time ran about 0.5% slow, with frames and vblanks at 59.65 Hz instead of 59.94. The clock is now computed from a fixed base without rounding or locking. Windows and Linux keep the full integer result when scaling that clock.
- A frame that finishes just after its vblank is shown right away instead of a whole frame later. The game held every late frame for the next vblank, because showing it immediately would tear on the console; the PC presents without tearing either way, so this removes a source of stutter.

### Direct3D 11 renderer (experimental, Windows)

- Windows builds now include an experimental Direct3D 11 renderer for GPUs without good Vulkan or Direct3D 12 drivers. Choose *Direct3D 11 (experimental)* in the launcher's graphics backend setting, or set `gpu = "d3d11"`. Automatic still uses Vulkan, with Direct3D 12 as the fallback. It needs a feature level 11.0 GPU; vertex memexport needs 11.1.
- It runs the game's own draws natively: shaders, textures, render targets and MSAA, depth, ownership transfers between render targets, tiled resolves, higher render scales, gamma and the launcher's FXAA, CAS and FSR1 settings.
- Tested on a Steam Deck through Proton: the intro videos, menus, saves and Springfield at 1x and 2x, with movement, camera, pause and all presentation effects. It has not been tested on Windows drivers yet.
- Performance in the tested Springfield scene on the Deck: about 55 FPS at 1x and 22 FPS at 2x, where 2x started at 13 FPS. At 2x it is limited by the GPU, because resolves still go through the Xbox memory layout; Vulkan resolves directly into textures. The title screen holds its 30 FPS.
- Before this release, the work per frame was cut by uploading only the vertex data each draw reads, keeping state between draws instead of resetting it, updating constants in place, copying register blocks in bulk, writing depth and stencil in one pass and using the same depth settings as the other renderers.
- Scaled resolve views are retained with their GPU allocation instead of being created again for each read or write. The cache is bounded; growing an allocation creates fresh views while earlier draws keep their original data.
- Draws that discard rasterization keep their vertex work running and leave pixels untouched. Their stream-output setup now uses an explicit declaration, with vertex execution and unchanged render targets checked together.

### Audio

- Late mixer wakeups no longer trigger a burst of calls to the game's mixer. Requests for audio frames keep a steady schedule, stay at least half a frame apart, and wait with a high-resolution timer, so Windows' millisecond sleep granularity cannot slow the game's audio. With the Windows build on the Deck through Proton, normal play, four competing CPU workers, pause and resume ran without underruns with the 16- and 32-frame queues.
- SDL writes, semaphore wakeups and diagnostic logging no longer hold the audio queue lock. A failed SDL write returns its consumed queue credit so a device error cannot permanently reduce the queue.
- Audio queue settings are clamped before conversion to an unsigned value, so negative values no longer select the largest queue.
- The scalar stereo downmix puts the rear left and right channels on their correct sides, matching the x64 path.
- Decoder commits and mixer setters now publish only the fields they own. A delayed decode can no longer rewind a newer mixer read position or overwrite another audio update. Output positions publish their PCM samples before the mixer reads them.
- Cancelled or already completed decode work is checked again after taking its context lock, preventing an older worker from claiming the same voice later.
- The audio worker, decoder and game's DAC mixer request native audio scheduling. On Linux, the desktop priority service can grant it without running the game as root. Non-audio RenderWare workers retain their usual scheduling.
- Mixer status queries read their own context word instead of copying the whole audio context.
- In the tested 2x Springfield scene with four competing CPU workers, the 16-frame queue went from 119 underruns and 16 silent mixer blocks to zero of both. The 8-frame queue also passes this load check. Normal play and pause/resume also pass. The minimum 4-frame queue can still underrun under this load. The Windows build was checked through Proton, as above, but not yet on Windows itself.

### Windows

- Windows saves vector registers into the game's native setjmp storage with aligned stores. That storage is now always 16-byte aligned; an unaligned buffer crashed the Direct3D 11 test build at startup.

### Launcher

- The graphics backend setting has a *Direct3D 11 (experimental)* choice on Windows.
- New Linux runtime defaults also reach existing installs when the launcher writes their settings. Saved overrides, display settings and audio queue size are preserved.
- Includes the launcher and runtime security fixes from 0.0.6.3.

### Known issues

- Native GPU coverage is incomplete. Unsupported resource, shader and resolve cases still use the existing fallback.
- At 2x, frame drops remain in busy areas. Further CPU and GPU performance work is needed.
- The Direct3D 11 renderer is experimental: it is slower than Vulkan at higher render scales, has not been tested on Windows drivers, and in-engine cutscenes and most of the campaign have not been checked with it.
- Menus and the title screen run at 30 FPS, as the game's menus were made for. Rendering them at higher frame rates is planned.
- Frame rate settings above 60 do not add frames, because the game's frame scheduler tops out at 60 FPS, and they make frame pacing less even. 60 is recommended. Proper 120 FPS and unlimited rendering remain future work.
- At 60 FPS some scripted sequences can misbehave. If random deaths happen at the dam in "Lisa the Tree Hugger", switch to 30 for that section.
- In-game prompts still show controller buttons. Press F1 to see which key each one is on.
- Keyboard and mouse has been tested much less on Windows than on Linux and the Steam Deck. The new native resource defaults are qualified on Linux/Vulkan; Windows validation remains separate.

### Installing

- Windows: extract the zip and run `simpsons-launcher.exe`.
- Linux and Steam Deck: extract the archive and run `Play.sh`.
- CPU without AVX2 (most CPUs from before 2013): use the package ending in `-NoAVX2` instead.
- In the launcher's Install tab, select your Xbox 360 ISO, then press Play.
- Existing installs can update from the launcher's About tab.
