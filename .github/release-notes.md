You need your own copy of The Simpsons Game for Xbox 360. No game content is included; the
launcher installs the game from your own ISO.

Discord: https://discord.gg/Heag4zPJbx

### Fixes

- **Settings apply again after updating to 0.0.7.0** ([#61](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/61)). The 0.0.7.0 launcher wrote one setting, `native_gpu_calls`, into `simpsons.toml` twice every time it started, on Windows and Linux. The game ignores a settings file with a repeated entry, so it started windowed and in the default language, and changes made in the game could not be saved.
  - The launcher now writes each setting once, and repairs an affected file the first time it starts.
  - The game itself reads a file with a repeated entry instead of ignoring it.
  - The game's log now says when the settings file could not be read, instead of "Loaded config".
  - Thanks to Gabry179, who found the same cause and proposed the launcher fix in [#60](https://github.com/YesterMester/TheSimpsonsGameRecomp/pull/60).
- **Windows: settings changed in the game are saved safely.** The Windows build saved them with the plain rename used on Linux; it now replaces the file with Windows' write-through move, so a crash mid-save cannot leave it half written.
- **Windows: `frame_pacing_vblank_lock` works.** The option had no effect in Windows builds.

### Performance

The renderer's native graphics calls, and the way the recompiled code handles the game's interrupt switches, are faster. Both changes run the same way on Windows, Linux and Android.

- **Less work per draw.** The native graphics calls no longer clear several kilobytes of planning data for every draw, and they check guest memory ranges with a table lookup.
- **No fences where the console had none.** The game turns interrupts off and on around every graphics resource lock. The recompiled code wrapped each of those in two full memory fences and took a global lock just to read the interrupt state. Those fences are gone; the game's own memory barriers are unchanged.
- **Measured** on a Steam Deck LCD at 2× resolution in Neverquest's village fight, the busiest scene we measured (about 3,700 draws a frame): the game thread needs **about 27% less time** in its heaviest frames, 9.0 ms instead of 12.4 ms. Behaviour and image are unchanged.

### Android

- **Built and checked by the release itself.** The APK is now a release build, no longer a debuggable one, made by the same workflow as the desktop packages. It is signed with the same key as 0.0.7.0, so the app offers this update by itself.
- **Whole-program optimisation (ThinLTO)**, as the desktop builds already use.
- **To install:** download `TheSimpsonsGame-Recompiled-Android-arm64.apk` below on the phone and open it, allowing installs from your browser or file manager when Android asks.

### Known issues

- **Steam Deck at 2×:** in the heaviest moments, such as Neverquest's dragon fight, the GPU runs at its limit even at 60 FPS, and a few frames (about 0.5% there) take longer than 1/60 s. 1× is the smoothest setting on the Deck. On its 60 Hz screen, a frame rate above 60 renders frames that are never shown, so 60 is the best cap there.
- Above 30 FPS some scripted moments can still misbehave; switch to 30 if one does.
- The Windows programs are not code-signed. Defender or Smart App Control can warn about them or block them ([#52](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/52), [#54](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/54)).
- Wide screens show the original 16:9 picture; full widescreen is still being worked on.
- The debug menu's ragdoll test can freeze the game when the other family member touches the fallen body.
- Android is a preview and has only been tried on a Pixel 9 Pro.

### Thanks

- **Gabry179** for tracking down the settings bug and proposing the fix in [#60](https://github.com/YesterMester/TheSimpsonsGameRecomp/pull/60), and **Diego4C** for reporting [#61](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/61).
- Everyone else who made this possible is in the [credits](https://github.com/YesterMester/TheSimpsonsGameRecomp#credits).
