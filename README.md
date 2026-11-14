# The Simpsons Game Recompiled

A native port of *The Simpsons Game* (Xbox 360, 2007) for Linux, Steam Deck and Windows, with an
Android ARM64 development preview, made by statically recompiling the original game code.

The game's PowerPC executable is translated ahead of time into C++ and compiled for x86-64 or
Android ARM64, so the
game's own code runs directly on your CPU rather than inside an emulator. Underneath it, the
ReXGlue runtime, which is derived from the Xenia project, provides the Xbox 360 kernel, audio, input
and graphics layers, with Vulkan on Linux and Android, and Vulkan, Direct3D 12 or an experimental
Direct3D 11 renderer on Windows. Native graphics resources and API conversion are tracked
separately in [Native GPU progress](#native-gpu-progress).

**This project does not include any game content. You need your own copy of the Xbox 360 game.**

[Download the latest release](https://github.com/YesterMester/TheSimpsonsGameRecomp/releases/latest)

## Contents

- [Status](#status)
- [Features](#features)
- [Native GPU progress](#native-gpu-progress)
- [Modding and Modkit](#modding-and-modkit)
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
| Android (ARM64) | Development preview; tried on a Pixel 9 Pro so far |

The game boots, plays its videos, saves and loads, and runs its levels. See
[Known issues](#known-issues) for what still misbehaves.

**0.0.7.0** adds native graphics-call conversion (`native_gpu_calls`, on by default), an
Android preview, settings and player assignment in the game's own menus, two-player free roam
in Springfield, cutscene skipping, mod packages and a separate desktop Modkit preview with
translation tools. The debug menu also has experimental scene and character tools; NPC
possession and full character ragdolls are unfinished. The
[release notes](.github/release-notes.md) list everything, and the
[character-tool notes](simpsons/re/native_ragdolls.md) describe what those tools do today.

## Features

- Recompiled game code runs directly on x86-64, with an ARM64 build for Android.
  Desktop downloads include a NoAVX2 variant for older CPUs.
- Vulkan on Linux and Windows, Direct3D 12 on Windows, and an experimental Direct3D 11 backend
  for Windows 10/11. Android uses Vulkan with device-dependent shader preparation.
- Native vertex and index buffers, texture uploads, render targets, colour/depth/stencil
  resolves, surface transfers and native GPU API call conversion (`native_gpu_calls`, enabled by
  default) across the three desktop renderers. Retained versions protect queued draws from later
  writes. Supported GPU-owned and overlapping inputs stay on the GPU; guarded cases keep their
  existing path. See the [renderer plan](simpsons/re/native_renderer_plan.md).
- Ahead-of-time shader variants for matching desktop configurations, persistent driver caches,
  separate vertex/pixel program identities and correct invalidation when shader code changes.
- 60 FPS gameplay by default, with 30, 90, 120, 144, 165, 240 and Unlimited choices. Menus keep
  their original logic cadence, physics uses fixed steps and videos keep their authored timing.
  Some campaign scripts still need higher-rate checks.
- Settings inside the game's own main and pause menus, including live changes where supported.
  Mouse clicks and touch work on native menu rows, sliders, episode/movie browsers and maps.
- Independent player assignments and font-based prompts for Xbox, PlayStation, Nintendo,
  Steam Deck and keyboard. Keyboard/mouse and a controller can share a two-player game.
- Two-player split-screen free roam in Springfield, using the game's own co-op players and
  cameras. Episode and time-trial player limits remain as authored.
- Render scaling, texture filtering, FXAA, CAS sharpening and FSR1 image scaling. Free camera,
  photo mode, zoom, HUD hiding, ink-outline controls and cleaner character eyes are included.
  Character tessellation and colour grading are optional Vulkan features.
- Native audio scheduling, mixer/decoder lifetime fixes and improved recovery under CPU load.
  Android folds centre-channel dialogue into stereo for phone speakers and headphones.
- Original save slots and checkpoints, local backups and all 44 Xbox 360 achievements,
  including recognition of progress from older saves. The save-library experiment is shelved.
- A debug menu drawn in the game, with health, family characters, teleports, game speed,
  gravity, progress unlocks and a whole-game resource index. Spawning uses exact authored
  definitions and loaded sources. Unsupported or ambiguous assets fail without substituting
  a different NPC. Arbitrary NPC AI suspension and cross-rig animation playback are unfinished.
- **Animations & poses** lists the controlled character's loaded clips by their authored
  names and checks that the selected clip belongs to that character before playback.
- Experimental **Spooner** freecam editing of supported loaded entities: selection, movement,
  rotation, scale, cloning, replacement and reversible parking of existing map actors.
  Authored AI/scripts remain active; this is not a complete offline world editor.
- An experimental NPC costume preview follows the controlled family character. It preserves
  the original family HUD and abilities; it does not possess an NPC's controller or attacks.
- A preinstalled **Experimental Knockdowns** example (`euphoria.ragdolls`), disabled by default.
  Its whole-character root motion is a prototype and stays separate from the native work.
  The launcher adds it to the Mods tab once; if you remove it, it stays removed.
- An opt-in **native family ragdoll test** is in **Debug Menu → Ragdolls & knockdowns**.
  It uses 13 physical bodies and 12 joints with separate passive limits, drives the actual
  skeleton, and restores animation and controls after two simulation seconds. Paused menus
  keep their controls, and recovery preserves any game-owned teleport. Linux checks cover
  collision handoff, movement blocking, repeated recovery and a level exit/reload.
  This is a passive test; NPC ragdolls, anatomical tuning, animated get-up and phone qualification are
  [in progress](simpsons/re/native_ragdolls.md). **Known issue:** the same physics froze the game
  when the other family member touched the fallen body; keep them apart while testing.
- Development **Quick launch level (dev)** selection (`quick_launch_level`) uses a private gameflow
  override and advances the original front end automatically. It is intended for isolated
  Modkit Test profiles; it does not establish a native level-loader bypass.
- A launcher for installation, settings, patches, mod packages, save backups, updates and
  support bundles. Linux packages include a native window and Steam Deck Gaming Mode support.
- A separate Windows/Linux Modkit preview for Lua/C++ mods, scenes, missions, assets and
  translation packs. Android imports and manages mod packages.

## Native GPU progress

The progress script measures recorded work at defined graphics boundaries. The snapshot below
is the November 4 private Steam Deck/RADV Vulkan startup/title run: 1,079 frames at 1x with
experimental `native_gpu_calls` enabled. It is not a whole-game completion percentage.

<!-- native-progress-vulkan:start -->
```text
Draw command dispatch     ████████████████████   100%
Geometry source selection ████████████████████   100%
Texture upload commits    ████████████████████   100%
Host resolve requests     ████████████████████   100%
Shader command dispatch   ███████████████████░   99.89%
GPU synchronization       ███████████████████░   99.75%
Buffer submissions        ███████████████████░   <100%
Presentation commands     ████████████████████   100%
State and constants       ███████████████████░   99.55%
Queries                   ····················   not exercised
Other commands            ░░░░░░░░░░░░░░░░░░░░   0.00%
Host presentation         ███████████████████░   99.35%
```
<!-- native-progress-vulkan:end -->

The run records 3,367,526 native draw dispatches, no compatibility resource dependencies and
no compatibility allocations. Eleven of twelve systems were exercised. Queries were not
exercised; other executed commands still include retained markers. Seven startup host-refresh
attempts had no refreshed output and remain counted. Buffer submissions also retain four
attempts, so the script deliberately shows less than 100%.

Separate older generated geometry fixtures record 564 native draws and zero legacy draw
packets for each of Vulkan, Direct3D 11 and Direct3D 12. Windows APIs ran through Proton on
the Deck. Those reports lack the newer system counters, so their other percentages remain
unknown. They are not combined with the title sample or treated as Windows/Android coverage.

The [snapshot, input reports and reproduction commands](simpsons/re/native-progress/0.0.7.0/README.md)
keep the counts and hashes together. The [measurement guide](simpsons/re/native_progress.md)
explains each denominator. Native graphics-call conversion (`native_gpu_calls`) is enabled by
default in 0.0.7.0 and can be toggled in the Launcher or in-game Graphics settings; ordinary
native resource paths also have their own settings. Remaining API contracts, campaign states
and device checks are tracked in the [conversion notes](simpsons/re/native_gpu_calls.md).

## Modding and Modkit

The separate **Modkit** application opens with `Modkit.sh` on Linux or
`simpsons-modkit.exe` on Windows. The game launcher handles play, settings and installed mod
packages; the desktop editor handles authoring. Android only imports and manages packages.
The kit is a preview: the [full release gate](modkit/PLAN.md) is still open.

- Create credited projects from commented Lua, native plugin, input, owned-object, scene,
  mission, combat, camera and file-replacement examples. Validate, Test and export ZIPs.
  Packages import disabled and keep creator/translator credit, dependencies and file hashes.
- Use named Lua/C++ operations for players, transforms, health, cameras, native menu pages,
  events, simulation timers and saved mod state. Code reload prepares replacements in the
  background, preserves supported state/timers and rejects a failed replacement before commit.
- Edit source in independent tabs with undo, revision-checked saves, Find/replace, Quick open,
  a command palette and built-in API/help pages. Code navigation links generated functions
  to reviewed system names, callers and public operations.
- Test with separate progress and a real native game viewport on supported Windows/X11
  systems. Direct level previews select installed standard map entries. Paused free-camera
  controls and geometry/actor visibility help inspect a scene without playing through it.
- Search the game-wide resource library, use rotating model hover previews and inspect
  supported decoded geometry before Play. Live supported props can be dragged into the scene,
  moved, rotated, resized and removed with undo/redo and portable drafts. Offline mesh views
  show geometry; complete offline world editing, native materials and transform gizmos remain.
- Author route missions with objectives, dependencies, time limits, failure and retry points.
  Scene and mission drafts can be applied together. Combat and camera workshops use the same
  native ownership services; broader AI, campaign HUD and cinematic sequencing remain.
- Inspect native model parts, export supported geometry to OBJ and package bounded vertex,
  normal and UV edits. New topology, rigs, animation, collision and world import remain work.
- Make translation packs without writing gameplay code: export source text, search and edit
  catalogs, validate placeholders/button tags, track untranslated lines and export a partial
  language pack with translator credit. Missing translations retain their fallback text.
  Complete Unicode font coverage and right-to-left layout still need work.

The [Modkit guide](modkit/README.md), [editor workflow](modkit/EDITOR_WORKFLOW.md),
[scene tools](modkit/SCENES.md), [mission tools](modkit/MISSION_EDITOR.md) and
[translation workflow](modkit/README.md#translations) cover the supported paths and limits.
Browsing an asset does not mean every class can spawn with complete behavior. Native Windows
drivers, Android devices and the remaining creator acceptance projects need separate checks.

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

**Android (preview):** on an ARM64 phone with Android 10 or newer and Vulkan 1.1, download
`TheSimpsonsGame-Recompiled-Android-arm64.apk` from the release and open it, allowing installs
from your browser or file manager when Android asks. The app installs the game from your ISO or
an already extracted folder, and offers later releases itself.

On Steam Deck, use **Add to Steam** in the launcher from Desktop Mode, then find
**The Simpsons Game (Recompiled)** under Steam's **Non-Steam Games** to play from Game Mode.
If this installation is already registered, the button shows its existing Steam name instead
of adding another copy. It reports command errors and keeps Steam running while it starts.

The 0.0.7.0 Linux launcher bundles its native window and opens fullscreen in Game Mode.
Steam Linux Runtime 1.0 is supported. When Steam runs the shell launcher inside Runtime 2,
it starts the native launcher alongside Steam, keeping the game's Steam identity and controller
environment. **Add to Steam** uses a desktop entry for the game's name and donut icon,
handles spaces and special characters in the installation path, and sends sandboxed requests
to the same host as Steam. If Steam opens an Add a Game dialog, finish it in Desktop Mode.

To start the game without the launcher window, for example from your own Steam shortcut, run
`Play.sh --play` on Linux or `simpsons-launcher.exe --play` on Windows. It uses the same settings
as the launcher's Play button.

The launcher can update itself from the **About** tab. Updates replace only the engine and
launcher files. Your settings, saves and mods are kept. If the installed package does not match
your CPU, the launcher offers the right one.

Updating from 0.0.6.5 or earlier takes two steps. That launcher copies only the files its own
version had, so after it finishes and you restart, the new launcher offers to finish the update
and installs what 0.0.7.0 added: the mod runtime's description, the Modkit and, on Linux, the
launcher's own window. Until then the game plays, but mods cannot be enabled. In Steam Deck
Game Mode this second step runs by itself on the first start.

**Windows ISO extractor.** The 0.0.7.0 packaging uses the unchanged 41,472-byte Win64 Release
build from [XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso/releases/tag/build-202609111233),
rather than rebuilding it with an embedded C runtime. Its download and executable hashes are
pinned and checked before packaging. The launcher's installer supplies the required C runtime
from a separate folder. `extract-xiso-source.json` records the upstream version, source and
packaged file hashes; the upstream license is included under `licenses/extract-xiso`.

**Antivirus warnings.** The Windows programs are not code-signed. If security software flags a
file, report the product, exact detection name and the file's SHA-256 so the actual binary can
be checked. A public build or a smaller file does not by itself establish that a detection is
a false positive. Microsoft provides a [file review service](https://www.microsoft.com/en-us/wdsi/filesubmission)
for incorrectly detected software.

The Windows launcher build pins PyInstaller and compiles its bootloader from source, without
using a cached wheel or falling back to the prebuilt bootloader. This does not replace checking
the final release package with the antivirus that reported it.

On Windows laptops with integrated and dedicated graphics, the game executable requests the
high performance GPU before the renderer starts. The graphics driver or Windows graphics
preferences can override that request. Include the selected adapter from the game log when
reporting poor laptop performance.

## Configuration

Most settings are in the launcher's **Settings** tab, saved as soon as you change them. They are
written to `simpsons.toml` next to the game executable, inside a block the launcher manages.
Settings outside that block can be edited by hand. The launcher has a light and a dark theme; the
button at its top right switches between them.

Click a settings heading to expand or collapse it; Enter and Space work too. Sections start
collapsed and remember which ones you opened. **Expand all** and **Collapse all** also control
the subsections. To change the debug menu's keyboard shortcut, open **In-game shortcut keys**
and choose **Change** beside **Debug menu** (F2 by default).

Saves and the shader cache are stored in:

| Linux | Windows |
|---|---|
| `~/.local/share/simpsons` | `%LOCALAPPDATA%\simpsons` |

**Saves.** The original four slots, checkpoint saves and save menus are used. The experimental
save library and save-anywhere menu are shelved for now. Existing library files are kept in
the data folder; this change does not delete saves or convert them over your original slots.

**Achievements and the debug menu.** Both are drawn in the game, once a save is loaded. While
playing, hold RB and press Left on the D-pad for the debug menu; in the pause menu, press Y for
the achievements and X for the debug menu. F5 and F2 open them on a keyboard, and both are under
*Options → Extras*, which is also the way in with touch. The debug menu's top bar has a close
button, and a back arrow on its inner pages.
Achievements are kept in `achievements.txt` next to your save slots, so the launcher's save
backups include them; `achievement_notifications = false` in `simpsons.toml` turns the
unlock pop-ups off. Spawned things go when the level unloads; other debug menu changes
last until the game restarts, except power upgrades and infinite power, which the game saves.

**Cheats.** The debug menu's god mode, teleports, characters, spawning, game speed, gravity and
unlocks are cheats: they work once *Cheats* at the top of the debug menu is on, and
`cheats = true` in `simpsons.toml` keeps it on. Achievements still unlock with
cheats on; the achievements screen then shows *Cheats detected* and marks what was unlocked
that way. The free camera isn't a cheat, but with cheats on, B (F9) in the free camera brings
the family to where the camera is. *Player → Simpsons family & costumes* lets players 1 and 2
play as anyone, including the episode costumes, and restarts from the last checkpoint; levels are
made for their own characters, so story triggers can wait for the right one, and *The level's
character* brings them back.

**Frame rate.** The launcher's *Frame rate in levels* setting (`frame_rate` in `simpsons.toml`)
offers 30, 60 (the default), 90, 120, 144, 165, 240 and Unlimited (0). 30 matches the original
console exactly. At 60 and 30 the game paces frames with its own vblank timing. At other rates
each frame is measured precisely, so game time follows real time, and Havok physics keeps
stepping 1/60 s, 60 times a second, as at 60. Choose your display's refresh rate: a fixed-refresh
display never shows the extra frames, and with Unlimited they are dropped unevenly, which makes
motion and thin distant outlines shimmer. Menus, the title screen, loading screens and the pause
menu were made for 30 FPS and count their logic in frames (held directions repeat every few
frames, for example), so by default they run at 30 (*Frame rate in menus*, `menu_frame_rate`).
Set to the level rate (0), they are drawn at that rate while their logic keeps its 30 FPS pace.
Their animations play at their authored rate either way, and videos keep their own timing at any
frame rate.

**Asset streaming.** The optimized RefPack decompressor reduces work during
loading and streaming across the graphics backends. A paired Steam Deck boot and checkpoint
check decoded the same 175 MiB in 342 ms, compared with 595 ms for the original routine:
42% less measured decoding time. Windows and Android performance measurements are pending.

**Image quality.** FXAA anti-aliasing is on by default for new installs and smooths the
cel-shading outlines at little cost. A render scale of 2x or 3x supersamples the whole image, which
gives the outlines their cleanest look.

**Steam Deck frame rates (0.0.7.0).** Measured on a Steam Deck LCD with the Linux package's
default settings, except the frame rate (also in menus) unlocked and V-sync off, in a 1280×720
window in Desktop Mode, with the test tools' per-frame log and GPU telemetry on. Each row
is the median of three 40-second samples (four for Neverquest) after a warm-up run; the 1% low is
the frame rate at the 99th-percentile frame time. In play the game caps levels at 60 FPS by
default. These are two scenes; performance varies through the game.

| Scene | Render resolution | Average | 1% low |
|---|---|---|---|
| Springfield, the Simpsons' house (start of the hub) | 1× | 132 FPS | 100 FPS |
| Springfield, the Simpsons' house | 2× | 103 FPS | 66 FPS |
| Springfield, the Simpsons' house | 3× | 53 FPS | 44 FPS |
| Neverquest, the dragon attack at the start | 2× | 110 FPS | 56 FPS |

**Audio.** The launcher's *Audio buffer* setting chooses how many 5.3 ms audio frames are queued:
Small (16), Normal (32, the default) or Large (64). Without a launcher setting the game uses 8.
Values from 4 to 64 can be set with `audio_maxqframes` in `simpsons.toml`. On the Steam Deck,
the Linux build passes the heavy CPU load check at 8 and 16 frames. The Windows build, tested
through Proton, has no queue underruns at 16 and 32 frames, but can still produce occasional
silent mixer blocks under that synthetic load. The minimum of 4 can still underrun under heavy
load. Updating keeps your saved setting.

Audio pause requests now take priority over newly available buffer slots. A
continuously draining queue can no longer starve the worker's pause request
during shutdown; a callback already running finishes before the worker pauses.

**In-game settings.** Open **Options → Recomp settings** from the main menu or the
pause menu. Graphics, display, sound and keyboard controls use the game's own TV screen, fonts
and buttons. Changes save automatically to the same config as the launcher. Settings that need
a restart are marked; frame rate, FXAA, texture filtering, image scaling, sharpness, fullscreen
and mute can change while the game is running. The original Audio and Controls screens remain,
with Downloads and Credits under Extras.

The original menus also accept mouse clicks and touch input. Point at a row to select it, click
or tap to activate it, and use the on-screen Back button or right click to return. Tap the left
or right side of a slider or choice to adjust it, or use the mouse wheel. In Replay Episode,
the movie browser and the clichés, tap the arrows or a picture beside the current one, or swipe
sideways; tap the current episode or movie to play it, and tap a playing movie to stop it.
Keyboard and controller navigation remain available.
On Android, the touch gamepad hides while a menu is open so you can tap the menu directly.

**Players.** Open **Options → Players** to choose the input for each player, or
assign it by pressing a button. Keyboard and mouse can take either player slot alongside a
controller. Moving an assigned input to the other player swaps their assignments. Button prompts
can follow the assigned controller automatically or use an Xbox, PlayStation, Nintendo, Steam Deck
or keyboard layout for each player. Font-based prompts include the ability icons in the HUD,
including separate prompts for the two players in split screen. Button names baked into artwork
still use the original design.

Springfield free roam also accepts a second player. Assign their input in **Players**, then
press Start on that input to join. **Two players in Springfield** is enabled by default;
changing it requires a restart. Episode and time-trial player limits are kept.

**Keyboard and mouse.** Turn on *Play with keyboard & mouse* in the launcher's Settings tab, or
use **Options → Recomp settings → Keyboard and mouse**. The game takes the
mouse during gameplay and releases it in menus or when you switch to another window.
Press F1 for the list of bindings. The default layout:

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
too. You can also rebind actions in **Options → Recomp settings → Keyboard
and mouse → Key bindings**. Use the arrow keys and Enter to navigate, and Escape to go back.
F4 still opens the runtime's advanced settings, with controls under Input > Keybinds (press
*Save to config* to keep changes made there).

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

**Character tessellation.** The launcher offers Off (the default), Low, Medium and High on
Vulkan. It rounds supported skinned character meshes from their existing positions and normals,
adding more triangles where an edge is large on screen or bends sharply. Higher levels cost more
GPU time. Unsupported shaders and meshes keep their original geometry; it does not add detail
to scenery or textures. This is experimental and has not been checked across the full campaign.
The setting is `character_tessellation` and requires a restart.

**Colour.** Vulkan offers Original (neutral), Vivid, Punchy, Soft and Custom looks. Custom exposes
vibrance, saturation, contrast and brightness. Vibrance raises muted colours more than vivid
ones. The settings are `color_vibrance`, `color_saturation`, `color_contrast` and `color_brightness`;
they require a restart. Colour grading shares the FXAA pass when it is enabled; without FXAA it
uses a separate pass. Original skips colour grading entirely.

**Skip cutscenes.** Enable **Skip cutscenes** in the launcher's **Patches** tab, or in the game's
**Extras** menu. During a story video, tap another button to reveal the bottom-right skip hint;
press B to pause the video and ask for confirmation. A skips; B keeps watching. Prompts follow
your controller or rebound keyboard controls. The hint and question use the game's own renderer,
fonts and button icons. You can also click or tap the hint and the confirmation choices. This
works on a first viewing and does not require changing the save or video files.

**Start episode.** The launcher's **Patches** tab can start a new game in any of the 18 episodes.
As the chosen episode starts, the new game runs the game's own episode cheat, which marks every
earlier episode complete (Springfield is unlocked, for example). As with that cheat, those
episodes' completion achievements unlock too. Starting in Game Hub adds no progress. While the
patch is on, the same happens whenever a save starts that episode; in an existing save that only
marks episodes it has already finished, and it cannot replace a real best time (the game keeps
the lower one). The launcher keeps `simpsons_gameflow.lua.original` beside
the modified gameflow script; choosing Land of Chocolate restores it exactly. If another tool
edits the script while this patch is active, the launcher keeps both files and refuses to
replace the external edits. Individual episodes may depend on progress from earlier levels;
starting every episode this way has not been checked through a full play-through.

**Game timing.** `physics_step` selects Steady (the default), Legacy (the previous 60 FPS fix)
or Original (the console code). Steady uses the game clock's frame time instead of its smoothed
copy, avoiding the extra 8.3 ms Havok steps at 60 FPS, and at rates other than 60 and 30 takes
every step at 1/59.94 s. `tick_count_precise` reads the millisecond clock directly so delayed
background timer updates cannot distort the game's frame time. Both settings require a restart.
`physics_log` also reports how the frames the game clock counted compare with real time.

0.0.7.0 also bounds animated collision-body corrections by the simulation step
([#49](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/49),
[#50](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/50)).
At high frame rates, the original correction could overshoot and leave a platform's collision
shape far from its visible model. The original position, rotation and activation calculations
remain in use.

**Diagnostics.** `audio_log_underruns` also reports callback rate, callback time, wait lateness
and queue depth. `audio_dump_file` writes submitted six-channel, 256-sample big-endian float
frames and a companion `.ts` file with one host `uint64` monotonic nanosecond timestamp per frame;
it is off by default and requires a restart. Recording to a slow disk can affect audio timing.
`physics_log` reports physics steps and hazard damage messages. `REX_TIMER_STATS` adds per-timer
arming rates and intervals to the existing timer diagnostics.

## Known issues

- Above 30 FPS some scripted sequences can misbehave, because the game was built for 30 FPS.
  Random deaths in "Lisa the Tree Hugger" were reported in
  [#2](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/2). The fork's author reported
  completing the affected section at 60 FPS with the timing fixes. Physics steps the same way at
  every rate above 30, but the whole campaign still needs testing at 60 and above; switch to 30 if
  it happens.
- If videos show a black screen on Windows, switch the graphics backend to Vulkan in the
  launcher's settings.
- The Direct3D 11 renderer is experimental. It has been tested through Proton on a Steam Deck,
  not yet on Windows drivers. Through Proton, if the log reports an unsupported `SHDR` shader
  chunk, install Microsoft's shader compiler with `protontricks <app id> d3dcompiler_47`: Wine's
  own compiler produces shaders the renderer cannot use yet. Windows includes the right one.
- Character tessellation is experimental. It has been checked at every setting in a few
  episodes, not across the whole campaign or on Windows drivers.
- The 0.0.6.5 download can lose right-stick camera input above 60 FPS
  ([#45](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/45)). 0.0.7.0 fixes a
  cutoff that discarded vertical stick input on short frames. Both players and both camera input
  handlers are covered by the native regression check; 30/60 FPS behavior and turn speeds are
  preserved. See the [community bug fixes](simpsons/re/community_bug_fixes.md) for validation
  and the remaining reports.
- Native rendering is still being completed. Unsupported resources and resolves use the
  existing fallback. On Windows, the native resource paths have been tested with the Windows
  build through Proton on a Steam Deck, not yet on Windows drivers.
- The minimum 4-frame audio queue can still underrun under heavy CPU load. Windows audio has
  been checked under load with the Windows build through Proton, not yet on Windows itself.
- Initial Android phone testing has reported stutters and audio skips. Audio thread priority,
  background compilation limits, the adaptive frame-wait limiter, performance-hint session
  recovery and the Vulkan upload-memory fix still need measurements on Tensor and Snapdragon
  phones. The app requires a 4 KB memory-page kernel; rendering and suspend/resume need further
  testing. See [Android setup](android/README.md).
- Full widescreen rendering is still in progress. Turning off letterboxing stretches the
  original image. The development camera hook alone does not widen the game's render targets
  or adapt its HUD and post-processing.

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

The source launcher needs Python and PySide6 for its native window. Release packaging builds
a standalone Linux launcher with `tools/launcher/build_linux.py` and the pinned dependencies
in `tools/launcher/requirements-linux.txt`; the resulting package includes their licences.

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

### Android development

The ARM64 app uses Vulkan, the shared launcher and a multitouch gamepad. Build and installation
instructions, requirements and current limits are in [android/README.md](android/README.md).
The phone launcher includes the donut app icon, startup progress and game-folder imports that
continue in the background with progress notifications. Choose a game ISO to extract it on
the phone, or import an already extracted folder; your saves and settings are kept separately.
The Patches tab includes reversible logo skipping, cutscene skip confirmation and all 18
start-episode choices. The touch
layout editor lets you move, resize, fade or hide individual controls, with stick deadzone and
camera sensitivity settings. Android audio workers use audio priority, background pipeline
compilation leaves more CPU time for gameplay, and the hidden launcher pauses during play.
Before the first game frame, a preparation screen builds known shader variants and supported
pipeline states for the phone, then reuses compatible cached results on later launches.
The Android frame limiter adapts its sleep margin to the scheduler's wake-ups, reducing
busy waiting while retaining the game's deadlines. Performance hints recover from service
interruptions and activity changes, and retired rendering threads leave the hint session.
These paths support both Tensor and Snapdragon devices; phone timing still needs measurement.
See [Android performance](simpsons/re/android_performance.md) for checks and current results.
Android also mixes the game's centre dialogue channel into stereo before sending audio to
the device, including phones whose audio driver accepts six-channel streams.
The launcher's pictures come from your game's cutscenes, as on the desktop. The app checks the
latest GitHub release when the launcher opens and offers to update when the release has a newer
APK; it downloads it, checks that it is signed with the same key as the installed app, and
hands it to Android's package installer, which asks before installing.

The separate manual Android preview workflow builds an APK artifact using the owner's
signing key and checks its certificate, native dependencies, alignment and shader assets.
Local debug builds keep their existing development certificate. See the
[Android build guide](android/README.md#signed-preview-packaging) for signing setup and
package verification. Phone performance and lifecycle checks remain separate from packaging.

## Contributing

Contributions are welcome, from bug reports and testing to fixes and new features. Read
[CONTRIBUTING.md](CONTRIBUTING.md) for setup, coding guidelines, checks and pull requests, and
please follow the [Code of Conduct](CODE_OF_CONDUCT.md).

See [Accessibility](ACCESSIBILITY.md) for current options, known barriers and how to report them.
For security issues, follow the short [security policy](SECURITY.md) and report them privately.

## Reporting bugs

Before opening an issue, check the [known issues](#known-issues) and the existing issues. When you
open one, include:

- Your platform, operating system, CPU, GPU and graphics driver version.
- The release version, shown in the launcher's **About** tab.
- What you were doing and what happened. For graphical problems, add a screenshot.
- The log. The launcher's **Play** tab shows the game's console output, and its diagnostics
  section can create a support bundle with the logs and settings.

If the game cannot start, the Play tab reports the operating system's error. Include that
message with your report as well.

## Project layout

```
launcher/            Launcher, installer and Modkit desktop UI (Python / Qt)
android/             ARM64 Android app, shared launcher bridge and touch gamepad
modkit/              Modkit C++/Lua SDK, stream tools, templates, tests and docs
simpsons/            The game project: recompiled code (generated/), hand-written hooks (src/)
simpsons/re/         Notes on the game's internals and native progress snapshots
tools/rexglue-sdk/   ReXGlue runtime: kernel, audio, input and graphics layers
tools/XenonRecomp/   PowerPC to C++ static recompiler
tools/extract-xiso/  Xbox ISO extraction, used by the launcher's installer
tools/bench/         Unattended test and benchmark tooling
tools/android/       Android native build and packaging tools
.github/workflows/   Build check on every push and pull request; manual release packaging
```

Not in the repository: game data, prebuilt toolchains (`tools/clang20`, `tools/rexglue-bin`) and
build output.

## Roadmap

The goal is for this to be the best way to play the game. Checked items are implemented in
the current tree; experimental and preview features keep the limits described above.

### Implemented

- [x] Native graphics resources across Vulkan, Direct3D 12 and experimental Direct3D 11:
  compact vertex/index inputs, retained GPU owners, supported texture uploads, resolves,
  surface aliases and original/scaled target transfers.
- [x] Bounded native graphics-call producers for the identified draw-emitter families,
  supported state/constants, shader commands, submissions, synchronization and presentation.
  Record per-system traffic with the progress script; retain unsupported contracts explicitly.
- [x] Higher/unlimited frame-rate choices, fixed-step physics, separate menu logic and video
  timing, with camera-input and keyframed-collision fixes for high frame rates.
- [x] Recomp settings, player assignment, achievements, debug tools and mod pages in the game's
  own menus, with mouse/touch interaction and per-player font-based button prompts.
- [x] Two-player free roam in Springfield and optional native cutscene-skip confirmation.
- [x] Free camera/photo mode, ink and eye controls, Vulkan character tessellation and colour
  grading, image scaling, sharpening and texture filtering.
- [x] Native Linux launcher, Steam Deck Gaming Mode handling, safer Steam shortcut creation,
  extractor provenance checks and a source-built Windows launcher bootloader.
- [x] Android ARM64 preview with ISO/folder installation, background progress, touch-layout
  editing, reversible patches, mod-package import, shader preparation and stereo dialogue.
- [x] Standalone desktop Modkit preview, public Lua/C++ operations, isolated Test profiles,
  native scene/mission tools, cross-level resource ownership, source navigation, bounded mesh
  editing and credited translation packages.
- [x] Original save slots retained; the experimental replacement save library stays shelved.

### Still in progress

- [ ] **Complete native API conversion.** Finish unconverted state/shader orchestration,
  resource preparation, special recording/growth/heap paths, queries and unproved ownership
  or alias cases. Keep [implementation contracts](simpsons/re/native_gpu_calls.md) separate
  from [recorded traffic](simpsons/re/native-progress/0.0.7.0/README.md).
- [ ] **Campaign and hardware coverage.** Qualify effects, co-op, repeated streaming,
  checkpoints and cutscenes across all renderers, native Windows drivers and Android GPUs.
- [ ] **Performance.** Reduce GPU work at 2x/3x and in two-player scenes, compilation hitches
  and long frame times. Extend measured high-FPS correctness across the campaign.
- [ ] **Full widescreen.** Complete render-target sizing, HUD, menus, post-processing and movie
  framing for wider aspect ratios. The camera/culling hook is only one part of this.
- [ ] **All controller artwork.** Replace button names baked into pictures and finish
  two-player HUD ownership checks beyond the implemented font-based prompts.
- [ ] **Android qualification.** Measure sustained Tensor and Snapdragon performance, audio,
  suspend/resume and controller/mod workflows. Add runtime support for 16 KB memory pages.
- [ ] **Character tools.** Finish real NPC possession/abilities and portraits, reviewed AI
  suspension, cross-rig animation mapping, and jointed Havok ragdolls with collision and recovery.
- [ ] **Full Modkit release gate.** Finish proven picking/snapping/gizmos, complete world and
  collision editing, broader spawn/AI behavior, new mesh/rig/animation import, materials/audio,
  campaign mission/HUD integration, cinematic tracks and creator/platform acceptance.
- [ ] **Translation presentation.** Add font import, complete Unicode coverage, right-to-left
  layout and acceptance languages across menus, gameplay and cutscenes.
- [ ] **Remaining game fixes and restored content.** Reproduce the open community reports on
  the affected hardware and bring back unused features where their behavior can be proved.

The [renderer plan](simpsons/re/native_renderer_plan.md),
[community bug tracker notes](simpsons/re/community_bug_fixes.md),
[Modkit plan](modkit/PLAN.md) and [Android guide](android/README.md) hold the detailed work.
The [0.0.7.0 preparation record](docs/release-0.0.7.0-prep.md) lists validation results
and the remaining package and device checks before publication.

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
- [(frankyfife)](https://github.com/frankyfife), for the free camera and photo mode,
  ink outline controls, clean eye shading, start episode selection, multilingual intro skipping,
  Windows timing and guest-address fixes, Havok step changes and audio/physics diagnostics.
  These contributions were developed in [his fork](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff)
  on 3–4 October 2026 and brought together in
  [#43](https://github.com/YesterMester/TheSimpsonsGameRecomp/pull/43). His later work there
  is also in: playing as other characters, bringing the players to the free camera, tutorial
  hints that wait for their text, fresh XMA decoder state for every sound, playing without
  sound when there is no audio device, and giving a new game the progress of the episodes before
  its start episode. The launcher's collapsible settings sections also
  come from his fork, including remembered open sections and Expand all / Collapse all.
- Everyone who has contributed code: Gabry179, awemancba, tronuo0 and anasalialamgir.
- The libraries the runtime is built with: SDL3, Dear ImGui, glslang, SPIRV-Tools, FFmpeg, spdlog,
  fmt and Tracy.
