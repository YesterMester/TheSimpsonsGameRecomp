# Taps and clicks on the game's own screens

`simpsons/src/game_menu_pointer.cpp` gives the game's own menus touch and mouse input. Screens
are found when `sub_827F2DC8` shows them (state at +28 is 1 while shown, parent at +32, the movie
path through the handle at +96). The input method is the screen class's vtable entry +100, called
as `(screen, player, key)`. A screen counts only while its first word is still the class it was
shown with: some screens are freed without a close, and their memory is reused.

## Keys

The input methods take the game's UI events, in the order the menu scripts name them
(`UI_EVENT_PAUSE` … `UI_EVENT_RIGHT_STICK`). Measured with a controller on the bus stop and the
notepad:

| Key | Button |
|---|---|
| 0 | Start (pause) |
| 1 | Back (select): opens and closes the notepad |
| 2–5 | D-pad left, right, up, down |
| 6 | A (accept) |
| 7 | B (cancel) |
| 8, 9 | LT, RT |
| 10, 11 | LB, RB |
| 12 | X (option) |
| 13 | Y (help): the notepad's compass |

Hit regions keep their own codes (1 Start, 8 X, 9 Y); `ScreenKey` converts them. The front end's
startup screens poll the controller instead and get queued buttons.

## In-game screens

During play the front-end state (`0x82D08C98`) is 9 after loading a save but 7 after starting a
New Game (until a later load; `game_ui::LevelRunning` accepts both), and `0x82D572DC` is 1 while a
menu is up (2 otherwise); it is copied from `0x82D572E0` by `sub_82691208`.

**Notepad** (input `sub_82399D98`, movie `_level2`, clip `GoalsRoot.EpisodeGoals`): A and B turn
pages, Back closes it, key 13 asks `GoalsRoot.EpisodeGoals.CanAccessBusStop` and, if allowed,
closes the notepad to open the bus stop map. Its Y button is the text `$FE_Button_EPGoalsMOHMap`,
drawn in the middle of the `Compass` clip.

**Bus stop** (input `sub_82391E88`, movie `_level2`): pages are clips set as `ActiveMenu` —
`QueryMenu` (Change Location / Change Character), `BusStopMap`, `1PCharSelect`, `2PCharSelect` and
`OutfitSelectScreen.OutfitSelect`. The movie is laid out on a 512×448 stage stretched over the
frame; each page sits at (256, 224) with `_xscale` 60 and `_yscale` 93.3 in widescreen.

- `BusStopMap`: `numBusStops`, the selected stop `curBusStop`, and the stops' icons
  `marker0`…`markerN` (positions from the screen's `GetBusStopXCoord`/`YCoord`). The D-pad step
  (`findNextBusStop`) sets `curBusStop`, calls `updateBusStops` and plays
  `_level0.ui_thumbnail_swap`; A rides to `BusStopMap.getSelectedBusStop`.
- `1PCharSelect`: `Characters` = Bart, Homer, Lisa, Marge (clips of those names, rotated −2°).
  `curChar` is the player being chosen (0 main, 1 buddy) and `char1`/`char2` the characters picked
  for them. Each character clip holds its icon on the phone's arc (`Still_mc`) and its pictures in
  the main and buddy circles (`Char_mc_red`, `Char_mc_blue`). Right moves to the next character
  that can be chosen; A confirms; B from the buddy goes back to the main, and from the main closes
  the bus stop.

The map's "Navigate" prompt (`$FE_Dpad`) is the `[LDLeft]` glyph drawn as the whole D-pad.

## Front-end pages without rows

Replay Episode, Movies and Clichés have no menu rows, so their pictures and arrows are targets of
their own; a sideways swipe (or the mouse wheel) moves through them too. Their movies use the same
512×448 stage, with the page clip at (256, 224) scaled 60 % × 93.3 % in widescreen.

- **Replay Episode** is the `EpisodeSelect` page of the front end's own screen (input
  `sub_8239F330`, movie `_level2`, `ActiveMenu._name` `EpisodeSelect`). The `Episodes` clip has
  `currentEpisode`, `maxEpisodes` and `bLeaderboardMode`; its arrows and picture are unnamed
  shapes, measured on screen in that clip's units: arrows at (−145, −50) and (140, −49), the
  picture from (−124, −106) to (122, 15). Left and Right change the episode, A plays it.
- **Clichés** (input `sub_82392960`, movie `cliche_summary`) and **Movies** (input `sub_8239A7A0`,
  `fmv_browser`), both `_level3`, are carousels: `ClicheScreen` and `FMVScreen` hold a forward and
  a backward slide (`ClicheFwd`/`ClicheBkwd`, `MoviesFwd`/`MoviesBkwd`), of which one is visible,
  with slot clips `obj1`… (ten for clichés, six for movies). The slot drawn largest, at x = 0, is
  the current item; slots beside it fade out (alpha 80, 50, 20 for clichés; 50, 30, 15 for
  movies) and empty slots have no size.
  `FirstClipNum` goes up by one a move and `bStopped` is 1 once the slide has finished, so a tap
  on a slot two places away sends two moves, each after the last has shown. The movie browser has
  `leftArrow`/`rightArrow` clips; the clichés' arrows are drawn over the third slot on each side.
- A movie chosen in the browser (`FMVScreen.bMoviePlaying`) reads the controller itself: B stops
  it, but key 7 sent to the screen's input method did not, so a tap queues a B press instead.

## Testing without a touch screen

`menu_pointer_test_file` reads taps, clicks and swipes (`tap X Y`, `click X Y`,
`swipe X Y X2 Y2` in game image coordinates) and `probe`, which logs the APT values listed in
`<file>.probe`. The harness's `memscan` step found the scripts' names in guest memory, and
`memrange` dumps of the loaded movies (each starts `Apt Data:74`, its constants after
`Apt constant file`) gave the clip names above.
