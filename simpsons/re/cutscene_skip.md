# Cutscene skipping

`src/cutscene_skip.cpp` implements `cutscene_skip` (default on). The launcher exposes it in
Patches; the native Extras menu can change it in the running game. Turning it off restores the
original movie controls. `cutscene_skip_log` enables transition diagnostics.

During a story video, a fresh non-B button shows the bottom-right hint for four seconds. B
opens the question and pauses the decoder; a separate A confirms, and B cancels. Held input
from loading or from opening the question cannot confirm it. Cancel wins if A and B arrive
together. Each assigned player can operate the question; its glyphs follow the player who
last pressed a button, the selected controller style, or the actual keyboard bindings.

Mouse and touch taps use normalized guest-image coordinates, including letterboxing. The
hint is clickable, as are both choices. A drag or a release on a different choice does not
confirm. Input is handled by the movie controls and does not open a host UI screen.

## Movie lifecycle

Story videos are VP6 streams declared by `NewMovie` in `simpsons_gameflow.lua`, decoded by the
recompiled game. The existing boot-logo patch is independent; EA, Fox and Gracie logos retain
their original controls because they run before normal UI fonts are available.

| Guest address | Role |
| --- | --- |
| `826B8B70` | Accept a movie descriptor; identifies the player and stream |
| `82D09750` | Movie-player singleton; state is at player +20 |
| `826B8D28` | Pause playback: state 3 to 6 |
| `826B8D68` | Resume playback: state 6 to 3 |
| `828A8968` | Movie-manager input messages; replace their seen-movie-only skipping |
| `8239A7A0` | Movie-browser controls; suppress its immediate B shortcut while active |
| `8238F130` | Emit a native event; descriptor `82D61DF0` requests movie stop |
| `826B9708` | Stop-event listener: calls stop/cleanup, then the normal finish |
| `826B9290` | Stop subtitles, release the decoder and resource, enter state 4 |
| `826B8AD8` | Finish, enter state 0 and emit MovieHasEnded through the original flow |
| `826BBF88` | Flush a user's old guest input before handing control to the next scene |

A confirmed skip sends the original stop event. It does not return early out of the decoder,
change the clock, or write a save's seen-movie flags itself. The original finish callbacks advance the
story and own audio/subtitle/resource cleanup.

## Native drawing

Videos unload the front-end popup screens, so their popup singleton cannot supply this UI.
The movie's YUV presenter (`8282EC58`, reached through `82373518` / `82374FA0`) runs before the
native HUD is prepared. Drawing font quads there produces no visible UI.

The working drawing point is the end of the native HUD pass, before `826D4840` restores its
camera and render states. `game_ui.cpp` calls `DrawCutsceneSkip` there once per game frame,
including when `FrontEndShows` is true for a story video. `canvas::Canvas` submits the game's
font batch and preserves its buffer and render states. The title/body fonts and controller
or keyboard glyphs are shared with the other native menus. No ImGui, WebView or host-window
popup is involved. Playback keeps its original rate.

## Qualification

The private Linux/Vulkan test used the actual first story video, `movies/en/loc_igc01`, from a
fresh disposable profile. It verified the visible hint, PlayStation circle/cross prompts,
rebound keyboard Z/Enter prompts, B pause/cancel, mouse cancel and hint activation, and an
Enter-confirmed skip through the original finish event into playable Land of Chocolate.
Holding B for two seconds left the question open with player state 6. The installed save was
not used. Final installation evidence is recorded with the private build manifest.

Windows x64, Windows without AVX2 and Android ARM64 compile checks are separate from live
qualification. This does not establish a campaign-wide playthrough or Pixel device test.
The patch handles the game's movie-player videos; it does not bypass scripted gameplay.
