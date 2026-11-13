# Getting started with Modkit

Open **The Simpsons Game Modkit** from the desktop application menu or the
separate Modkit entry in the game package. The game launcher also has an
**Open Modkit** shortcut in Mods. Android imports and manages mod ZIPs;
authoring takes place in the desktop application.

## Your first project

1. Choose **File → New project**. Start with **Scene editor** to place props,
   **Mission editor** for route objectives and props, or another commented
   template for gameplay, combat or camera work.
2. Enter a name, stable id and creator credit. A project starts disabled and
   does not change your ordinary game profile.
3. Use **Project files** or **Ctrl+P** to open Lua, README or mod.json. Each file
   keeps its own tab, cursor, undo history and unsaved changes. **Ctrl+S** saves
   the current file; **Ctrl+Shift+S** saves all changed files. An asterisk marks
   unsaved work. Closing a changed file or switching projects offers Save,
   Discard and Cancel. A conflicting external edit is rejected before overwriting.
4. Choose **Validate** to check files, dependencies and package metadata.
   Validation is separate from proving behavior in the game.
5. Choose **Test** or press **F5**. The viewport hosts the real game on Windows
   and Linux X11. Test uses this project's separate progress and leaves normal
   saves alone. Alternatively, choose a map above the viewport and **Load level**
   to go directly into a fresh private preview with a paused camera.

Validate, Test and Export save changed source files first. If any save fails,
the requested action stops and the affected buffer remains open. Earlier files
that saved successfully keep their new revisions. **File → Reload file from
disk** rereads an externally edited file after the usual unsaved-change check;
it does not silently overwrite your editor buffer.

**Ctrl+F** finds text; **Ctrl+H** opens replacement tools. Search supports case,
whole words and wraparound. Replace All is one undoable edit and cannot modify
read-only files during Test. **Ctrl+G** goes to a source line. New lines retain
the current indentation. Script failures open Output and include recent per-mod
file/line diagnostics from the game log when available.

## Scene workspace

The viewport's level picker reads the original gameflow from your installed
files, including a project's gameflow replacement when present. **Load level**
selects one standard-mode map entry, keeping its original player overrides,
checkpoint and extension flags. The game operates its own starting menus;
you do not need to play through earlier episodes. Each direct preview has
fresh private progress and a one-file gameflow override. Normal game files,
the launcher's selected start episode and your saves are kept intact. Ordinary
**Test / F5** still opens the project's persistent Test progress.

Use **Paused editor** to freeze simulation and fly the native camera; **Free
camera** leaves simulation running. Click the viewport for WASD movement,
arrow-key look, Q/E height, Shift/Ctrl speed and 1/3 zoom. **Frame selection**
positions the camera toward the live outliner selection; the distance field
also controls temporary preview framing for large or small props.

**Map geometry** hides supported factory actors, leaving the static world.
**Isolate selected actor** keeps one live authored actor visible against the
world. **Full scene** releases only the editor's hide requests, preserving
the game's scripted visibility. These are preview filters; they do not delete
entities, remove collision, rewrite archives or export with your mod. Other
entity layouts and effects can remain visible and are reported in the status.
Filters reset on world changes and when an isolated actor is removed.

Once a level is ready, **Assets** lists loaded native resources. Search by name,
class, archive or GUID; multiple words narrow the results together. Category
and **Placeable props** filters help separate supported props from other resource
types. Right-click a resource to bookmark it or copy its source identity.
**Favorites** persist across Test sessions using source GUIDs and archive paths,
rather than temporary game handles. Select an asset for its type, archive and
definition details in Inspector. Supported props can be dragged into the viewport or
double-clicked. The current placement rule puts the prop beside your controlled
character. It does not guess a collision hit underneath the mouse pointer.

Current Scene projects select exact live template identities. Repeated placement
reuses the loaded choices instead of rebuilding the whole catalog on every click;
an out-of-date catalog is refreshed before selection. Older projects with numbered
template choices remain compatible. Runtime handles stay local to the preview,
and saved scenes retain portable GUIDs and source paths. Script safety budgets
remain unchanged.

**Scene outliner** lists authored objects with searchable names, tags and source
types. Select one to inspect its position, source and identity. **Inspector**
exposes XYZ, rotation, scale, duplication and removal. During Test, Scene undo/redo
uses the same history as the native in-game editor. These operations preserve
the same stable object tags, source
retention and save documents; the desktop application does not spawn unrelated
copies through a separate factory.

Outside Test, opening mod.json also exposes saved scene objects in the outliner.
Rename, reposition, rotate, resize, duplicate or remove them without launching
the game. These changes edit the validated portable scene in mod.json and use
its source undo history; they preserve all three basis vectors, including
nonuniform scale. **Alt+Z** and **Alt+Shift+Z** undo and redo scene edits in either
mode. Save All keeps offline edits; Test loads them through the existing scene
service. Offline authoring does not render or check collision. Names are edited
outside Test; live placement and transforms still use native services.

The **Whole game** asset tab searches the native archive index with categories
and paging, including outside Test. Hover a model for a rotating geometry card,
or open **3D preview** to orbit, pan, zoom and frame its real decoded mesh. See
[Asset previews](ASSET_PREVIEWS.md) for format support and extension points.
Select a resource for its source details, then **Prepare source**
to retain its original archive and dependencies. If a resource has multiple
definitions, the source picker pages through all of them rather than silently
truncating its variants. Source loading reports native progress and supports
cancellation. **Native prop preview** creates a temporary native prop in front of the
camera, frames it and isolates it from supported actors. **Clear preview**
removes only that temporary object; it never writes it into your scene document.
To author a lasting object, find the prepared prop under **Loaded resources**
and place it through the Scene editor. Save live drafts to keep that placement.

The browser can inspect models, textures, audio and other resources. Native
prop preview qualifies prop factories; it does not activate arbitrary NPC AI or
pretend that every indexed resource is an entity. Dedicated material/texture,
audio and animation preview surfaces, collision-rich placement, picking and
gizmos remain in the implementation plan.

Open **Options → Extras → Mods** in Test for the project's native Scene,
Mission, Camera or Combat editor. This includes mission point placement,
prerequisites, preview start/failure/retry and camera controls. The desktop
Inspector also edits mission names, rules and prerequisites when mod.json is
open outside Test.

## Keep and share your work

Choose **Save live drafts**, wait for confirmation, then stop Test. **Apply
saved drafts** checks their source fingerprint and updates the project's
mod.json. Mission and scene documents are applied together. Save any source
changes before starting another Test or exporting.

Draft application follows the most recently launched Test, including direct
level previews. Switching levels restarts Test and asks before discarding
unsaved live edits. Save and apply drafts before switching to keep them. Source
files keep their normal revision and unsaved-buffer protection throughout.

Choose **Export package** and a destination ZIP. Creator credit, requirements
and checksums travel with it. A player imports the ZIP in the game launcher's
Mods tab or the Android app, then enables it. Imported mods start disabled.

## Find help and arrange the workspace

**F1** opens the matching API reference when the source cursor is on a game
operation; elsewhere it opens Getting started. Help search covers guide text,
API descriptions and parameter names. Search named operations such as
world.spawn, entities.set_transform or missions.advance_batch. Read the
parameters and ownership rules before extending a template. Source comments
address modders and use public operations instead of generated addresses.

Use **View** to show or hide panels; drag dock headers to rearrange them.
Narrow or tiled windows automatically group the side panels into one tabbed
column, leaving room for the viewport and source. **View → Compact workspace**
also selects this layout manually. Wider windows restore the previous layout;
Reset workspace layout returns to automatic sizing.
**Ctrl+Shift+P** opens a searchable command palette for available actions,
project files and guides. Unavailable actions stay disabled. The toolbar keeps
the common Create, Save All, Validate, Test, Stop and Export actions together;
draft actions remain in the File and Project menus.
**Reset workspace layout** restores the starting arrangement. Output keeps
validation messages and actionable errors. Source editing and project switching
wait until Test closes; live scene edits go through the native creator bridge.
Files may be read during Test but cannot be edited. Background polling keeps
an active Inspector field intact; a foreground edit waits for a poll to finish
instead of disappearing behind it.

The viewport only embeds the editor's own Test process. Linux Wayland display
servers that do not expose foreign windows use the real game in its own window;
asset/property editing still uses the same authenticated session. There is no
screenshot-based renderer or separate rendering implementation.
