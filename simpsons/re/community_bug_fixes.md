# Community bug fixes

Checked against GitHub on November 12, 2026 (UTC): 15 open issues. This tracks development changes against the published 0.0.6.5 reports. A successful
build or a different GPU is not enough to close a hardware-specific report.

| Report | Development work | Validation and remaining work |
|---|---|---|
| [#42: laptop performance](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/42) | The Windows game exports the NVIDIA and AMD high-performance GPU preferences. Release packaging checks both exports. | Both Windows variants compile. The report still needs laptop specifications, the selected adapter and a support bundle; there is no measured fix for that machine yet. |
| [#44: DualShock 4 stick behavior](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/44) | Camera cutoff scaling (`camera_input.cpp`), per-player input assignment and SDL input polling updates are in the tree. | The report describes intermittent stick delay and drift at 60 FPS on Linux Wayland without logs or a support bundle. Still needs device/transport details (Bluetooth vs USB, Steam Input state) and reproduction logs. |
| [#45: camera above 60 FPS](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/45) | Fixed the frame-dependent cutoff in both original camera input handlers. | The native check passes 196 cases, including both input ports, retained turn speeds and unchanged 30/60 FPS behavior. Vertical movement also works with an SDL virtual controller in the Linux game. Native Windows testing remains separate. |
| [#46: GTX 1080 rendering](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/46) | Settings migration 5 disables copy-free resolves and buffer write watches on existing installs. Other native resource paths stay enabled. | Migration was checked against the report's actual settings and the packaged Linux launcher. The Pascal-specific visual report still needs confirmation on that GPU. |
| [#49: marshmallow contact](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/49) | Bound hard-keyframe correction by the simulation step so animated collision bodies cannot outrun physics. | Normal approaches reproduced falling through. In a failing 120 FPS start, all seven marshmallow collision bodies were far above their visible models. The compiled Linux fix keeps the bodies in place and passes the first two consecutive launches at 165 FPS, using normal movement and jumps, with full health. The original-code check passes 242 cases. Windows x64, Windows without AVX2 and Android ARM64 compile; live testing on those platforms remains separate. |
| [#50: Cheatrix springboard](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/50) | The shared keyframe correction also covers springboard collision bodies. | The compiled Linux fix passes three consecutive drops onto the blue springboard at 164 FPS and reaches the upper platform each time. These tests position the player above the pad; they do not recreate the reported mission state after defeating the first NFL enemy. The original release could leave the player inside the pad. No speculative global physics change is included. |
| [#51: Groening boss controls](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/51) | Checked native controller input, character switching and movement at the later checkpoint. | Bart and Homer both move with SDL controller input at 60 FPS and with a 165 FPS cap in the current Linux build. The checkpoint position and native enemy-death events do not recreate the active Matt Groening fight shown in the report; that exact saved mission state and the Windows Xbox Elite path remain unqualified. |
| [#52: Defender detection](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/52) | Windows packaging builds the pinned PyInstaller bootloader from source without a prebuilt fallback. The extractor is the unchanged, hash-checked upstream executable with its required runtime. | Extractor installation fixtures pass. The reported whole-package `Trojan:Script/Wacatac.H!ml` detection still needs a Defender rescan of the new Windows package. These packaging changes do not prove antivirus clearance. |
| [#53: blank Windows launcher](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/53) | Diagnosed from the reporter's log (`serving http://127.0.0.1:8712/`, `server answers`) on Windows 10 with Intel Celeron J1800 / Bay Trail Intel HD Graphics. | The local HTTP UI loads and accepts cursor hit-testing while Qt WebEngine's GPU compositor renders a blank surface on that legacy iGPU. Opening the logged localhost URL in a browser or running with `--no-browser` / software WebEngine rendering works around the blank window; automatic software-compositing fallback on legacy Windows iGPUs remains for a later patch. |
| [#54: Smart App Control WinError 4551](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/54) | Windows packaging stages the unchanged, hash-verified upstream `XboxDev/extract-xiso` Win64 release binary with its separate `VCRUNTIME140.dll` and `extract-xiso-source.json` instead of an ad-hoc unsigned CI build. | Extractor staging and verification pass. The upstream binary is still not Authenticode-signed, so the launcher now falls back to its own reader (`launcher/xiso_extract.py`, ported from the Android app's XDVDFS reader) when `extract-xiso` is missing or cannot start (any `OSError`, such as WinError 4551). `tools/bench/xiso_extract_check.py` covers XGD2/XGD3 partition offsets, damaged and unsafe images, and the install fallback with a blocked or missing extractor, on Linux and under Windows CPython in Wine; a real ISO extracts 7,968 files identical to the installed game data. Not tested on Windows with Smart App Control, which may also refuse the other unsigned programs. |
| [#55: Big Super Happy Fun Fun Game exit](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/55) | No episode-specific change yet; 0.0.7.0 includes the settings migration 5 renderer defaults, resource-bounds fixes and audio shutdown fixes. | Reported on the ice section at 2K 60 FPS without platform, renderer or logs. Needs a support bundle or crash log from the reporter to reproduce and diagnose. |
| [#19: Android version](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/19) | ARM64 development APKs, touch controls, installation, mod-package import and shader preparation are implemented. | Keep open until a supported APK is published. Current test APKs use development signing; sustained Tensor/Snapdragon performance and device lifecycle checks remain. |
| [#35: PS4 port](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/35) | A separate platform request. | Deferred beyond this desktop/Android release. Other projects' console ports do not qualify this game. |
| [#40: ultrawide](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/40) | Camera/projection research exists. | Keep open: full render-target, HUD, menu, post-processing and movie framing support remains unfinished. The supported picture is still 16:9. |
| [#41: in-game PC settings](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/41) | Recomp settings, input assignment, keyboard options and exit controls are integrated into the game's menus. | The main feature is implemented. Review every requested control and full input-remapping workflow against the packaged build before closing the broader proposal. |

## Camera cutoff

`sub_8295C9C0` and `sub_82960870` multiply stick input by the camera's frame
factor at +72 and its turn speed, then compare the result with a fixed 0.02
cutoff. With the usual 0.03 vertical speed, a full stick reaches the cutoff
at 90 FPS and falls below it on shorter frames.

`camera_input.cpp` scales the cutoff by the same positive frame factor on
frames shorter than a 60 Hz frame. It calls the original handler and restores
the exact tuning word afterwards, including on an exception. The original
input readers, turn speeds and camera limits remain in use. Both public hooks
have `REX_EXTERN` declarations so they replace the generated C aliases.

Run the check on Linux without launching the game:

```sh
python3 tools/bench/camera_input_check.py --output /tmp/camera-input-check
```

The check compiles the production hooks with the actual generated handlers
and register-save helpers. It covers 30/60 FPS and frame factors through
1000 FPS, both ports, partial input, drift, register restoration and an
interrupted input callback. Windows x64, Windows without AVX2 and Android
ARM64 compilation are checked separately.

## Keyframed collision bodies

The original hard-keyframe helper, `sub_82A93FE0`, converts the difference
between a body's current and requested pose into velocity using the render
frame's inverse duration. At 120 or 165 FPS, that correction can run faster
than the world's 1/59.94 s physics step. Repeated overshoots send a collision
body away while its visible model stays in place. Lowering the frame cap
afterwards does not immediately put that body back.

`physics_keyframes.cpp` bounds this inverse duration by the simulation step
and calls the original helper. `physics_step.cpp` supplies the mode's step
and retains a longer observed step together with its world, so a slow frame
does not use a correction from another world. Detached bodies retain their
caller's timing. The correction keeps the original centre of mass, rotation,
motion setters and activation requests.

The native check runs the unchanged generated helper and normalizer, with
captured motion setters and a fixed-step feedback loop. It reproduces the
original divergence, checks stationary and moving targets, rotations,
detached bodies, invalid timing and register restoration. It passes 242
checks through 1000 FPS. Its `acos` callback uses the host math library;
the check validates timing and motion rather than console math precision.

The compiled Linux build was also checked from a fresh start of the original
Land of Chocolate episode flow. Normal movement and double jumps trigger the
first and second marshmallow launches, reach both landing islands and retain
100 health at a measured 165 FPS. This test uses the production hook without
a diagnostic timing override. All seven retained marshmallow bodies stay in
the level, including their normal compression animation after a launch.

In Enter the Cheatrix, three repeated drops onto the blue springboard also
reach its upper platform at a measured 164 FPS. The player is positioned above
the pad for this targeted collision test. It is separate from progressing
through the mission and defeating the first NFL enemy.

```sh
python3 tools/bench/physics_keyframes_check.py --output /tmp/physics-keyframes-check
```

## Other bugs found during investigation

- Typing into the SDK settings or console no longer runs registered game
  shortcuts. Previously an underscore could also press the Minus photo-mode
  shortcut and freeze the world while searching for a setting.
- ImGui receives queued modifier events, including both Windows keys, so
  Ctrl+A works in settings text fields.
- The debug menu and Modkit use the game's stored character order:
  Homer, Bart, Marge, Lisa. Lisa and Marge were labelled the wrong way round.

Keep `native_resolve_copy_free` and `native_buffer_write_watches` false in
normal builds. The versioned migration preserves video and audio choices;
later deliberate developer overrides are not repeatedly reset.

## 0.0.7.0 closure review

No issue was closed or changed by this review. #45 and #49 are the strongest
candidates after the tested fixes are published and reporters retest. #50 has a
targeted fix but still needs the exact mission sequence. Keep #42, #44, #46,
#51–#55 open for the remaining hardware, scenario or packaging checks above.
#19 and #41 need published-package acceptance; #35 and #40 remain future work.
The maintainer closed [#56](https://github.com/YesterMester/TheSimpsonsGameRecomp/issues/56)
on November 12 at 02:34 UTC because QuiverLauncher is maintained separately.
That closure does not establish a game or launcher fix; no reopening or other
issue mutation is part of this review.
The new comment on #55 reports `Wacatac.B!ml`; it adds an antivirus report,
not a crash log that explains the ice-section exit. Treat it alongside #52.
