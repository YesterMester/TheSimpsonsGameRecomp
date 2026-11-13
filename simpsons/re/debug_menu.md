# Debug menu

The debug menu (`simpsons/src/debug_menu.cpp`) and the achievements screen
(`achievements_screen.cpp`) are drawn inside the game's frame by `game_ui.cpp`, with the game's own
2D renderer and fonts (`game_canvas.cpp`). Only the pop-up for a new achievement still uses the
runtime's ImGui overlay (`overlay.cpp`, `achievements_ui.cpp`). Everything that changes the game
runs on its main thread through `game_thread.h`: the frame-start hook (`sub_82718788`,
frame_pacing.cpp) runs queued tasks with the thread's own context, so guest functions can be
called directly. The game reads input, updates and draws on that same thread.

## Drawing in the game's frame

The font manager at `0x82D08EA8` draws text as a batch of textured quads: its camera at +44 (the
target raster at camera +96, width +12, height +16), vertex buffer +68, count (u16) +72 and
capacity (u16) +74. Vertices are 28 bytes (x, y, z, rhw, ARGB, u, v) and `sub_826BE8C0(manager)`
flushes them. Fonts are listed at manager +16 (count +20), the current one at +28; a font's glyph
table starts at +36 (u16 width, height and advance at +2/+4/+6, s16 offsets at +8/+10, atlas
coordinates at +12..+18 in sixteenths of a texel) and its atlas texture is at +1060.

The canvas points the manager at its own 4,096-vertex buffer, draws, flushes and puts the
buffer, the render states and the raster back. Render states go through the device at
`0x82D0CA68` (+32 set, +36 get): 1 raster, 6 z test, 8 z write, 10/11 blend (5/6), 12 vertex
alpha, 14 fog. Solid shapes sample the middle of the font's `_` glyph. Triangles must wind like
the font's quads (negative cross product) or they are culled.

There are two 2D passes a frame: the HUD (`sub_826D5050`, ending in `sub_826D4840`) and the front
end's APT render (`sub_827BCAB8`, called only from `sub_827F58D8`). The front end's pass is the one
that shows when no level runs or `0x82D572DC` is 1 (menus and pause); otherwise the HUD's does.
A running level has `0x82D08C98` at 9 after loading a save but 7 after a New Game (until a later
load). Checking for 9 alone left every screen and shortcut invisible in play after a New Game;
`game_ui::LevelRunning` accepts both. The screens are drawn once a frame at the end of whichever pass shows. While one is open the
HUD's own movies are skipped (`sub_8272BA10`, freecam.cpp) so they don't show through.

Calling the front end's sound objects (`_level0.ui_*.start`) from these screens hung the
interpreter once; menu sounds are off unless `game_ui_sounds` is set, and only play when the
object exists.

## Opening it

The pause screen is a singleton at `0x82D08E30` with its state at `0x82D08E34` (1 while its menu
is up). `sub_823A5C50` shows it and `sub_823A6C78(screen, player, key)` takes its input. While it
is up and Options is not open over it, Y and X on any controller open the achievements and the
debug menu (the presses are taken out of the XInputGetState thunk, `sub_82B766A8`). X resumes the
game first: the debug menu works over the game, not the pause menu. In play, RB + Left opens the
debug menu; F2 and F5 open the screens from a keyboard. *Options → Extras* has both once a save is
loaded, which is the way in with touch.

Open screens read the controllers through the same thunk and keep the game from seeing the menu
buttons (the debug menu leaves the sticks to the game, so the camera and the family still move).
Keyboard, mouse and touch come from a window listener above the game's menus. A press that closes
a screen is held from the game until it is released.

The pause movie is `_level2`. Its constant pool (found in guest memory) uses the same base menu
script as Options: `TextRefs`, `MenuItemButtons`, `MenuItemIds`, `RemoveItems`,
`initializeButtons`, with nine items from `Resume_game` to `Exit_game`, the optional ones removed
through `IsPlayerOneInControl`, `CanQuitEpisode`, `IsChallengeMode` and `IsPlayingDemo`. Unlike
Options the arrays are not readable afterwards (`ActiveMenu.MenuItemIds` is empty), so real list
items would need a different approach. Reading paths through the `Pause` frame label hung the
interpreter; `ActiveMenu.*` is safe.

## Players

| Address / offset | Meaning |
|---|---|
| `0x82D08BA4` | Player manager; its list at +8 has an array at +4 and a count at +8 |
| list entry − 2032 | Player entity (vtable `0x82004E98` Homer, `0x82004988` Bart, …) |
| entity +3460 | Character: 0 Homer, 1 Bart, 2 Lisa, 3 Marge |
| entity +140 | RenderWare frame: modelling matrix at +16 (right, up, at, position at +64), LTM at +80 (position at +128) |
| entity +344 | Health component: hit points at +16, maximum at +20, the value the low-health check divides by at +24, flags at +28 |

`sub_82895130(manager, character)` finds a player; the Player class vtable is `0x82181A60`
(destructor `sub_828884F0`), with its message handler at +332. Health is read through vtable +176
(`sub_823BE4C0` → component vtable +32).

**God mode.** The health component's `iMsgDamage` handler (`sub_82957478`) asks
`sub_82957208(component, damage)` whether a hit counts; 2 means it is ignored, as for a component
that is already down. The hook returns 2 for the family's components.

**Teleport.** Position changes dispatch entity virtual +28. The character override
`sub_8288D2E0` calls the base setter and the controller at entity +784, virtual +60
with flag 0. This replaces the earlier direct writes into both matrices. The shared
adapter and frame hierarchy are described in [the entity notes](entities.md).

**Jumping** is the `PlayerSimpsonsJumpSM` state machine with its own gravity (about 35 m/s²
from the jump arc: 1.7 m peak in 0.33 s); it does not use the Havok world's.

## Spawning

The level loader (`sub_826A3418`) builds each entity from a stream record: flags at +16 (bit 0:
has a header), a relative offset to its 16-byte GUID at +24 and the class hash at +28.
`sub_82703CA8(class hash, record)` looks the class up in the registry filled by
`sub_82704008(hash, name, create, size)` (`sub_82703E90(hash)` returns the name), builds the
entity. The loader separately registers it with the world (`sub_826A31F0`, which copies
the GUID to entity +28). Once a layer has loaded, `sub_826A0A28` sends `iMsgPostSpawnInitialize` (id at
`0x82D583C4`) to its entities through the subscriber at entity +56 (vtable +4, message
`{id, 0, 1}`).

A copy is built from a live source record, given a unique GUID and registered in the direct
entity index and its source layer before post-spawn initialization. The [entity notes](entities.md)
describe the registration and destruction paths. Records hold no plain position, so the
copy is moved afterwards through entity virtual +28, including class-specific controller
placement and transform notifications. The earlier heuristic pointer traversal and float-triple
rewrites have been removed. Copies are placed in rows in
front of the leading player, clear of earlier copies that are still there (when a person was
spawned on the spot of an earlier car, the car was gone seconds later). Springfield builds about
2,800 entities of 93 classes on load. Records are forgotten when the level unloads
(`sub_82691530(0)`), and a record is not used once its original entity or the record's class hash
has changed, as when streaming unloads its section.

**Names.** The spawner lists everything by name: the class gives the category and a fallback
name, and the entity's asset paths give the rest. A weapon's name is at +528 (`_pipe`); models are
found through +136 → +180 → +28, +136 → +124 → +244/+216/+196/+188 and +136 → +148 → +28/+8;
characters through +236 → +148/+100 and +236 → +28 → +4; others at +1180 → +8, +428 → +132 and
+156 → +44 → +144. A name must have an underscore or a digit; a table turns known assets into
proper names (`genericcompactcar` → Compact car, `agnes_skinner` → Agnes Skinner) and the rest
lose their level prefix and part suffix.

## World

| Address | Meaning |
|---|---|
| `0x82CED750` | Game clock time scale: 1 normally, 0 while paused (game_clock.cpp) |
| hkpWorld +16 | Gravity vector, (0, −9.8, 0) in Springfield; the world comes from `sub_82AA2070` (`hkWorld::stepDeltaTime`) |
| `sub_826D5CB0` | The UI scene's draw callback: returning 1 hides the HUD (freecam.cpp) |

Game speed multiplies the time scale only while the game has it at 1, so pausing still pauses.
Gravity scales the level's own vector for each Havok world.

## Progress

Unlocks set the `_LOCKED` scores (episodes, time challenges, costumes) and the bus stop
`_SELECTABLE` scores through the ScoreKeeper (see `achievements.md`), never the `_COMPLETE` scores
that achievements watch. `SCORE_POWER_INFINITE` and `SCORE_EVENT_POWER_INCREASE` give infinite
power and a power upgrade with the game's own screens; the game saves both.

## Development switches

`debug_menu_log_players` (with `debug_menu_log_players_frames`), `debug_menu_log_entities`,
`pause_menu_probe` and `pause_menu_test_screen` log the structures above or open a screen with the
pause menu for controller-only test runs. `game_ui_log_input` logs the presses the screens read,
and `game_ui_test_file` takes `click X Y`, `move X Y` and `tap X Y` (game image coordinates, 0 to
1) for test runs without a mouse or touch screen. `characters_test = "one,two"` picks character
templates for players 1 and 2 once the first level runs, to test the restart without the menu.
