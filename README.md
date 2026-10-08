# The Simpsons Game Recompiled

A native PC port of *The Simpsons Game* (Xbox 360, 2007) for Linux, Steam Deck and Windows, made by
statically recompiling the original game code.

The game's PowerPC executable is translated ahead of time into C++ and compiled for x86-64, so the
game's own code runs directly on your CPU rather than inside an emulator. Underneath it, the
ReXGlue runtime, which is derived from the Xenia project, provides the Xbox 360 kernel, audio, input
and graphics layers, with a Vulkan renderer on Linux and Vulkan, Direct3D 12 or an experimental
Direct3D 11 renderer on Windows.

**This project does not include any game content. You need your own copy of the Xbox 360 game.**

[Download the latest release](https://github.com/YesterMester/TheSimpsonsGameRecomp/releases/latest)

## Contents

- [Status](#status)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Configuration](#configuration)
- [Known issues](#known-issues)
- [Building from source](#building-from-source)
- [Contributing](#contributing)
- [Reporting bugs](#reporting-bugs)
- [Project layout](#project-layout)
- [Roadmap](#roadmap)
- [Legal](#legal)
- [License](#license)
- [Credits](#credits)

## Status

| Platform | Status |
|---|---|
| Linux (x86-64) | Playable |
| Steam Deck | Playable, and the main test platform |
| Windows (x86-64) | Playable, less tested than Linux, mostly on Steam Deck hardware |
| Android | Planned |

The game boots, plays its videos, saves and loads, and runs its levels. See
[Known issues](#known-issues) for what still misbehaves.

## Features

- Native x86-64 executable: the recompiled game code runs without CPU emulation.
- Vulkan renderer that draws with the GPU's own render targets, plus an accurate fallback path
  that emulates the Xbox 360's EDRAM in the pixel shader.
- Native vertex and index buffers, texture uploads and render target copies for supported
  resources. Changed data gets its own copy so later frames cannot overwrite an earlier draw.
- Experimental Direct3D 11 renderer for Windows 10/11, for GPUs without good Vulkan or
  Direct3D 12 support. Choose it in the launcher's settings.
- Audio fixes for late mixer wakeups and concurrent decoder updates, with native audio thread
  scheduling to keep the mixer running under load, on Linux and Windows.
- 60 FPS gameplay, with menus, the title screen and loading screens kept at the original 30 FPS so
  they run at the speed they were made for.
- Render resolution scaling (supersampling), anisotropic filtering and FXAA.
- Free camera and photo mode, with controller and keyboard controls, zoom and HUD hiding.
- Original, soft or disabled ink outlines, custom outline colours and cleaner character eyes.
- Start episode selection for new games, and intro logo skipping across language folders.
- Precise game-clock reads and even Havok steps at 60 FPS, with optional timing and audio
  diagnostics for investigating slowdowns.
- A launcher that installs the game from your ISO, manages settings, patches and save backups,
  adds the game to Steam, and updates itself.
- Controller support, and keyboard and mouse controls with rebindable keys and an in-game list of
  the controls.

## Requirements

- Your own copy of *The Simpsons Game* for Xbox 360, as an ISO image. The USA release (title ID
  45410809) is the main target. The European release also boots and plays but has seen less
  testing.
- About 5 GB of free disk space for the installed game data.
- A 64-bit x86 CPU. The standard download needs AVX2 (Intel Haswell, AMD Excavator or Zen, or
  newer, roughly 2013 onwards). For older CPUs with SSE4.2, download the package ending in
  `-NoAVX2` instead; it runs the same game, somewhat slower.
- A GPU with a current Vulkan driver (Linux) or Direct3D 12 driver (Windows). The experimental
  Direct3D 11 renderer needs feature level 11.0 or newer.
- A controller is recommended.

## Installation

1. Download the archive for your platform from the
   [Releases page](https://github.com/YesterMester/TheSimpsonsGameRecomp/releases). Use the
   `-NoAVX2` package if your CPU predates AVX2 (the standard package then fails to start, on
   Windows with error 0xc0000142).
2. Extract it anywhere.
3. Start the launcher:
   - Linux and Steam Deck: run `Play.sh`.
   - Windows: run `simpsons-launcher.exe`. It is a standalone program and does not need Python.
4. In the **Install** tab, select your Xbox 360 ISO. The launcher extracts and installs the game
   data.
5. Press **Play**.

On Steam Deck, use **Add to Steam** in the launcher to play from Game Mode.

To start the game without the launcher window, for example from your own Steam shortcut, run
`Play.sh --play` on Linux or `simpsons-launcher.exe --play` on Windows. It uses the same settings
as the launcher's Play button.

The launcher can update itself from the **About** tab. Updates replace only the engine and
launcher files. Your settings, saves and mods are kept. If the installed package does not match
your CPU, the launcher offers the right one.

**Antivirus warnings.** The Windows programs in the release are not code-signed, so Microsoft
Defender and browsers such as Edge sometimes flag or block the download, occasionally with a
generic detection like `Trojan:Win32/Suschil!rfn`. These are false positives: every release is
built from this repository's source by the public GitHub Actions workflow in
`.github/workflows/release.yml`. If your download is blocked, you can build the game yourself
(see [Building from source](#building-from-source)) or restore the file from Defender's
protection history.

## Configuration

Most settings are in the launcher's **Settings** tab, saved as soon as you change them. They are
written to `simpsons.toml` next to the game executable, inside a block the launcher manages.
Settings outside that block can be edited by hand. The launcher has a light and a dark theme; the
button at its top right switches between them.

Saves and the shader cache are stored in:

| Linux | Windows |
|---|---|
| `~/.local/share/simpsons` | `%LOCALAPPDATA%\simpsons` |

**Frame rate.** 60 is the default and the recommended setting. 30 matches the original console
exactly. The game's frame scheduler cannot go above 60 FPS, so the 90 and 120 settings do not add
frames; they make frame pacing less even. Menus and loading screens run at 30 FPS regardless,
because the game runs their logic once per frame (`menu_frame_rate` in `simpsons.toml`, 0 turns
this off).

**Image quality.** FXAA anti-aliasing is on by default for new installs and smooths the
cel-shading outlines at little cost. A render scale of 2x or 3x supersamples the whole image, which
gives the outlines their cleanest look. Short Springfield checks on a Steam Deck run close to
60 FPS at 1x and around 55–58 FPS at 2x, with drops in busy areas. These are scene checks;
performance varies through the game.

**Audio.** The launcher's *Audio buffer* setting chooses how many 5.3 ms audio frames are queued:
Small (16), Normal (32, the default) or Large (64). Without a launcher setting the game uses 8.
Values from 4 to 64 can be set with `audio_maxqframes` in `simpsons.toml`. On the Steam Deck,
the Linux build passes the heavy CPU load check at 8 and 16 frames. The Windows build, tested
through Proton, has no queue underruns at 16 and 32 frames, but can still produce occasional
silent mixer blocks under that synthetic load. The minimum of 4 can still underrun under heavy
load. Updating keeps your saved setting.

**Keyboard and mouse.** Turn on *Play with keyboard & mouse* in the launcher's Settings tab. The
game then takes the mouse whenever its window is active, and lets go of it when you switch to
another window. The game's own prompts show controller buttons; press F1 in the game to see which
key each button is on. The default layout:

| Action | Keys |
|---|---|
| Move | W A S D |
| Look around | Mouse |
| Jump, confirm | Space, Enter |
| Attack | Left click |
| Special attack (hold), back | Right click, Backspace |
| Action: talk, use, pick up | E |
| Target (hold) | Shift |
| Walk (hold) | Ctrl |
| Switch character | 1 to 4, arrow keys |
| To-do list | Tab |
| Pause | Esc |

Every control can be changed in the launcher's **Keyboard & mouse** section, where an action can
have several keys, mouse buttons included, and the wheel and side buttons can be bound. The camera
can also be put on keys (Look up, down, left and right), on top of the mouse. The keys that open
the in-game overlays (F1 controls, F4 settings, F3 FPS overlay, ` console) can be changed there
too. In the
game, F4 opens the settings, with the controls under Input > Keybinds (press *Save to config* to
keep changes made there).

**Free camera and photo mode.** The free camera uses the game's developer camera. Characters
receive idle controller input while it is active; photo mode also pauses the level and hides the
HUD and subtitles. These controls work while a level camera is running:

| Action | Controller | Keyboard |
|---|---|---|
| Free camera on/off | L3 + R3 | F6 |
| Photo mode on/off | Y with the free camera active | F8 |
| Move | Left stick | W A S D |
| Look around | Right stick | Arrow keys |
| Down, up | Left trigger, right trigger | Q, E |
| Zoom out, in | LB, RB | 1, 3 |
| Reset zoom | Tap R3 | 2 |
| Faster, slower | Hold A, X | Hold Shift, Ctrl |

F6 and F8 can be rebound in the launcher's **Keyboard & mouse** section. In `simpsons.toml`,
`freecam_speed` sets movement speed (5 by default), `freecam_hide_hud` controls HUD hiding in photo
mode, and `freecam_game_follows` lets the game stream and cull from the free camera. Leaving a
level releases the camera and its input lock.

**Outlines and eyes.** The **Ink outlines** settings offer Hard (original), Soft and Off, a soft
line strength slider, colour presets and a custom `RRGGBB` colour. **Characters' eyes** offers
Clean (the default) and Original. Clean reads the artists' no-rim-shadow flag with a tolerance
to remove speckled shadows on eye whites. The settings are `ink_outlines`, `ink_outline_strength`,
`ink_outline_color` and `eye_shading`; changing them requires a restart. Each image patch checks
the expected game data first and leaves an unsupported image untouched. Clean eyes and coloured
outlines change shader microcode, so those variants are translated at runtime and can cause a
first-use shader compilation pause. Black outlines retain the existing shader variants.

**Start episode.** The launcher's **Patches** tab can start a new game in any of the 18 episodes.
Existing saves keep their progress. The launcher keeps `simpsons_gameflow.lua.original` beside
the modified gameflow script; choosing Land of Chocolate restores it exactly. If another tool
edits the script while this patch is active, the launcher keeps both files and refuses to
replace the external edits. Individual episodes may depend on progress from earlier levels;
starting every episode this way has not been checked through a full play-through.

**Game timing.** `physics_step` selects Steady (the default), Legacy (the previous 60 FPS fix)
or Original (the console code). Steady uses the game clock's frame time instead of its smoothed
copy, avoiding the extra 8.3 ms Havok steps at 60 FPS. `tick_count_precise` reads the millisecond
clock directly so delayed background timer updates cannot distort the game's frame time.
Both settings require a restart. These fixes do not unlock rendering above 60 FPS.

**Diagnostics.** `audio_log_underruns` also reports callback rate, callback time, wait lateness
and queue depth. `audio_dump_file` writes submitted six-channel, 256-sample big-endian float
frames and a companion `.ts` file with one host `uint64` monotonic nanosecond timestamp per frame;
it is off by default and requires a restart. Recording to a slow disk can affect audio timing.
`physics_log` reports physics steps and hazard damage messages. `REX_TIMER_STATS` adds per-timer
arming rates and intervals to the existing timer diagnostics.

## Known issues

- At 60 FPS some scripted sequences can misbehave, because the game was built for 30 FPS.
  Random deaths in "Lisa the Tree Hugger" were reported in
  [#2](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/2). The fork's author reported
  completing the affected section at 60 FPS with the timing fixes, but the whole campaign still
  needs testing; switch to 30 if it happens.
- If videos show a black screen on Windows, switch the graphics backend to Vulkan in the
  launcher's settings.
- The Direct3D 11 renderer is experimental. It has been tested through Proton on a Steam Deck,
  not yet on Windows drivers, and it is slower than Vulkan at higher render scales: in the tested
  Springfield scene on the Deck, about 55 FPS at 1x and 22 FPS at 2x.
- Native rendering is still being completed. Unsupported resources and resolves use the
  existing fallback, and the new native resource paths have been tested most on Linux/Vulkan.
- The minimum 4-frame audio queue can still underrun under heavy CPU load. Windows audio has
  been checked under load with the Windows build through Proton, not yet on Windows itself.

## Building from source

The game and the ReXGlue SDK are built together in one CMake project. Continuous integration uses
exactly these steps; see `.github/workflows/build.yml` for the full list of Linux packages.

### Linux

Requirements: Clang 20, CMake 3.25 or newer, Ninja, pkg-config, and the development packages for
GTK 3, Vulkan, X11 and XCB, Wayland, xkbcommon, udev, ALSA, PulseAudio, PipeWire and D-Bus.

```sh
git clone https://github.com/YesterMester/TheSimpsonsGameRecomp.git
cd TheSimpsonsGameRecomp/simpsons
cmake --preset linux-amd64-relwithdebinfo \
      -DREXSDK_DIR="$PWD/../tools/rexglue-sdk" \
      "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3"
cmake --build --preset linux-amd64-relwithdebinfo --target simpsons
cd ..

# ISO extraction tool used by the launcher's installer
cmake -S tools/extract-xiso -B tools/extract-xiso/build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build tools/extract-xiso/build

# Install your game and play
launcher/simpsons-launcher.sh
```

The Linux preset calls the compiler `clang-20`. If your distribution names it `clang`, add
`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` to the first `cmake` command.

### Windows

Requirements: LLVM/Clang 20, Visual Studio Build Tools with the "Desktop development with C++"
workload (for the Windows SDK and linker), CMake 3.25 or newer, Ninja and Python 3.

```powershell
git clone https://github.com/YesterMester/TheSimpsonsGameRecomp.git
cd TheSimpsonsGameRecomp\simpsons
cmake --preset win-amd64-relwithdebinfo `
      -DREXSDK_DIR="$PWD/../tools/rexglue-sdk" `
      "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3"
cmake --build --preset win-amd64-relwithdebinfo --target simpsons
cd ..

# ISO extraction tool (WIN32 must be defined for the bundled getopt)
cmake -S tools/extract-xiso -B tools/extract-xiso/build -G Ninja `
      -DCMAKE_C_COMPILER=clang "-DCMAKE_C_FLAGS=-DWIN32"
cmake --build tools/extract-xiso/build

# Install your game and play
pip install PySide6
python launcher\launcher.py
```

Windows builds include the Vulkan and Direct3D 12 renderers; the launcher's settings choose
between them. Add `-DREXGLUE_USE_D3D11=ON` when configuring to include the experimental Direct3D 11
renderer, as the release builds do. Without PySide6 the launcher opens in your web browser instead of its own window.

## Contributing

Contributions are welcome, from bug reports to fixes and new features.

**Before you start**

- For anything larger than a small fix, open an issue first to discuss the approach.
- Check the open issues and pull requests so work is not duplicated.

**Pull requests**

- Branch from `main` and open the pull request against `main`.
- Keep each pull request to one change. Describe what it fixes or adds and how you tested it:
  platform, GPU, and what you played.
- The Build check workflow must pass. It builds the game on Linux and Windows for every pull
  request.
- Follow the style of the code around your change. The SDK includes a `.clang-format`.
- To change how the game behaves, prefer overriding a recompiled function from `simpsons/src` over
  editing files in `simpsons/generated`, which the recompiler produces.
  `simpsons/src/frame_pacing.cpp` and `simpsons/src/subtitles.cpp` show how.
- Launcher settings are stored in `simpsons.toml`. If you change a default, add an entry to
  `SETTINGS_DEFAULT_MIGRATIONS` in `launcher/launcher.py` so existing configurations pick it up.
- Never include game files, ISOs, extracted assets, or anything else derived from the game's data.

By submitting a pull request, you agree that your contribution is licensed under the license of
the part of the project it changes (see [License](#license)).

## Reporting bugs

Before opening an issue, check the [known issues](#known-issues) and the existing issues. When you
open one, include:

- Your platform, operating system, CPU, GPU and graphics driver version.
- The release version, shown in the launcher's **About** tab.
- What you were doing and what happened. For graphical problems, add a screenshot.
- The log. The launcher's **Play** tab shows the game's console output, and its diagnostics
  section can create a support bundle with the logs and settings.

## Project layout

```
launcher/            Launcher and installer (Python, with a Qt WebEngine interface)
simpsons/            The game project: recompiled code (generated/), hand-written hooks (src/)
simpsons/re/         Notes on the game's internals
tools/rexglue-sdk/   ReXGlue runtime: kernel, audio, input and graphics layers
tools/XenonRecomp/   PowerPC to C++ static recompiler
tools/extract-xiso/  Xbox ISO extraction, used by the launcher's installer
tools/bench/         Unattended test and benchmark tooling
.github/workflows/   Build check on every push and pull request; manual release packaging
```

Not in the repository: game data, prebuilt toolchains (`tools/clang20`, `tools/rexglue-bin`) and
build output.

## Roadmap

The goal is for this to be the best way to play the game. In rough order:

- **Fully native renderer.** The Xbox 360 GPU emulation is being replaced piece by piece with
  native rendering, each step checked to give exactly the same image. Done: shaders compiled
  ahead of time for the tested Vulkan configurations, native replacements for expensive shaders,
  matching render-to-texture copies done natively, and render targets sized like native ones.
  Supported vertex and index data now uses native buffers, with unchanged copies retained and
  changed data checked before reuse. Supported CPU textures upload directly, and matching color
  resolves keep separate texture images. Next: cover the remaining formats and resource aliases,
  move resource creation to the game's own loading paths, and remove the remaining EDRAM and
  memory-mirror fallbacks. See the [renderer notes](simpsons/re/native_renderer_plan.md) for
  coverage and validation. A native renderer is also what the features below build on.
- **No slowdowns or stutters.** A steady 60 FPS everywhere, including at 2x internal resolution
  on the Steam Deck, with no shader compilation hitches.
- **Higher frame rates.** Proper 120 FPS and unlimited rendering, with game logic, physics,
  scripted sequences and audio kept at the right speed.
- **Direct3D 11.** A native Windows 10/11 renderer for older GPUs, experimental since 0.0.6.4.
  Next: resolve directly into native textures as the Vulkan renderer does (the round trip through
  the Xbox memory layout is what slows it at higher render scales), and testing on Windows
  drivers. See the [DX11 renderer notes](simpsons/re/d3d11_renderer.md).
- **Widescreen.** Wider aspect ratios such as 21:9 without stretching.
- **Controller prompts.** Button icons that match the controller you have connected (Xbox,
  PlayStation, Nintendo, Steam Deck), or your keyboard keys, in every in-game prompt, menu and
  tutorial.
- **Game fixes.** Fixes for bugs in the original game.
- **Restored content.** Unused content from the original game, brought back where it works.
- **Mod support.** A mod loader and tools for replacing and adding scripts, models, objects and
  levels.
- **Android** port.

## Legal

*The Simpsons Game* is © 2007 Electronic Arts Inc. *The Simpsons* and all related characters and
trademarks belong to their respective owners. Xbox and Xbox 360 are trademarks of Microsoft
Corporation.

This is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or
sponsored by Electronic Arts, Fox, Microsoft or any other rights holder.

This repository and its releases contain no game assets: no textures, models, audio, video,
scripts or level data. The code in `simpsons/generated` is produced by recompiling the game's
executable code and does not contain the game's data. The game runs only with data installed from
an ISO of your own copy, and nothing from your installation is uploaded anywhere.

Please support the original release. Do not use this project with copies of the game you do not
own, and do not ask for or share game files in this repository's issues or discussions. You are
responsible for complying with the laws that apply to you regarding backups of media you own.

This software is provided as is, without warranty of any kind.

**An updated note on AI assistance:** most of the early work on this project used little to no
AI assistance. As things have become harder, I've used ChatGPT and Claude a handful of times to
help with research, bug fixes and improvements, and to get closer to what I want this recomp to
be. I'm afraid I may have to use them more as I get further into the project, and I apologize
for that. This is still a solo, fan-made effort and a fast-moving hobby project.

## License

The project's own code (the launcher, the hand-written game code and the tooling) is licensed
under the [GNU General Public License v3.0](LICENSE). The ReXGlue SDK in `tools/rexglue-sdk` is
under the BSD 3-Clause License, as is the Xenia code it derives from. XenonRecomp in
`tools/XenonRecomp` is under the MIT License. Third-party libraries keep their own licenses; see
`tools/rexglue-sdk/thirdparty`.

## Credits

- [Xenia](https://xenia.jp), the Xbox 360 emulator research project whose code the runtime is
  built on.
- The ReXGlue SDK by Tom Clay, the recompilation runtime this port uses.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) by hedge-dev and contributors.
- tronuo, for the 60 FPS patch and the Havok physics fix.
- [Frank Kitzing (frankyfife)](https://github.com/frankyfife), for the free camera and photo mode,
  ink outline controls, clean eye shading, start episode selection, multilingual intro skipping,
  Windows timing and guest-address fixes, Havok step changes and audio/physics diagnostics.
  These contributions were developed in [his fork](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff)
  on 3–4 October 2026 and brought together in
  [#43](https://github.com/YesterMester/TheSimpsonsGameRecomp/pull/43).
- Everyone who has contributed code: Gabry179, awemancba, tronuo0 and anasalialamgir.
- The libraries the runtime is built with: SDL3, Dear ImGui, glslang, SPIRV-Tools, FFmpeg, spdlog,
  fmt and Tracy.
