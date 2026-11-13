# Standalone Modkit editor

Modkit is a separate desktop application included with the game package. It
should feel like a small game editor: real game rendering, searchable resources,
direct manipulation, readable properties, useful errors and help beside the work.
The game launcher is for playing, settings and installing/enabling mod packages.
Android imports and manages mods; it does not expose creator tools.

## Architecture

Keep one runtime, package format and set of game-thread adapters. Separate
desktop entry points share the bundled Python/Qt runtime, rather than shipping
two copies or introducing a second project format. The editor has its own native
window, application identity and layout. It does not use the launcher's game
artwork, cartoon buttons or browser fallback as its interface.

Use Qt dock panels for projects/files, a source workspace, the live viewport,
assets, properties, output and help. Panels can be rearranged or hidden. Restore
a useful layout from the View menu. Keyboard shortcuts, visible dirty state,
source revision checks and clear Test/Stop/Export actions are required.

The viewport hosts only the editor's own Test process on supported Windows/X11
systems. It displays the real native renderer and receives normal game input.
When embedding is unavailable, open the same game in its own window and explain
the platform limitation. Do not substitute a screenshot or pretend preview.

The existing authenticated creator bridge is the live connection. Keep requests
bounded, asynchronous and bound to the selected project and exact Test process.
Reuse native Scene/Mission pages and ownership rules for edits. No arbitrary
memory writes, stale native handles or second spawning implementation.

## Delivery stages

1. **Separate applications.** Ship Windows/Linux Modkit entry points, professional
   native project/source/property panels, built-in reference and guides, and the
   existing create/edit/validate/Test/apply/export workflow. Remove authoring from
   the desktop game launcher and Android. Preserve existing projects and progress.
2. **Live scene workspace.** Embed the owned Test renderer, browse current native
   assets, drag supported props into the scene, select owned objects and edit
   transforms through the same Scene editor operations. Save portable drafts,
   apply, export and load the package in another session. Explain placement and
   supported classes honestly; a factory call alone does not qualify an NPC.
3. **Whole-library authoring.** Add indexed unloaded resources, dependency and
   memory-cost feedback, source preparation/cancellation, source-aware previews
   and typed import/export. Native loader, collision and lifecycle checks gate
   each asset family. Keep large scans off the UI/game threads.
4. **Direct manipulation.** Add camera-correct picking, move/rotate/scale gizmos,
   snapping, selection outlines, multi-selection and bounded transactions. Picking
   must use proven matrices/hits; a guessed mouse-to-world projection is not a
   finished placement tool. Retain the same undo/document/ownership contracts.
5. **Mission and cinematic workspaces.** Build visual objectives, encounters and
   camera tracks on the public services. Add campaign/HUD integration only after
   proving its native lifecycle. Keep videos and simulation timing correct.
6. **Creator experience.** Add guided first projects, contextual examples, an
   offline searchable API/help system, readable diagnostics, localization tools,
   asset validation and complete acceptance projects. Explain prerequisites and
   unsupported edits before users lose work.

Stages may overlap, but qualify complete workflows. Record implemented behavior,
actual platform evidence and remaining work in coverage.json and the work log.
Do not advertise the full kit as released until the full gate in PLAN.md passes.

## Workspace update

The desktop workspace now keeps separate source tabs, unsaved buffers and undo
histories, with revision-checked Save All and explicit conflict recovery. Find
and replace, Quick open, a command palette and contextual offline help support
keyboard authoring. Assets use a filtered model with categories and portable
favorites. The searchable scene outliner and Inspector edit both the native live
scene and validated saved source objects; offline changes use source undo and
preserve complete basis vectors. Background reads do not drop foreground edits.

The expanded Qt workflow and creator Python checks cover source retention,
partial save failure, portable favorites, offline transforms and undo, read-only
Test sources and world-change handle refresh. A private Linux X11 run separately
qualifies real renderer embedding, loaded prop placement, native transform
changes, undo/redo, draft application and credited disabled package import.
Native Windows drivers, viewport picking, gizmos, collision authoring and the
remaining full release gate need separate qualification. Cross-level prop
previews are covered by the later viewport workflows below.

## Viewport preview workflows

Direct level previews read the player's installed gameflow through the same
parser used by checkpoint sweeps. Standard map entries retain their original
configuration. The editor mounts only a private gameflow override over the
original archives, starts with fresh Test progress and drives the original
menus. Paused/free native camera modes, explicit framing distance and balanced
actor-visibility leases keep authoring independent of campaign progression.
The whole-library browser uses bounded native queries, source preparation,
cancel/status services and temporary owned prop previews. Preview objects do
not enter Scene documents or get silently exported.

Private Linux runs qualify automatic Land of Chocolate and Bartman Begins
entry, paused camera/framing, geometry and selection filters, balanced reveal,
and a Land of Chocolate prop loaded/rendered/removed in Bartman Begins. Both
runs retain native Scene placement/edit/undo/draft/export behavior. Other
platform execution and the rest of the campaign remain separate evidence.

Next delivery order:

1. Dedicated previews for textures/materials, audio and animation, with source
   identity and supported-format diagnostics. General resources must not be
   mislabeled as spawnable props.
2. A native studio/world visibility path for isolated asset backgrounds and
   reviewed camera bounds, followed by camera-correct picking and gizmos.
3. Full base-world outliner, category visibility and live checkpoint navigation,
   with lifecycle, collision and original-save qualification per asset family.
4. Finish the mission, encounter, cinematic and package acceptance projects in
   PLAN.md; retain the independent Windows/Linux creator and Android consumer
   boundary. A source compile is not native-driver/device acceptance.

Offline geometry preview now has a separate read-only asset path using the
existing native format readers. Indexing/decoding run off the UI thread, source
identities are checked, and a bounded cache feeds real vertex/index buffers to
rotating hover cards and an interactive inspection view. A private Linux GPU
run indexes 19,994 resources without launching gameplay, then renders and orbits
the original 363-vertex, 498-triangle coffee-table resource. These neutral,
bind-pose mesh views do not replace native material/behavior qualification or
complete offline world authoring and transform gizmos.

## Acceptance

- Launch the game launcher and Modkit independently and together; they must not
  start duplicate games, corrupt source files, alter normal saves during Test or
  compete for one local server/window identity.
- Create a credited project, edit/save Lua and guided mission properties, reject
  stale source edits, validate, Test, apply actual saved drafts, export and import
  disabled. Close with unsaved changes without silently discarding them.
- Observe real renderer frames in the viewport, use native menu/game input,
  place a supported prop from the asset panel, change its pose, undo/redo and
  remove it. Saved documents and original game resources must remain correct.
- Provide useful empty, loading, disconnected, missing-runtime and failed-import
  states. Every visible action works or explains its exact prerequisite.
- Check Windows and Linux packaging, keyboard navigation and Steam Deck screen
  sizes. Compilation and native-driver/device execution are separate evidence.
- Maintain the Steam Deck's 25 GiB disk reserve; share runtime artifacts and build
  incrementally. Never publish game data or overwrite user projects and saves.
