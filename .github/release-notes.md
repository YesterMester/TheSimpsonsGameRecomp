You need your own copy of The Simpsons Game for Xbox 360. No game content is included; the
launcher installs the game from your own ISO.

### Frame rates above 60

- The launcher's *Frame rate in levels* setting offers 30, 60 (the default), 90, 120, 144, 165, 240 and Unlimited. 30 and 60 keep the game's own vblank timing; the other rates count each frame in fractional vblanks and pace frames with a precise limiter, so game time follows real time at every rate.
- Havok physics keeps 1/59.94 s steps, 60 per second, at any rate. Background loading keeps at least the game's own frame period, so loading screens finish when frames are uncapped.
- Menus, the title screen and the pause menu run their frame-counted logic (input repeat, timers, credits) at the 30 FPS they were made for, and by default are drawn at 30 too (*Frame rate in menus*). UI movies no longer run 1-4% slow.
- Pick your display's refresh rate: a fixed-refresh display never shows the extra frames, and Unlimited drops them unevenly.

### Graphics options

- Character tessellation (experimental, off by default): Low, Medium and High round the characters and other skinned meshes using their own positions and normals, adding triangles where an edge is large on screen or bends sharply. It works with Vulkan on Linux and Windows. Shaders and meshes it does not recognise keep their original geometry. Its helper shaders compile on first use.
- Colour: Original (unchanged), Vivid, Punchy, Soft and Custom, with vibrance, saturation, contrast and brightness. Vibrance raises muted colours more than vivid ones. It shares the FXAA pass when FXAA is on and is skipped entirely at Original. Vulkan only.
- Both settings are in the launcher's Settings tab and need a restart.

### Renderer

- Special attacks and other effects no longer turn the scene black for about a second. Some effect passes resolve the scene and clear it in the same command, and a held-back copy-free resolve read the image after the clear. Seven captured failure frames and a live effect trace now match exactly at 1x and 2x, and recordings of repeated combos show no blackouts.
- Copy-free resolves are on by default. In 0.0.6.4 they were off because live 2x play could alternate between old and new frames; the swap now flushes held-back resolves after opening its submission. Replays are bit-identical and 0.57 ms faster per frame on the GPU at 2x. The launcher turns them on once for existing installs; a player who turns them off afterwards keeps that choice.
- The GPU copy of the Xbox 360's memory is created only when something needs it. Resolves that share memory write into one native buffer, fresh resolves take their initial data from guest memory, and textures only partly covered by native buffers are assembled from them. In new games of all 18 episodes at 2x, 16 never create it. Replayed gameplay frames are pixel-identical, with 0.3 ms (2x) to 0.9 ms (1x) less GPU time.
- The rare frames where a depth buffer owned memory that a colour resolve read (seen in Eighty Bites and The Day the Earth Stood Stupid) now go through a native depth-to-colour transfer, keeping the depth and stencil bits, instead of the EDRAM emulation.
- The shipped translated shader set matches the current translator again; in 0.0.6.4 its hash no longer matched, so about 95% of shaders were translated during play. It also serves more GPUs now: the configuration it checks holds only what the modules depend on, so a set made on the Steam Deck works on most desktop GPUs. In Springfield at 2x all 262 shaders come from it. Release builds stop if the set does not match the translator.
- Retained vertex and index data is checked by hash instead of byte for byte, and found through a flat table: on the Deck at 1x in Springfield the GPU command thread drops from about 9.2 to 8.2 ms per frame.

### CPU performance

- Linux builds use the default code model and ThinLTO, and Windows builds use ThinLTO. On the Deck at 1x in Springfield with an unlimited frame rate, the code model change alone took the average from 90.0 to 91.9 FPS and the 1% lows from 67.3 to 69.5 FPS.
- The game's memset and memcpy, the renderer's shader constant copies and the XDK's write-combined copies use the host's routines, and indirect calls are no longer counted in release builds: the main thread drops from about 10.45 to 10.0 ms per frame at 1x in Springfield.
- At 30 and 60 FPS the game's frame limiter sleeps until shortly before its deadline instead of spinning. On the Deck at 2x and 60 FPS the APU drops from its 15 W limit to about 14.3 W and frames over 18 ms from about 41 to 29 in 1500.

### Direct3D 11 renderer (experimental, Windows)

- Resolves of whole single-sampled colour render targets copy straight into the textures that read them, instead of being decoded again from the Xbox memory layout. Under Proton the first level renders the same with it on and off.
- Under Proton, if the log reports an unsupported `SHDR` shader chunk, install Microsoft's `d3dcompiler_47` with protontricks; Wine's own compiler produces shaders the renderer cannot use yet. Windows includes the right one.

### Windows

- Windows packages now use the native renderer paths and the translated shader set, like Linux. In 2x Springfield runs through Proton on a Steam Deck this averaged 68.9 and 68.7 FPS against 63.1 and 63.6 with the previous Windows settings, with 1% lows of 51.6 and 52.5 against 39.0 and 37.3. With ThinLTO, 1x runs averaged 78.0 FPS against 76.2 and 76.7. Windows drivers themselves are still untested.

### Linux

- A crash or forced quit no longer leaves the game's memory file in /dev/shm, where enough of them made the next start fail with SIGBUS.

### Launcher

- New settings: level and menu frame rates, character tessellation and colour. FXAA's description explains that at 2x and above it costs about 1 ms a frame on the Deck for an image that is already supersampled.

### Known issues

- Native GPU coverage is still incomplete. Unsupported resources and resolves use the existing fallback.
- Character tessellation is experimental. It has been checked at every setting in a few episodes, not across the whole campaign or on Windows drivers.
- Above 30 FPS some scripted sequences may still misbehave; switch to 30 if one does. The whole campaign has not been played at the higher rates.
- The Direct3D 11 renderer is experimental and has not been tested on Windows drivers.
- The minimum 4-frame audio queue can still underrun under heavy CPU load.
