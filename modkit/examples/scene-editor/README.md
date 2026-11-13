# Scene editor

Modders: start here for a small prop scene using public APIs and the game's
original Options menu. Read the [scene guide](https://github.com/YesterMester/TheSimpsonsGameRecomp/blob/main/modkit/SCENES.md) in the repository
for the complete workflow and supported resources.

Use Modkit's isolated **Test** action, load a level, then open **Options →
Extras → Mods → Scene editor**. Place props, edit their positions, rotate or
resize them, and use undo/redo to revise your scene. This project starts
disabled and keeps your normal playthrough separate.

Choose **Save scene draft** and wait for confirmation. Close Test, choose
**Apply scene draft** in Modkit and review mod.json before exporting.
Enable the episode autoload toggle if your scene should appear automatically
for players. Importing the resulting ZIP preserves your credit and starts
disabled.

The Lua comments explain the important boundaries: original definition GUIDs
and archive paths identify resources; stable object tags identify owned copies;
temporary native handles never enter saved state. Preview rollback removes
only newly created copies. Do not replace these checks with model-name guesses
or stored addresses.

The initial editor supports 64 props in one episode, XYZ movement, yaw,
uniform scaling and 64 session edits of undo/redo. Its document supports full
world matrices. It does not create maps, collision, navigation or complete NPC
encounters. Adapt the commented code for your own project while keeping those
resource and ownership requirements explicit.
