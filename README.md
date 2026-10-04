# The Simpsons Game Recompiled — ff fork

A native PC port of *The Simpsons Game* (Xbox 360, 2007) for Linux, Steam Deck and Windows, made by
statically recompiling the original game code.

> **This is a modified version** of
> [YesterMester/TheSimpsonsGameRecomp](https://github.com/YesterMester/TheSimpsonsGameRecomp),
> changed by [frankyfife](https://github.com/frankyfife) since 3 October 2026. It is distributed
> under the same licenses as the original, the GNU GPL v3.0 for the project's own code (see
> [License](#license)). [This fork](#this-fork) describes the changes; the rest of this README is
> the original project's, adjusted only where the fork differs. Every change is in the git
> history with its date.

The game's PowerPC executable is translated ahead of time into C++ and compiled for x86-64, so the
game's own code runs directly on your CPU rather than inside an emulator. Underneath it, the
ReXGlue runtime, which is derived from the Xenia project, provides the Xbox 360 kernel, audio, input
and graphics layers, with a Vulkan renderer on Linux and Direct3D 12 or Vulkan on Windows.

**This project does not include any game content. You need your own copy of the Xbox 360 game.**

This fork publishes no release packages: [build it from source](#building-from-source). The
[original project's releases](https://github.com/YesterMester/TheSimpsonsGameRecomp/releases) do
not contain this fork's changes.

## Contents

- [This fork](#this-fork)
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

## This fork

What it adds to the original project:

- **Free camera and photo mode** in every level, with a controller or the keyboard.
- **Ink outlines to taste:** the game's hard black outlines can be softened, recoloured or turned
  off.
- **Clean eyes:** no more speckled dark shadow on the characters' eye whites.
- **Start episode (level select)** in the launcher: a new game can start in any of the 18 episodes.
- **Fixes:** the game running 7.5% fast on Windows PCs with a coarse system timer, even physics
  steps at 60 FPS, choppy, slowed-down sound on some Windows PCs, the menus' frame rate on
  Windows, and the skip-intro patch on releases that are not in English.

The changes were developed and checked on Windows 11. They build for Linux too but have not been
tried there.

### Free camera and photo mode

The free camera leaves the gameplay camera where it is and flies through the level. While it is on,
the game sees an idle controller, so the characters stand still while the level goes on around
them. The photo mode also pauses the level and hides the HUD and subtitles. Both work in levels,
not in menus or videos.

| Action | Controller | Keyboard |
|---|---|---|
| Free camera on/off | L3 + R3 (press both sticks) | F6 |
| Photo mode on/off | Y (with the free camera on) | F8 (also frees the camera) |
| Move along the view | Left stick | W A S D |
| Look around | Right stick | Arrow keys |
| Down, up | Left trigger, right trigger | Q, E |
| Zoom out, in | LB, RB | 1, 3 |
| Reset the zoom | Tap R3 | 2 |
| Faster, slower (hold) | A, X | Shift, Ctrl |

F6 and F8 can be changed in the launcher's **Keyboard & mouse** section, with the other overlay
keys. In `simpsons.toml`, `freecam_speed` sets the speed in metres per second (5 by default),
`freecam_hide_hud` whether the photo mode hides the HUD, and `freecam_game_follows` whether the
game streams, culls and hears from the free camera. The controller controls have been checked in
the game; the keyboard controls not yet.

The camera is the game's own developer fly camera, which the shipped game cannot reach, so the
level's culling, level of detail and projection follow it as they follow the gameplay camera. Code:
`simpsons/src/freecam.cpp`.

### Ink outlines

The comic-style black outlines over the geometry, one of the most criticised parts of the game's
look, can be changed in the launcher's **Settings** tab, section **Ink outlines**:

- **Hard (original):** as on the Xbox 360.
- **Soft (anti-aliased):** thinner, lighter lines. The strength slider (0.1 to 1.0) sets how dark
  they are.
- **Off:** no outlines. The colours, shadows and rim light stay as they are.
- **Colour:** black (the original), dark grey, dark brown, dark blue, dark red, or any RRGGBB
  value, for hard and soft lines.

The settings are `ink_outlines` (`original`, `soft` or `off`), `ink_outline_strength` and
`ink_outline_color` in `simpsons.toml`. The game is patched as it loads, so a change needs a
restart. Before patching, the game's data is checked against the release the patch was made for;
any other release is left as it is. Code: `simpsons/src/ink_outlines.cpp`.

### Characters' eyes

The original game, on the Xbox 360 too, draws a grainy dark crescent on the shaded side of the
characters' eye whites: the shader that leaves the eyes out of the rim shadow misreads the
artists' flag for that at scattered pixels. **Characters' eyes** in the launcher's **Settings** tab
is *Clean* by default, which reads the flag with a tolerance, so the eyes are plain white as they
were drawn. *Original* keeps the speckled look. Nothing but the eyes changes. The setting is
`eye_shading` (`clean` or `original`), patched as the game loads like the ink outlines. Code:
`simpsons/src/eye_shading.cpp`.

### Start episode (level select)

The launcher's **Patches** tab has a dropdown with the game's 18 episodes. A new game then starts
in the chosen episode instead of Land of Chocolate; games already in progress keep their progress.
The launcher changes `gamedata/simpsons_gameflow.lua` for this and keeps the untouched file as
`simpsons_gameflow.lua.original`. Choosing Land of Chocolate again restores the original file byte
for byte. Starting this way has been tried with Springfield and The Day the Earth Stood Stupid.

### Fixes

- **Game speed on Windows.** The game measures every frame with a millisecond tick count, which
  the runtime copies from its clock with a 1 ms timer. On Windows that timer only runs as often as
  the system timer lets it; with the default 15.6 ms the tick count moved in 15.6 ms steps, the
  game counted about 22 of every 300 frames twice, and everything ran 7.5% fast at 60 FPS: timers,
  sequences, characters and physics. The game's tick count now comes straight from the clock
  (`tick_count_precise`, on by default), and the game clock runs at real time.
- **Physics at 60 FPS.** The game steps its Havok physics in steps made for 30 FPS. The earlier
  60 FPS fix (tronuo's upstream pull request
  [#24](https://github.com/YesterMester/TheSimpsonsGameRecomp/pull/24)) was meant to give one 16.7 ms step
  per frame, but a rounding detail in the game's frame time smoothing made it take two 8.3 ms
  steps instead, about 120 a second, a step size the console never used. Now each frame gets one
  16.7 ms step per vblank it took: 60 steps of 16.7 ms a second at 60 and at 30 FPS, like the
  console's usual mode, and physics time follows the game clock exactly. `physics_step` in
  `simpsons.toml` chooses `steady` (default), `legacy` (the earlier fix) or `original` (the
  console code, 30 Hz steps at 60 FPS); it needs a restart. The fix is now made when the game is
  loaded, so `simpsons/generated` is again the recompiler's unchanged output.
- **Choppy, slowed-down sound on Windows**
  ([#36](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/36),
  [#38](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/38)). The audio thread paced
  the game's audio with a sleep that wakes on Windows' 15.6 ms timer tick whenever Windows does not
  grant the game a finer timer, and then a third of the played audio was silence. It now paces with
  a high-resolution timer (Windows 10 version 1803 or newer), and the game opts out of Windows 11
  ignoring its timer resolution request to save power. Measured outside the game; not yet confirmed
  on one of the PCs that had the problem.
- **Menu frame rate on Windows.** The frame pacing addressed the game's memory 4 KB off on Windows,
  so the menus never got their 30 FPS cadence (`menu_frame_rate`), and each change between a menu
  and a level wrote into an unrelated game object. Linux was not affected.
- **Skip intro on releases that are not in English.** The launcher's patch looked for the logo
  movies in `movies/en` only, so on discs with another language folder, such as `movies/de` on the
  German release, it showed *unavailable*. It now looks in every language folder.

For diagnosing audio problems, `audio_log_underruns` now also logs the audio pacing and the depth
of the output queue every 5 seconds, `audio_dump_file` writes all audio the game submits to a raw
file with timestamps, and the `REX_TIMER_STATS` environment variable also logs how often the game
arms each of its timers. For timing and physics, `physics_log` logs every 5 seconds the game
clock's rate, the real time between frames and the physics steps per frame, and for every second
with hazard damage how many damage messages the game's touch and trigger hazards sent.

### Branches

`main` contains all of the above. Each change is also on a branch of its own, based on the
original project's `main`:

| Branch | Change |
|---|---|
| [`freecam`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/freecam) | Free camera and photo mode |
| [`ink-outline-options`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/ink-outline-options) | Ink outlines and their launcher settings |
| [`eye-shading-fix`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/eye-shading-fix) | Characters' eyes |
| [`launcher-level-select`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/launcher-level-select) | Start episode (level select) |
| [`fix-windows-audio-pacing`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/fix-windows-audio-pacing) | Windows audio fix and audio diagnostics |
| [`fix-windows-guest-addresses`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/fix-windows-guest-addresses) | Menu frame rate on Windows |
| [`fix-skip-intro-languages`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/fix-skip-intro-languages) | Skip intro on releases that are not in English |
| [`fix-game-tick-count`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/fix-game-tick-count) | Game speed on Windows |
| [`fix-havok-step`](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff/tree/fix-havok-step) | Physics at 60 FPS and `physics_log` |

## Status

The original project's status:

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
- 60 FPS gameplay, with menus, the title screen and loading screens kept at the original 30 FPS so
  they run at the speed they were made for.
- Render resolution scaling (supersampling), anisotropic filtering and FXAA.
- A launcher that installs the game from your ISO, manages settings, patches and save backups,
  adds the game to Steam, and updates itself.
- Controller support, and keyboard and mouse controls with rebindable keys and an in-game list of
  the controls.
- In this fork: a free camera and photo mode, adjustable ink outlines, clean eyes and a start
  episode choice (see [This fork](#this-fork)).

## Requirements

- Your own copy of *The Simpsons Game* for Xbox 360, as an ISO image. The USA release (title ID
  45410809) is the main target. The European release also boots and plays but has seen less
  testing.
- About 5 GB of free disk space for the installed game data.
- A 64-bit x86 CPU. The build commands below compile for CPUs with AVX2 (Intel Haswell, AMD
  Excavator or Zen, or newer, roughly 2013 onwards). For older CPUs with SSE4.2, build for
  x86-64-v2 instead (see [Building from source](#building-from-source)); it runs the same game,
  somewhat slower.
- A GPU with a current Vulkan driver (Linux) or Direct3D 12 driver (Windows).
- A controller is recommended.

## Installation

1. Build the game and the ISO extraction tool as described in
   [Building from source](#building-from-source).
2. Start the launcher from the repository folder:
   - Linux: run `launcher/simpsons-launcher.sh`.
   - Windows: run `python launcher\launcher.py`.
3. In the **Install** tab, select your Xbox 360 ISO. The launcher extracts and installs the game
   data.
4. Press **Play**.

On Steam Deck, use **Add to Steam** in the launcher to play from Game Mode.

To start the game without the launcher window, for example from your own Steam shortcut, add
`--play` to the launcher command. It uses the same settings as the launcher's Play button.

**Updates.** The launcher's **About** tab offers the original project's releases as updates.
Installing one replaces this fork's engine and launcher with versions that do not have its
changes; your settings, saves and mods are kept either way. To update this fork, pull the new
commits with `git pull` and build again.

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
gives the outlines their cleanest look. On a Steam Deck, 1x holds 60 FPS; 2x looks much sharper
and runs at about 59 FPS in Springfield, the busiest area, with occasional drops. This fork's
**Ink outlines** and **Characters' eyes** settings are described under [This fork](#this-fork).

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
the in-game overlays (F1 controls, F4 settings, F3 FPS overlay, ` console, and in this fork F6 free
camera and F8 photo mode) can be changed there too. In the game, F4 opens the settings, with the
controls under Input > Keybinds (press *Save to config* to keep changes made there).

## Known issues

- Random deaths were reported at 60 FPS in one section of "Lisa the Tree Hugger", the walkway
  with circular saws and conveyor belts
  ([#2](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/2)), on a build from before
  any physics fix; switching to 30 helped then. Whether it still happens with this fork's physics
  and game speed fixes has not been tested yet. If it does, switch to 30 for that section.
- On Windows PCs where the system timer stays at 15.6 ms, the 30 FPS setting runs the game about
  7.5% fast, with frames alternating between 31 and 47 ms: the runtime makes the 30 Hz vblanks
  itself and paces them with 1 ms sleeps. At 60 FPS the frames follow the display's vsync and
  were measured even.
- If videos show a black screen on Windows, switch the graphics backend to Vulkan in the
  launcher's settings.

## Building from source

The game and the ReXGlue SDK are built together in one CMake project. The original project's
continuous integration uses exactly these steps; see `.github/workflows/build.yml` for the full
list of Linux packages.

### Linux

Requirements: Clang 20, CMake 3.25 or newer, Ninja, pkg-config, and the development packages for
GTK 3, Vulkan, X11 and XCB, Wayland, xkbcommon, udev, ALSA, PulseAudio and PipeWire.

```sh
git clone https://github.com/frankyfife/TheSimpsonsGameRecomp_ff.git
cd TheSimpsonsGameRecomp_ff/simpsons
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
git clone -c core.longpaths=true https://github.com/frankyfife/TheSimpsonsGameRecomp_ff.git
cd TheSimpsonsGameRecomp_ff\simpsons
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

Windows builds include both the Direct3D 12 and Vulkan renderers; the launcher's settings choose
between them. Without PySide6 the launcher opens in your web browser instead of its own window.
`core.longpaths` lets Git check out the repository's longest paths when the folder you clone into
is itself deep.

For a CPU without AVX2, replace `x86-64-v3` with `x86-64-v2` in the first `cmake` command and add
`-DSIMPSONS_X86_MARCH=x86-64-v2` to it, as the original project's release workflow does.

## Contributing

Fixes and features for the game in general are best made in the
[original project](https://github.com/YesterMester/TheSimpsonsGameRecomp), where every player gets
them. Its guidelines follow; this fork keeps each of its own changes on a separate branch so that
it can be offered there on its own.

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

Problems with this fork's changes, the ones listed under [This fork](#this-fork), are not the
original project's: please do not report them there. Before reporting anything else to the original
project, check that it also happens with its own release.

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

The original project's roadmap. Its goal is for this to be the best way to play the game. In rough
order:

- **Fully native renderer.** The Xbox 360 GPU emulation is being replaced piece by piece with
  native rendering, each step checked to give exactly the same image. Done: every shader compiled
  ahead of time, native replacements for the most expensive shaders, render-to-texture and copies
  done natively instead of through the emulated EDRAM, and render targets sized like native ones.
  Next: real vertex and index buffers, and textures and buffers uploaded when the game loads them
  instead of watching its memory. A native renderer is also what the features below build on.
- **No slowdowns or stutters.** A steady 60 FPS everywhere, including at 2x internal resolution
  on the Steam Deck, with no shader compilation hitches.
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
sponsored by Electronic Arts, Fox, Microsoft or any other rights holder. This fork is likewise not
affiliated with or endorsed by the original project.

This repository contains no game assets: no textures, models, audio, video, scripts or level data.
The code in `simpsons/generated` is produced by recompiling the game's executable code and does not
contain the game's data. The game runs only with data installed from an ISO of your own copy, and
nothing from your installation is uploaded anywhere.

Please support the original release. Do not use this project with copies of the game you do not
own, and do not ask for or share game files in this repository or the original project's issues or
discussions. You are responsible for complying with the laws that apply to you regarding backups of
media you own.

This software is provided as is, without warranty of any kind.

**A note on AI assistance:** parts of this project, including research into the game's internals,
build tooling and bug fixes, have been developed with the assistance of Claude AI, in the interest
of getting a working release out and turning around fixes as quickly as possible for a solo,
fan-made effort. Treat it as a fast-moving hobby project rather than a polished commercial
release. This fork's changes were also made with the assistance of Claude AI.

## License

The project's own code (the launcher, the hand-written game code and the tooling) is licensed
under the [GNU General Public License v3.0](LICENSE). This fork is a modified version of
[YesterMester/TheSimpsonsGameRecomp](https://github.com/YesterMester/TheSimpsonsGameRecomp), and its
changes to that code are licensed under the GNU GPL v3.0 as well.

The ReXGlue SDK in `tools/rexglue-sdk` is under the [BSD 3-Clause License](tools/rexglue-sdk/LICENSE),
as is the Xenia code it derives from. This fork changes some of its files (audio pacing and
diagnostics, timer statistics, the Windows timer resolution request and the input injector); they
keep their copyright and license notices, and the fork's changes to them are under the same BSD
3-Clause License. XenonRecomp in `tools/XenonRecomp` is under the
[MIT License](tools/XenonRecomp/LICENSE.md). Third-party libraries keep their own licenses; see
`tools/rexglue-sdk/thirdparty`.

## Credits

- [YesterMester](https://github.com/YesterMester), for
  [The Simpsons Game Recompiled](https://github.com/YesterMester/TheSimpsonsGameRecomp), the
  project this fork is based on.
- [Xenia](https://xenia.jp), the Xbox 360 emulator research project whose code the runtime is
  built on.
- The ReXGlue SDK by Tom Clay, the recompilation runtime this port uses.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) by hedge-dev and contributors.
- tronuo, for the 60 FPS patch and the Havok physics fix.
- Everyone who has contributed code to the original project: Gabry179, awemancba, tronuo0 and
  anasalialamgir.
- The libraries the runtime is built with: SDL3, Dear ImGui, glslang, SPIRV-Tools, FFmpeg, spdlog,
  fmt and Tracy.
- This fork: [frankyfife](https://github.com/frankyfife), with Claude AI.
