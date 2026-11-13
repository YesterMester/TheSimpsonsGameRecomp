# Mission editor

Create a **Mission editor** project in **File → New project** in desktop Modkit, or use the same bundled template from the repository root:

```sh
python3 tools/modkit/modkit.py new "My mission" --id yourname.mission --author "Your name" --template mission-editor
```

The project starts disabled. Its main.lua uses the shared Scene editor in
scene.lua; the template generator supplies that module from the Scene editor
source. Both editors use public APIs and the game's original Options movie.
They share one package, source fingerprint and saved state.

## Author and play

1. Choose **Test** and load the episode where the mission belongs. Test uses
   separate progress and keeps the normal mod profile unchanged.
2. Open **Options → Extras → Mods → Mission editor**. Add an objective at your
   character. The action uses the logical player operating the menu.
3. Choose **Reach point**, **Stay at point** or **Wait**. Move points with XYZ
   controls or stamp them at your character; set the radius or duration.
4. Choose a prerequisite and use **Add or remove prerequisite** to toggle it.
   Each new point initially follows the selected point. Cycles, missing ids
   and an entirely optional mission are rejected before replacing the draft.
5. Set optional goals, retry points, a time limit and the defeat rule. Reorder,
   duplicate and remove points as needed. Removal also removes their incoming
   prerequisite references. Undo/redo retains up to 64 edits for this session.
6. Open the project's **Scene editor** page to place props. See [Scenes](SCENES.md)
   for resource selection, transforms and ownership. This is the same editor,
   rather than another implementation with different save rules.
7. Choose **Play from beginning**. Only living family members controlled by
   logical P1/P2 satisfy visits. Staying requires the same player to remain
   continuously inside the radius; leaving or handing the visit to the other
   player resets that hold. Wait objectives need an active living player.
   Newly unlocked goals are evaluated on the next sample.
8. Use fail, retry or stop actions to revise the result. Stop before editing;
   edits replace the idle mission definition and validate the whole document.

The editor samples at roughly 0.1 simulation seconds. Pausing stops its clocks.
The timeout applies to the mission; zero disables it. The defeat rule fails
when controlled family members exist and all have fallen. An absent controller
does not count as a defeat. Optional goals do not delay completion.

A retry point records objective progress, elapsed time and continuous holds.
It does not rewind the world's enemies, props or player positions. Use a game
checkpoint or supported exact save for that. The editor stores logical player
indices, document ids and values; it does not save temporary entity handles.
Unchanged source reload and exact restore rebind the native mission service
without resetting its saved progress or partial sample clock.

## Save and share

Choose **Save mission and scene drafts**, then wait for its completion message.
The Scene editor's save action also saves both documents when used in this
project. One storage transaction writes project-drafts.json, preventing half
of a combined save from replacing the other half.

Close Test before choosing **Apply saved drafts** in Modkit. The launcher
validates both documents and checks the source fingerprint before replacing
mod.json once. If another editor changed the project, it rejects stale changed
drafts; already applied identical documents are harmless. Newer individual
drafts from older tools are also recognized. Applying drafts preserves package
metadata and creator credit.

Open **Mission settings** for titles, description, objective rules and
prerequisites. The guided form and **Edit JSON** view edit the same mod.json.
Save before testing or exporting; a conflicting source edit requires reopening
the file. Invalid graphs leave the project file untouched.

Enable **Start on episode entry** and the Scene editor's autoload toggle when
the package should start itself. The mission waits for its matching scene's
owned tags before starting. Validate and Test the applied project, then export
it. Another player imports the ZIP disabled, with its credit preserved.

## Portable document

The package's `mission` object has schema 1, a name, description, `level_root`,
positive contract `revision`, `time_limit`, `fail_on_defeat`, `auto_start` and
an `objectives` array. Each objective has an id, title, kind, position, radius,
seconds, prerequisites, optional flag and checkpoint flag. Wait rules retain a
position so changing their kind does not discard it. Positions and radii use
native float bounds; durations use simulation seconds. A document describes
one exact `.str` episode root and has at most 32 goals and 64 KiB of metadata.

`projects.document`, `projects.validate`, `projects.save` and
`projects.save_all` expose the same workflow to Lua and C++. The native mission
service separately owns progress. `missions.advance_batch` validates all
updates against the same prerequisite snapshot before committing any;
`missions.remove` retires only the caller's definition and pending changes.
See [Missions](MISSIONS.md) for service ownership and migration rules.

This route editor does not complete campaign game flow, mission HUD integration,
general enemy behavior, cinematic tracks, map/collision/navigation authoring or
device qualification. Those remain in the [full kit plan](PLAN.md).
