You need your own copy of The Simpsons Game for Xbox 360. No game content is included; the
launcher installs the game from your own ISO.

Discord: https://discord.gg/Heag4zPJbx

### Highlights

- **Settings, players and button prompts inside the game.** The game's own Options menu now has the recomp's settings, a Players screen for assigning controllers or keyboard and mouse, and prompts that match the controller each player holds.
- **Two players in Springfield.** A second player can join free roam in split screen.
- **Skip cutscenes**, including on a first viewing.
- **Mods, a Modkit and translation packs.** The launcher installs mod packages, and Windows and Linux downloads include a separate Modkit preview for making them.
- **Android preview.** An ARM64 build with a touch gamepad you can lay out yourself.
- **The renderer converts the game's graphics calls directly** by default on Vulkan, Direct3D 12 and Direct3D 11.
- **Fixes for play above 60 FPS**: the camera, the marshmallow platforms and the Cheatrix springboard.

### Updating from 0.0.6.5

- Update from the launcher's About tab as usual. The 0.0.6.5 launcher only knows the files that version had, so after it finishes and you restart, the new launcher asks once more to **finish the update**. That second step installs what 0.0.7.0 added (the mod runtime's description, the Modkit and, on Linux, the launcher's own window). In Steam Deck Game Mode it runs by itself on the first start and takes a minute or two.
- Your saves, settings, installed game data and mods are not touched. A fresh download needs none of this.

### In the game's own menus

- **Options → Recomp settings** has graphics, display, sound and keyboard settings on the game's TV screen, in the main menu and the pause menu. Most change while you play, including frame rates, FXAA, texture filtering, image scaling, sharpness, fullscreen, sound, subtitles and controls. The renderer, render resolution, V-sync, audio buffer and the character, outline, eye and colour options need a restart and are marked.
- **Options → Players** gives each player a controller or the keyboard and mouse, by choosing from a list or pressing a button on the device. Moving a device to the other player swaps them. A keyboard and a controller can share a two-player game.
- **Button prompts** follow each player's controller: Xbox, PlayStation, Nintendo, Steam Deck or keyboard, or pick one yourself. This covers menus, hints and the ability icons in the HUD, separately for both players in split screen. Button names drawn into pictures keep their original look.
- **Mouse and touch** work in the menus: click or tap a row, use the wheel or the sides of a slider, swipe through episodes, movies and clichés, and tap the map.
- **Achievements and the debug menu** are drawn in the game instead of over it (*Options → Extras*, Y and X in the pause menu, or F5 and F2). All 44 Xbox 360 achievements are there; their names, descriptions and pictures are read from your own game files, and loading an older save unlocks what it had already earned.
- The debug menu's key can be changed in the launcher (*In-game shortcut keys*).

### Playing

- **Two players in Springfield.** Assign a second input in *Players* and press Start on it to join free roam; *Drop Out* in the pause menu returns to one player. Episodes and time trials keep the player counts they were made with. On by default (`freeroam_coop`).
- **Skip cutscenes.** During a story video, press B to pause it and confirm the skip; any other button shows a hint in the corner first. It follows your controller's prompts and works with a click or tap too. On by default; the switch is in the launcher's Patches tab and in *Options → Extras*.
- **Camera above 60 FPS** ([#45](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/45)). The right stick no longer loses vertical and partial input on short frames. 30 and 60 FPS behave as before.
- **Platforms above 60 FPS** ([#49](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/49), [#50](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/50)). The collision of animated objects could drift away from what you see, which let you fall through the marshmallows in *The Land of Chocolate* and stopped the blue springboard in *Enter the Cheatrix* from launching you. Their correction is now limited to one physics step.
- Level loading decompresses faster: the same 175 MiB of boot and checkpoint data takes 342 ms instead of 595 ms on a Steam Deck.
- Closing the game can no longer be held up by its audio queue.
- The original four save slots and checkpoints are unchanged. The save library that was tried during development is not part of this release.

### Debug menu

Spawning, Spooner, animations, the NPC costume preview and the ragdoll test need *Cheats* switched on at the top of the menu. Browsing the library and setting up quick launch work without it.

- A **game-wide library** lists the characters, props and effects of the whole game by category and spawns the ones the current level can load. Something it cannot place is refused instead of being replaced by a different object.
- **Animations & poses** lists the clips in your character's loaded animation bank by their authored names and checks ownership before playing them. This fixes the empty animation lists for Bart and Homer.
- **Spooner** selects loaded objects from the free camera and moves, turns, resizes, copies or hides them. The level's own scripts keep running; this is not a level editor yet.
- **NPC costume preview** shows the character you control as an NPC. It is a costume: the family member's abilities and HUD stay.
- **Ragdolls & knockdowns → Test player 1 ragdoll** (or player 2) is a first look at real ragdolls built on the game's Havok physics: 13 bodies and 12 joints drive the character's own skeleton for two seconds, then hand control back. NPC ragdolls, hit reactions and getting up are not done, and it can freeze the game (see Known issues).
- **Quick launch level (dev)** boots straight into a chosen level for testing.

### Mods, Modkit and translations

- The launcher's **Mods** tab installs mod packages, orders them and switches them on and off. Each set of enabled mods keeps its own save progress, so a mod cannot damage your normal saves.
- An example mod, *Experimental Knockdowns*, is installed switched off. It is a simple whole-body knockback written to show how a mod asks the game for something and gives it back; it is not the ragdoll physics above.
- Windows and Linux downloads include the **Modkit** (`Modkit.sh`, `simpsons-modkit.exe`), a separate application for making mods. It is a preview. It creates projects from commented Lua and C++ templates, edits and reloads code while the game runs, tests in a separate profile with the game shown inside the editor, browses the game's resources with spinning model previews, places and moves props, builds route missions, and exports and re-imports small model edits.
- **Translation packs** can be made without writing code: export the text, edit it with checks for placeholders and button tags, and package it with your name on it. Lines you have not translated keep the original text.
- What the Modkit cannot do yet is listed in its [plan](https://github.com/YesterMester/TheSimpsonsGameRecomp/blob/main/modkit/PLAN.md).

### Android (development preview)

- **To install:** download `TheSimpsonsGame-Recompiled-Android-arm64.apk` below on the phone and open it, allowing installs from your browser or file manager when Android asks. It is a development-signed preview build; later releases can update it from inside the app.
- An ARM64 build for Android 10 and later with Vulkan 1.1. It uses the same launcher and installs the game on the phone from your ISO or an extracted folder, in the background with a progress notification.
- A multitouch gamepad with an editor: move, resize, fade or hide each control, and set the stick deadzone and camera sensitivity. It hides while a menu is open so you can tap the menu.
- Shaders and pipelines are prepared before the first frame and kept for later starts. Dialogue is mixed into stereo so it is audible on phone speakers.
- Patches, mod packages and language packs can be imported. Making mods stays on the desktop.
- The app offers newer releases itself when one carries an APK signed with the same key.
- This is a preview: expect stutters and audio skips until it has been measured on more phones. It needs a kernel with 4 KB memory pages.

### Renderer

- **Native graphics calls** (`native_gpu_calls`) are on by default and can be switched in the launcher or in *Recomp settings → Graphics*. Instead of interpreting the console's command buffer, the game's draw, state, shader, synchronisation and presentation calls are converted where they are made. Anything not converted yet takes the previous path.
- Vertex and index buffers, texture uploads, resolves of colour, depth and multisampled targets, and copies between original-size and scaled targets are native on Vulkan, Direct3D 12 and Direct3D 11.
- The shipped shader set has 6,660 translated variants, vertex and pixel shaders are cached separately, and driver pipeline caches are kept between runs. The four hand-written full-screen passes now also come in 3x versions.
- Measured on a Steam Deck (Vulkan, start-up and title screen, 1,079 frames, 3,367,526 draws):

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

  This counts the work in that sample, not the whole game. The [numbers behind it](https://github.com/YesterMester/TheSimpsonsGameRecomp/blob/main/simpsons/re/native-progress/0.0.7.0/README.md) are in the repository.
- Existing installs get copy-free resolves and buffer write watches switched off once, which fixes the broken textures some GPUs showed ([#46](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/46)).

### Steam Deck frame rates

Measured on a Steam Deck LCD with this release's Linux package at its default settings, except the frame rate (also in menus) unlocked and V-sync off, in a 1280×720 window in Desktop Mode, with the test tools' per-frame log and GPU telemetry on. Each row is the median of three 40-second samples (four for Neverquest) after a warm-up run; the 1% low is the frame rate at the 99th-percentile frame time. In play, levels are capped at 60 FPS by default.

| Scene | Render resolution | Average | 1% low |
|---|---|---|---|
| Springfield, the Simpsons' house (start of the hub) | 1× | 132 FPS | 100 FPS |
| Springfield, the Simpsons' house | 2× | 103 FPS | 66 FPS |
| Springfield, the Simpsons' house | 3× | 53 FPS | 44 FPS |
| Neverquest, the dragon attack at the start | 2× | 110 FPS | 56 FPS |

These are two scenes; performance varies through the game. 1× is the recommended render resolution on the Steam Deck.

### Launcher, Steam Deck and Windows

- **Linux downloads carry their own launcher window.** `Play.sh` no longer depends on the system's Python or Qt, opens full screen in Steam Deck Game Mode, and works under Steam Linux Runtime 1.0 and 2.
- **Add to Steam** recognises a shortcut that already exists instead of adding another, copes with spaces in the path, sets the donut icon and leaves Steam running.
- Settings sections collapse and remember what you opened.
- **Windows laptops** with two GPUs: the game asks for the fast one before the renderer starts ([#42](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/42)).
- **Start episode** now also gives the new game the progress of the episodes before the one you choose: as that episode starts, the game's own episode cheat marks every earlier episode complete, so Springfield is unlocked, for example. As with the cheat, those episodes' completion achievements unlock. Starting in Game Hub adds no progress. While the patch is on this also happens when an existing save starts that episode, which only marks episodes it has already finished and never replaces a real best time. From frankyfife's fork.
- **Installing when the ISO extractor cannot run** ([#54](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/54)). If `extract-xiso` is missing or Windows refuses to start it, the launcher now reads the disc image itself and installs from it. A damaged or unexpected image is refused without touching the game data you already have.
- **Windows ISO extractor**: packages carry the unchanged upstream `extract-xiso` build, checked against its published hash, with a record of where it came from. The launcher's start-up code is compiled during the release build instead of using PyInstaller's prebuilt one ([#52](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/52)).
- Typing in the settings window or the console no longer triggers game shortcuts.

### Known issues

- Above 30 FPS some scripted moments can still misbehave; switch to 30 if one does. The whole campaign has not been played at the higher rates.
- Windows has been tested with the Windows build through Proton on a Steam Deck, not yet on Windows graphics drivers. If something renders wrong, switch `native_gpu_calls` off in the launcher and report it.
- The Windows programs are not code-signed. Defender or Smart App Control can warn about them or block them ([#52](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/52), [#54](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/54)). Installing no longer depends on the ISO extractor being allowed to start.
- On very old integrated graphics the Windows launcher window can stay blank ([#53](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/53)); the address printed in `launcher.log` opens the same launcher in a browser.
- Wide screens show the original 16:9 picture; full widescreen is still being worked on.
- Character tessellation (Vulkan) and the Direct3D 11 renderer (Windows) are experimental.
- The debug menu's ragdoll test can freeze the game: the same physics froze it when the other family member touched the fallen body. Keep them apart, or leave the test alone.
- Android is a preview and has only been tried on a Pixel 9 Pro.

### Thanks

- **frankyfife**: this release brings in more work from [his fork](https://github.com/frankyfife/TheSimpsonsGameRecomp_ff): playing as other characters (*Player → Simpsons family & costumes* in the debug menu, with cheats on), bringing the family to the free camera (B or F9, with cheats on), tutorial hints that wait for their text, sounds that no longer start with a fragment of the previous one, playing without sound when there is no audio device, the collapsible launcher settings, and the start-episode progress above.
- Everyone else who made this possible is in the [credits](https://github.com/YesterMester/TheSimpsonsGameRecomp#credits).
