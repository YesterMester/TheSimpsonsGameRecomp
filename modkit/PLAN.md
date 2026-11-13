# Modkit implementation plan

The release gate is the full suite below. A working foundation or one example does not
complete the kit. `coverage.json` records the current evidence and unfinished work;
`api.json` lists implemented operations only.

The [standalone editor plan](EDITOR_PLAN.md) defines the requested professional
desktop application, real renderer viewport, asset browser, direct manipulation
and integrated help. It shares this kit's formats and services. Android keeps
mod importing and profile controls; creator tools belong in Windows/Linux Modkit.

## Creator experience and completion standard

The goal is a community that can make small changes, new missions, characters, maps and
larger conversions through approachable, documented tools. Judge completion by what a
creator can build and share. Code indexes, readable internal names and individual hooks
support that work; they do not complete the creator experience.

Use one project format throughout this workflow:

1. **Create.** The standalone Modkit offers guided projects with working, commented templates for
   gameplay, missions, assets and translations. A first visible change needs no C++ compiler
   or knowledge of generated functions. Advanced native plugins remain available.
2. **Edit.** Creators use named properties, events and Lua APIs with completion and examples.
   Artists get documented asset import/export and previews. Object, mission and cinematic
   editors use the game's native menus and save back into the same editable project.
3. **Test.** A Test action selects isolated progress and the project's test scene/checkpoint.
   Supported edits reload with useful state; changes needing a restart are identified clearly.
   Errors identify the mod, source file and affected object or resource.
4. **Validate.** Report missing dependencies, conflicting replacements, invalid assets and
   unsupported target capabilities before export. Distinguish implemented features from
   platform behavior that has actually been qualified.
5. **Share.** Export an installable package with credits, version, requirements and optional
   editable source. Another player imports it through desktop or Android Mods, sees its
   dependencies/conflicts, and can disable it while preserving vanilla files and progress.

The acceptance projects must demonstrate a complete mission with objectives, encounters,
failure/restart and checkpoints; an imported character or prop with its required resources;
an authored playable area with collision and navigation; and translated text through the
native UI. These supplement the full suite below. Tutorials must reproduce each supported
workflow from a clean creator project, without private scripts or undocumented addresses.
The save-system example is shelved. Prioritize small, practical starter projects and a
guided translation-pack workflow instead; keep existing save files and research intact.

Prioritize complete creator workflows. Finish the missing mission/lifecycle/ownership APIs
for a source-commented mission project, then qualify a complete asset import/load/retirement
path and build editors on those same public services. Reuse the current runtime, profiles,
packages, translation editor and creator bridge. Expand research names as those workflows
need them; naming every generated function is not a prerequisite for a usable modding kit.

Hit & Run's documented mod packaging, asset tools and editor assistance provide useful
workflow references: [creating mods](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/Mods/CreatingMods),
[tool documentation](https://github.com/donutteam/docs), and
[Lua editor definitions](https://github.com/donutteam/lucas-mod-launcher-lua).
Our STR/RenderWare tools and native adapters must implement this game's own formats and
behavior; these references do not establish file or plugin compatibility.

Current creator deliveries include the desktop [source workflow](CREATOR_WORKFLOW.md)
and the [mission service](MISSIONS.md). Android imports and manages packages. The route example proves
objectives, failure/retry, saved state and reload in a private Linux scene. It does not
complete campaign encounters, mission HUD integration or asset authoring.
The [Mission editor](MISSION_EDITOR.md) adds native route authoring with the shared
prop Scene editor, prerequisite validation, continuous visits and timed goals,
retry points, undo/redo and atomic combined drafts. The launcher offers guided
mission properties. Cinematic editing, campaign/HUD integration, complete enemy
behavior and platform qualification remain in the full gate.

The [owned-object workflow](ENTITIES.md) adds safe loaded-template registration, removal,
reload transfer and an ownership example in both creator launchers. Private Linux checks
cover two independent mods and failure cleanup. General component/transform authoring,
broader spawn-class acceptance, exact-snapshot game qualification and device runs remain
part of the gate.

Owned-object world transforms and placement now use the original frame getter
and class setter, including parent conversion and character-controller dispatch.
Full world-pose authoring now also dispatches the class matrix setter, including
rotation, scale and native model/controller notifications. The commented Owned
objects project exposes those operations in its native menu. Broader typed
components and collision-rich placement remain separate work.

Resident template discovery now reads the original definition index and native
class registry, rather than depending entirely on live source instances or a
fixed class list. Distinct behavior records stay selectable. A private Linux
hub run exposes 276 templates, including 106 without a live source object,
and qualifies retained ScriptableProp creation, world placement, simulation
and balanced removal. Original lookup comparisons, stale-owner guards,
sanitizers and Windows/Android cross-compiles cover the reader. Foreign archive
loading now has an experimental native-menu implementation. The source browser
indexes 415 installed SimG archives, requests the episode catalog and selected
archive through the original loader, holds foreign layers inactive and retains
their definitions. A Linux hub run adds 165 selectable templates from 343 foreign
definitions, without creating source actors. Native release returns 17 files to
the original 14 and the object count to 797, preserving all 844 baseline resource
bindings. An exact save restores both the source and its owned prop after their
retirement; a second release preserves those same baseline resources. The game
exits normally. Broader class behavior and native Windows/Pixel qualification
remain in the resource-streaming gate; resources are not all spawnable entities.

The retained source session now accepts up to eight archives without replacing
older spawned objects. A private Linux Vulkan run combines eight archives from
two episodes, retains 1,671 definitions, exercises native-IO cancellation and
claim rollback, and restores three independently posed props through exact saves
and code reload. Last-owner retirement returns 26 files to fourteen and 800
objects to 797, preserving all 844 baseline resource bindings and exiting zero.
The commented Library objects creator page includes retained-archive counts and
explicit cancellation; public APIs and save preflight use the same ownership
rules. Eighteen Windows x64/NoAVX2/Android ARM64 object compiles pass; live device
and broader behavior acceptance remain open.

The [Scene editor](SCENES.md) now supplies the first native prop-scene workflow:
stable owned tags, original definition identities, XYZ movement, yaw, uniform
scale, duplication, removal and bounded undo/redo in the original Options menu.
Validated drafts transfer from isolated Test storage into mod.json with source
revision checks, then export and import disabled with creator credit. Private
Linux checks exercise actual editor actions, exact/reload rebinding and fresh
imported-scene autoload, returning to all 797 objects, fourteen files and 844
baseline bindings on retirement. This does not complete map/collision,
encounter, mission or cinematic editing; native Windows/Pixel acceptance remains.
Native mod pages also refresh delayed row/title changes, preserve the current
slider selection and normalize a page offset when callbacks shorten its rows.
A private Linux keyboard/menu regression qualifies those cases, including an
actual paused source reload; unchanged pages keep their APT animations intact.

The [combat workflow](COMBAT.md) adds native health queries, scripted damage,
healing and owned death notifications. Combat workshop combines those services
with authored objectives, failure, checkpoint retry and reload rebinding. The
source project has deterministic host checks; private Linux checks separately
exercise actual loaded brawler resources. Broader enemy activation, AI,
collision/script dependencies, campaign HUD integration and device qualification
remain required before treating it as a complete campaign encounter workflow.

Owned visibility queries and hide/reveal requests now use the original native
model/scene methods. The owner contributes one balanced hide request, transferred
on reload and canceled by a native force-show. This is a visual authoring service;
AI suspension, targeting, navigation and complete enemy behaviors remain in the gate.

Loaded model GUIDs distinguish catalog variants and link them to named archive
metadata. The native loader supplies owned name strings without an additional
disk scan; ambiguous and replaced payloads do not lend stale labels. Combat
workshop includes an in-game template picker and keeps the encounter's resource
separate from the template being browsed. This is loaded-model selection, not
complete prefab/behavior authoring. Controlled Frink source/copy views now match,
including all 26 geometry/material bindings; other scripted appearances and
classes remain acceptance work.

Native camera authoring exposes applied world poses, rotation, vertical lens
angles and targeted release through the original camera writer. Its shared
Camera workshop captures and previews shots in the original menu. Ownership,
manual takeover, saved pose validation and projection arithmetic have focused
checks; full-scene exact restore, tracks, visual editing and device qualification
remain in the camera gate.

The [model tools](MODELS.md) add a typed MMdl reader and fixed-size writer,
checked against all 2,616 installed definitions and the original relocation
routines. Source-checked ModelPart variants use prepared package documents,
existing native references and the original loader's ownership. A private Linux
Frink comparison qualifies native binding, rendered source/copy views, simulation
and balanced cleanup for that variant. Broader live acceptance, new geometry/rig
import, collision, animation and dependency authoring remain in the typed-asset gate.

The [mesh attribute workflow](MESHES.md) now exports reviewed preinstanced positions
and triangles as OBJ and packages bounded position edits. All 5,533 installed
mesh resources and 20,490 native blocks pass the original relocation/copy oracle.
A private Linux Vulkan coffee-table edit verifies 363 live vertices, rendered
prop views and balanced native removal. Opt-in packed normal authoring adds
matching directions for shape edits; 1,881,360 source normals round-trip exactly
and a second edited-prop run verifies native positions and normals. New topology,
automatic normal generation, multipart native draws, skinning, collision and
platform acceptance remain in the asset gate.

Reviewed float2 UV streams support independent texture-coordinate edits on
static and skinned geometry. Byte-exact writer checks cover 6,436,832 UV pairs
across 15,321 geometries, including 12,176 skinned ones. A private Linux Frink
run verifies the bound vertex stream, preserves skinning and other attributes,
renders both views and removes the owned NPC with its world count restored.
Texture-image import, new seams/topology, rigging and multipart draws remain.

## Runtime and creator tools

| Area | Required delivery | Acceptance |
| --- | --- | --- |
| Public SDK | Stable C ABI, readable C++ helpers, Lua completion and generated reference | The same creator project builds independently of the game on supported targets. |
| Game code discovery | Whole-tree function inventory, direct call sites, indirect-call limits, reviewed names and source evidence | Creators can search by game system, follow the implementation and find a public API; changes invalidate stale results without renaming generated symbols. |
| Script runtime | Isolated Lua state, memory and callback budgets, simulation timers, explicit failures | Faults retire one mod and its dependents without stranding resources or affecting unrelated mods. |
| Profiles and packages | Dependencies, conflicts, ordered priority, creator credits, fingerprints, import/export | Desktop and Android install disabled, resolve the same profile, preserve base files and isolate saves. |
| Persistence | Namespaced asynchronous storage, checkpoint state and exact-save sidecars | Read-after-write, restart, failed writes, cancellation, changed-source rejection and restore preflight are qualified. |
| Code reload | Background verification, immutable code, staged initialization, state and timer migration | Lua and native code reload during play; failed preparation keeps the current instances; shutdown and cancellation remain bounded. |
| Owned resources | Per-mod and per-generation cleanup for every reversible host service | Retiring an owner preserves other owners and manual controls; saves restore complete supported host state. |
| Native UI | Original menu movies, mod pages and authoring tools, dynamic input prompts | Controller, keyboard, mouse, touch and both logical players work through the game's menus. |
| Creator automation | Local authenticated inspection, bounded named operations and repeatable projects | Tools can inspect, drive and diagnose mods without arbitrary memory writes, script evaluation or game-window guessing. |

## Game adapters and authoring

| Area | Required delivery | Acceptance |
| --- | --- | --- |
| Players and entities | Named queries, lifecycle events, transforms, components and owned spawns | Handles expire safely on unload, deletion, reload and restore; spawned objects clean up through proven game routines. |
| Combat and AI | Health, damage, abilities, targeting, behavior and encounter extension points | A creator acceptance mod changes a complete encounter with correct co-op, save and unload behavior. |
| Missions and progression | Objectives, events, ScoreKeeper, unlocks, checkpoints and game flow | A source-commented mission project can start, complete, fail, restart and restore without hardcoded recomp addresses. |
| Cameras and cinematics | Complete camera state, tracks, sequencing and cutscene hooks | Playback, cancellation, checkpoints, exact restore and returning to the gameplay camera are qualified. |
| Visual editors | In-game layout, object, mission and cinematic editing | Editable projects round-trip through the public kit and stay in the game's original UI. |
| Debug library | Categorized whole-game catalog, search, paging and source identities | All installed resources are indexed; actual cross-level use follows verified dependencies and initialization. |
| Resource streaming | Dependency-aware cross-level loading, spawning and retirement | A resource from another level loads, behaves correctly, releases safely and restores from a save. |

## Assets and translations

| Area | Required delivery | Acceptance |
| --- | --- | --- |
| Container tools | Lossless STR/RefPack inspection, rebuilds, merged catalog corrections and provenance | Native decoder comparisons, changed sizes, compression, catalog/dependency layout changes and native loader projects agree. |
| Typed assets | Meshes, rigs, animations, collision, navigation and worlds | Each type has documented bounds, a supported writer and a native loader round-trip acceptance project. |
| Presentation extensions | Audio, textures, materials and renderer extensions | Projects work across supported renderers and platforms with explicit ownership, save and unload behavior. |
| Translation workflow | Searchable source catalogs, editor, validation, progress, partial language packs and translator credit | Placeholders and dynamic button tags survive; stale sources are detected; the native text lookup displays translations. |
| Fonts and layout | Font import, Unicode coverage, right-to-left text and text fitting | Acceptance languages render correctly in menus, gameplay and cutscenes on desktop and Android. |

## Bundled example and delivery

Bundle commented starter projects for native menus, logical-player actions, simulation
timers, scenes, missions and the supported asset workflows. Each project starts disabled,
uses named public APIs and includes editable locale catalogs. A translator can create a
partial language pack from the installed game or another mod through the desktop editor,
validate it and export a credited ZIP for desktop and Android. The original slot menus
remain active while the separate save-library experiment is shelved.

Qualification covers Linux, Steam Deck desktop and Gaming Mode, native Windows and Pixel
hardware. Cross-compilation and Proton checks are useful evidence and are recorded separately
from native Windows and Android device runs. Creator projects must cover the systems above,
dependency failures, overlapping mods, bad packages, slow storage, hot reload, co-op, level
changes, checkpoints, exact restore and shutdown.

Finish and document these checks before staging the full kit, migrating the save example,
or delivering a release package as complete. Preserve existing source work, installed builds,
real saves and the workspace's 25 GiB free-space reserve throughout development.
