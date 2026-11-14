# 0.0.7.0 release preparation

Local preparation checked November 12–13, 2026 (UTC). The sections after
[Publication](#publication) record that state, before anything was committed.
No issue closure was performed.

## Publication

v0.0.7.0 was published on November 14, 2026 (UTC) from commit `e7c93150`.
Later commits change only documentation.

- The manual release workflow built the Linux x64, Linux NoAVX2, Windows x64
  and Windows NoAVX2 packages. The first hosted Windows builds needed four
  fixes, all in the tagged commit: the include order of the D3D12 render-target
  cache, a JSON comparison in the Modkit runtime, the native GPU hook check (a
  Windows executable has no symbol table, so the check reads the linker map)
  and resource linking for the Modkit entry program.
- On the Steam Deck, the workflow's Linux package loaded Bartman Begins, moved
  the player and drew the debug menu. Its Windows package loaded a level and
  moved the player under Proton.
- Real updates from the published 0.0.6.5 Linux and Windows packages (Windows
  under Proton) kept the player's settings and backups, and on Linux an
  installed mod. Afterwards every file matched the 0.0.7.0 package except the
  top-level `LICENSE`, which neither updater installs.
- The Android APK attached to the release was built locally from the release
  sources. It is the debug variant, signed with the development certificate
  (`37f9620d…2351`) that earlier test builds and the in-app updater use, so it
  is debuggable. `check_apk.py` passes: certificate, five native libraries
  matching staging, 72 shaders and ELF/ZIP alignment. It was not run on a
  phone in these checks. The owner release key and a hosted Android release
  build remain open.

## Release material

- [Release notes](../.github/release-notes.md) cover the new game menus, input,
  co-op, cutscene skipping, Modkit, translations, renderer, launcher updates,
  Android preview and known limitations.
- The [README](../README.md) describes the implemented features, installation,
  settings and platform requirements. Its roadmap keeps unfinished native API,
  widescreen, hardware, character-tool and editor work open.
- The [native GPU snapshot](../simpsons/re/native-progress/0.0.7.0/README.md)
  reproduces with identical counters and provenance. Its percentages describe
  recorded traffic, with no overall game-completion percentage.
- The [community review](../simpsons/re/community_bug_fixes.md) covers all
  15 currently open reports and records the maintainer's separate closure of
  the QuiverLauncher report. No issue should be closed solely because a
  development change compiles.

## Local validation

| Check | Result |
|---|---|
| Launcher patches and saved settings | 21 tests pass, including the start-episode progress chain and files written by an older launcher |
| Updates from older launchers and bundled mods | 8 tests pass |
| Installing without `extract-xiso` (#54) | 7 tests pass on Linux and under Windows CPython: XGD2/XGD3 partition offsets, damaged and unsafe images, and installs with a blocked (WinError 4551) or missing extractor. A real ISO extracts 7,968 files, identical to the installed game data |
| Start-episode progress (from frankyfife's fork) | On an overlay copy of the game data, a new game started in a later episode marks the earlier episodes complete and unlocks their completion achievements, as the game's own episode cheat does. Game Hub is left without progress because its start event is shared with Springfield |
| Windows update repair (simulated) | Source launcher with a mocked Windows platform restores the four files an older updater leaves out from a release-layout archive; settings and a player mod survive |
| Steam shortcuts | 4 tests pass |
| Modkit packages, projects, translations and editor services | 128 tests run: 127 pass and one skip with Qt; 123 pass and five skips without Qt |
| Native progress accounting | 19 tests pass; both published summaries reproduce |
| Shipped translated shaders | Translator fingerprint matches |
| Native camera and keyframed collision handlers | 196 and 242 checks pass |
| Animation catalog against original lookup code | 70,794 assertions; 16,833 native lookups pass |
| Animation browser compilation | Windows x64, NoAVX2 and Android ARM64 pass |
| Game and Modkit source syntax | 274 checks pass across Windows x64, Windows NoAVX2 and Android ARM64; 91 additional Windows x64 checks pass with libc++ transitive includes disabled. The only later C++ change, `debug_menu.cpp`, passes again on all three targets and with strict includes |
| Platform macro guards (`-Wundef`) | 556 syntax-only compiles exit 0: every hand-written Linux build unit plus game and Modkit sources for Windows x64 and Android ARM64. Of 33 undefined-macro sites, only `cvar.cpp:29` and `:692` are defects (deferred below); the rest are intentionally conditional. The later `debug_menu.cpp` change adds no site |
| Private Linux/Vulkan game | 324 Bart and 194 Homer clips match original lookup; six authored clip playbacks succeed; passive ragdoll restores control and walking; exit 0 with no error logs |
| Complete current Linux source build | Game, SDK and Modkit asset tool build successfully on the Steam Deck |
| Current Linux executable's native GPU hooks | All 46 hooks replace their generated aliases |
| Packaged Linux game | Final local package: Bartman Begins loads; input moves Bart 2.65 m; the debug menu opens and draws over play (engine capture); clean exit 0 with no error-level logs. Its 55 repository files match the final tree, the game executable is byte-identical to a fresh relink, and the frozen launcher bundles the new `xiso_extract` module |
| Steam Deck frame rates | Steam Deck LCD, final package, default settings except `frame_rate` and `menu_frame_rate` 0, `vsync` off, a 1280×720 window in Desktop Mode, and the harness's `perf_log_csv`, `native_telemetry = 120`, input injection and a screenshot after every 25 s idle (audio to a silent sink): the Simpsons' house in Springfield runs at 132, 103 and 53 FPS at 1×, 2× and 3× (1% lows 100, 66 and 44); Neverquest's opening dragon attack at 2× runs at 110 FPS (1% low 56). Medians of three 40-second samples (four for Neverquest, whose third run measured a dialogue scene and was replaced) on an otherwise idle machine. Measured before the front-end state fix, which changes only UI and input conditions |
| In-game screens after a New Game | Fixed: in a level started with New Game the front-end state is 7, not the 9 the code expected, so the debug menu, achievements screen, pause-menu X/Y and RB+Left shortcuts, the Options → Extras rows and the gameplay paths of button prompts and mouse/touch treated play as a menu and drew nothing. They now accept 7 or 9. After a New Game through the front end and in a quick-launched level, the debug menu draws in play and the pause menu's X opens it; title and main menu are unchanged. The four changed files pass the Windows x64, NoAVX2, Android and strict-include checks with no new `-Wundef` site |
| Native knockdowns (attempted, not shipped) | Accepted hits on Homer or Bart started the native rig, and pause, exact-save refusal, control return and the mod switch all behaved. But the game thread froze when the AI partner touched the falling body (reproduced; twelve rounds without the rig did not freeze; it still froze with the player's collision kept and with the rig not driving the skeleton, so the rig's bodies are the cause), so the attempt was reverted. The debug menu's ragdoll test uses the same rig and now carries a known-issue note |
| Packaged Linux shutdown watchdog | With debugger-induced shutdown stalls, exits after 10.36 s with the UI thread held and 10.38 s with UI and log-lock threads held; older build stays alive past 40 s in the second case |
| Packaged desktop applications | Frozen launcher and separate Modkit open their native windows and exit 0 with isolated data |
| Desktop editor and desktop/Android launcher modes | Isolated Qt checks pass; editor and launcher also pass with the packaging pin, Qt 6.8.3 |
| Android development APK (earlier tree) | Superseded: `launcher.py`, `xiso_extract.py` and `debug_menu.cpp` changed after it was built; rebuild it from the committed tree. When built: full ARM64 build; five packaged libraries match staging; signature, dependency, shader and alignment checks pass; development certificate matches earlier APKs |
| Linux archive | 7,577 files round-trip with identical hashes; required runtime files and license included; no game data or local profiles |
| Android release-signing rejection | Missing credentials, normal debug-keystore reuse, debug-signed release APKs and certificate mismatch are rejected |
| Android release-signing integration | Release variant builds with a disposable fixture key; exact certificate, non-debuggable manifest, native libraries, shaders and alignment pass; fixture key and APK removed |
| Android inputs from an exported source tree | 2,295 unique native repository inputs accounted for; all 82 staged assets match the delivered APK, including 72 indexed shaders |
| Workflow structure | YAML, job dependencies, script paths and Bash syntax pass |
| Windows CI Python steps | Under Windows CPython 3.13.17 (official embeddable package, run through Wine/Proton) on the final tree: the launcher check runs 21 tests, 19 pass and two Linux-only tests skip; the ISO extraction check passes 7 of 7. Extractor staging, extraction check, runtime defaults and package verification passed on an earlier tree and their scripts are unchanged |

These checks cover the named sources, local Linux package, Android development
package and recorded samples. The new development APK includes the animation
catalog fix. The Linux package was built and exercised on the Steam Deck;
Windows, NoAVX2 and other Linux distributions still need their release builds
and acceptance runs. No phone execution is claimed.
The Windows syntax checks use LLVM-MinGW/libc++, and Android checks use NDK r29.
Disabling transitive includes checks include hygiene; it does not establish an
MSVC compiler or MSVC STL build, package linkage or Windows execution.
The watchdog check covers the exact local Linux package. It verifies forced exit
under simulated stalls, which skips remaining cleanup; the original intermittent
hang's cause and the Windows runtime path remain unverified.
The exported-tree Android check verifies input availability and asset preparation;
it does not rebuild the native libraries or establish a hosted workflow result.
The Windows Python steps ran under Wine on the Steam Deck. They found two
Linux-only launcher tests that failed on Windows; those tests now skip there.
This is not a run on Windows itself or of the frozen Windows launcher.

## Deferred follow-ups

- `tools/rexglue-sdk/src/core/cvar.cpp` tests `REX_PLATFORM_WIN32` without
  including `rex/platform.h`, so Windows saves in-game settings with
  `std::filesystem::rename` instead of `MoveFileExW` with write-through. Settings
  still save. Fix after 0.0.7.0 by including `rex/platform.h` before line 29,
  then check the Windows build with that code active.
- `simpsons/src/frame_pacing.cpp` uses weak references that do not import their
  targets on Windows, so the opt-in `frame_pacing_vblank_lock` and its trace event
  do nothing there. This is unchanged from 0.0.6.5.

## Automated release gates

The manual release workflow runs updater, settings, Steam shortcut, Modkit
package and progress checks before its desktop builds. The Linux builds also
run the original-code animation, camera and collision checks. Both desktop
builds check native GPU hook linkage before packaging. Shader fingerprint,
required files, Windows extractor provenance and Linux shared-library checks
remain part of packaging.

The separate manual Android preview workflow builds an APK artifact using the
owner's keystore and checks its expected public certificate fingerprint, native
dependencies, ELF/ZIP alignment and shader hashes. It does not publish a release.
Signing setup is documented in [the Android guide](../android/README.md#signed-preview-packaging).
The owner release key has not been configured locally; the checked local APK
uses the existing development certificate. A separate local release-variant
build passed with a disposable test key. That verifies the signing path without
establishing owner-key configuration or a successful hosted workflow run.

## Before publication

1. Review and commit the intended source files, including the new modules and
   bundled example package. Preserve deliberately staged work and exclude
   private test output, installed runtimes and game data.
2. Run the manual release workflow on that reviewed commit, then test the
   resulting Windows, NoAVX2 and Linux packages, including an update from
   0.0.6.5. Local workflow checks do not establish a successful hosted build.
3. Confirm Windows-specific graphics and input behavior on Windows drivers.
   Defender and Smart App Control reports remain open: verified provenance and
   a rebuilt launcher bootloader do not establish signing or antivirus clearance.
4. Configure the owner Android signing key and run the separate preview workflow
   on the reviewed source. The checked development APK is available locally;
   the desktop release workflow does not attach it. Phone acceptance, sustained
   Tensor/Snapdragon performance and suspend/resume remain open. The current
   runtime rejects 16 KB host pages; alignment checks do not establish runtime
   support. That requires changes to guest-memory protection and aliases before
   hardware acceptance. No ADB device was attached during these checks.
5. Publish only after review. Recheck the affected issues with the delivered
   packages and reporter hardware before closing them; #54's fallback has not
   been tried on Windows with Smart App Control. Native knockdowns (and the
   freeze they showed), automatic ragdoll get-up, NPC possession and the wider
   roadmap remain future work.
