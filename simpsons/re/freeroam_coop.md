# Springfield free-roam co-op

The original Springfield gameflow attaches a `GameMode_OnePlayer` mode to
`SPR_HUB`. The game already provides co-op players, split-screen cameras and
drop-in/drop-out handling. The native hook raises Springfield's mode limit to
two when the mode is attached, without editing the user's game files.

`sub_828A6900` is `EpisodePkg::AddEntry(episode handle, mode handle)`. A handle's
data pointer is at +4. Episode data has kind 5 at +8, a name pointer at +12 and
a big-endian 16-bit name length at +16. Mode data has kind 3 at +8 and its player
limit at +56; the original `SetNumPlayersAllowed` binding stores there too.
The hook requires both kinds, the exact seven-character `SPR_HUB` name and an
existing one-player limit. It always calls the original function with the
original arguments. Other episodes, time trials and already multiplayer modes
keep their limits.

`freeroam_coop` defaults to true and requires a restart. It appears as **Two
players in Springfield** in the launcher and the in-game Players screen. The
existing input assignments work for two controllers or keyboard/mouse alongside
a controller. Player two presses Start to join and can use **Drop Out** in their
pause menu to return to single player.

The native hook passes ten package-layout checks, including disabled co-op,
different episodes, invalid kinds and a similar name. A private Linux run with
unmodified Springfield gameflow also passes joining, independent movement,
pausing, dropping out to the full-screen camera and rejoining split screen, with
a clean shutdown. It used a keyboard for Bart and a Nintendo controller input
for Homer. Mission transitions and character selection beyond the early save's
unlocked characters still need coverage; Windows and Pixel gameplay are not
qualified by this run.
