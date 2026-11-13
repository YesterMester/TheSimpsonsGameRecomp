# Experimental Knockdowns

A preinstalled example mod, disabled in the default profile. Enable it in Mods
for a test save, then change its options in the in-game Mods menu.

This prototype moves and rotates a character's root transform. It is **not** a
jointed Havok ragdoll or an Euphoria implementation. Ground height is taken
from the initial pose; stairs, slopes, obstacles and authored sequences need
further work. It is not qualified for campaign progression.

The example shows mod-owned host settings: `game.physics.knockdowns(options)`
applies a complete, validated request, and unloading or disabling the package
releases it. The host restores surviving actors on the game thread. Simulation
pauses stop its motion. Failed or rejected hits do not create reactions.

Keep the existing `euphoria.ragdolls` package ID for saved profile compatibility.
A future jointed physics implementation can replace the prototype behind the
same owner-aware controls without adding special cases to the native menu.
