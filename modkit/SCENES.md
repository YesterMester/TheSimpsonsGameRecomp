# Build a prop scene in game

**Scene editor** is a commented creator project shared by the desktop launcher,
Android launcher and CLI. Its controls use the game's original **Options →
Extras → Mods → Scene editor** menu. It places original props, edits their world
poses and saves a portable scene in your project.

This is the first scene-authoring workflow. New maps, collision meshes,
navigation, enemy encounters and mission or cinematic editors remain in the
[full implementation plan](PLAN.md).

## Create, test and share

1. In **File → New project** in Modkit, choose **Scene editor**. Enter a name, stable mod id
   and your creator credit. The new project starts disabled.
2. Choose **Test**, load a level in that isolated playthrough and open the
   editor through the game's Options menu.
3. Choose **Refresh loaded props**, select a **Prop** and use **Place
   beside my character**. Template pages contain up to forty choices. The first
   placed prop selects the scene's episode.
4. Select an authored **Object**. Change its X, Y and Z position, choose a
   movement step, rotate left/right or grow/shrink it. You can bring it beside
   your character, duplicate it or remove it. Undo and redo keep up to 64 edits
   during the current editor session. Use **More settings** to reach the next
   page of controls. Long labels are shortened for the TV menu; full object
   names stay in the document and full status messages appear in the game log.
5. Enable **Load automatically in this episode** if players should see the
   scene when they enter its episode. Choose **Save scene draft** and wait for
   the saved confirmation.
6. Close the Test game. In the launcher, choose **Apply scene draft**. Review
   the scene in mod.json, then **Validate**, **Test** and **Export ZIP**.
7. Another player imports the ZIP through the normal Mods tab. It starts
   disabled and retains your creator credit. The package contains scene
   descriptions and your script; it does not need copies of the original
   archives that supply those props.

For the CLI, create the same project with:

```sh
python3 tools/modkit/modkit.py new "My prop scene" --template scene-editor --id yourname.scene --author "Your name"
```

**Unload preview** removes this scene's owned copies and releases its resource
claim without deleting the authored document. **Load scene preview** recreates
missing copies from the original definitions. **Cancel scene loading** rolls
back copies created by that load attempt. Other mods' objects stay owned by
their creators. **Start a new empty scene** clears this scene's preview and
document; saving and applying it replaces the previous draft.

## Documents and object identity

One project has one scene, stored in the optional `scene` field of mod.json.
The schema allows up to 64 objects in one episode and a canonical document of
at most 128 KiB. A saved draft also records the exact source-project
fingerprint. Applying a draft refuses changed source, symbolic links and
malformed documents before replacing mod.json. Your name, description, credit
and other manifest fields are preserved. Re-test after editing project files
to produce a draft for their new revision.

Each object records a stable id, a display name, its original definition GUID,
native class, archive path, placement radius and full world pose. The pose has
`position`, `right`, `up` and `forward` vectors. The menu currently exposes XYZ
movement, yaw in 15-degree steps and uniform size changes of ten percent;
scripts can author other valid rotation and scale matrices.

The document never records guest pointers or temporary entity/template
handles. Stable tags are unique within their owning mod. The runtime rejects
duplicate live or pending tags before creating another object. Tags allow the
editor to recover fresh handles after code reload and exact-save restoration.
The example uses `save_mode = "state"` so its validated document participates
in both checkpoint and exact saves.

Relevant public operations are:

| Operation | Use |
| --- | --- |
| `scenes.document()` | Read the scene and source fingerprint prepared before callbacks. |
| `scenes.validate(document)` | Validate and canonicalize a document without changing the world. |
| `scenes.save(document)` | Queue a draft in the mod's namespaced storage. Check storage status for completion. |
| `world.spawn(..., tag)` | Create a supported owned object with a stable tag. |
| `world.tagged(tag)` | Get this mod's current live handle, or no handle. |
| `world.catalog()` | Discover available definition GUIDs, classes and source streams. |
| `world.status()` | Check level readiness, its episode catalog and active native streaming. |

Lua completion, the generated API reference and C++ helpers describe the full
signatures. The [loaded-object guide](ENTITIES.md) explains transform and
ownership rules; the Library objects project demonstrates selecting foreign
archives explicitly.

## Resource loading and limits

The editor matches the saved GUID and class against the native definition
catalog. If the source is missing, it requests the saved archive through the
original loader and waits for its dependencies. It never substitutes a
similarly named model. Autoload runs only when the loaded episode matches the
document. A missing or ambiguous episode catalog leaves the preview unloaded
with an explanation.

The retained source session supports up to eight selected foreign archives;
dependencies can share files. Sources stay retained while an owner needs
them. Retiring the last owner checks native file identity before releasing
owned files. The editor waits through active native streaming and handles
recoverable busy responses without discarding its pending load.

The initial template picker contains supported props. Factory availability
alone does not establish an NPC's AI, scripts, encounter activation or
navigation requirements. Moving or resizing a renderable prop also does not
author new collision geometry. These limits apply even if a model appears in
the asset library.

## Qualification

A private Linux Vulkan run exercises the public Lua editor actions, world
poses, undo/redo, tag isolation, duplicate rejection, asynchronous drafts,
exact restoration after actual source retirement and code reload. A second
fresh run loads the exported/imported scene without its original test data or
a provider mod. Removing its two copies returns the world to 797 objects,
fourteen files and all 844 baseline resource bindings, followed by a normal
exit.

Host checks cover document policy, pending/live tag uniqueness, stale drafts,
source preservation and credited disabled import. Changed C++ sources compile
for Windows x64, Windows NoAVX2 and Android ARM64; the changed Android creator
class compiles against API 36. Native Windows and Pixel interaction, storage
providers and the wider class library still need their own acceptance runs.
