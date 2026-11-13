# Simpsons Modkit

The goal is a complete creator workflow for gameplay mods, missions, characters, maps,
assets and translations: create a project, edit it, test it and share it through the
desktop Modkit application. The [creator experience standard](PLAN.md#creator-experience-and-completion-standard)
defines that delivery; parts of it are still under development.

The modkit gives mods named game operations instead of recompiled function addresses.
C++ plugins and Lua scripts use the same game-thread adapters. Mod settings pages use
the original Options movie, including its controller, keyboard, mouse and touch input.

The [loaded-object guide](ENTITIES.md) covers mod-owned spawns, world transforms,
placement, removal, reload transfer and the **Owned objects** creator project
shared by the desktop editor and CLI.
The [Scene editor](SCENES.md) adds an in-game prop authoring project with
placement, rotation, scale, undo/redo and portable scene drafts. Apply a tested
draft in Modkit, then export it with your creator credit.
The [Mission editor](MISSION_EDITOR.md) adds reach, continuous stay and wait
objectives, prerequisites, retry points and simulation-time rules. Its shared
Scene editor saves both documents together; the desktop Inspector also offers a guided
mission form for names and rules.
Owned objects also have native visibility queries and idempotent hide/reveal
requests, preserving authored hide requests and transferring across code reload.

This is development work. The full suite is a release requirement. New level and character
asset authoring, AI/combat extension points, the remaining visual editors and complete platform
qualification are still being implemented. The API catalog lists implemented operations;
it does not advertise those unfinished systems. The [implementation plan](PLAN.md) keeps the
full release scope and acceptance requirements together.

## Start a mod

Open the separate desktop **Modkit** application and choose **File → New project**. Choose a commented gameplay,
mission or file-replacement project, enter your creator credit, then use the source
editor, Validate, Test and Export ZIP. Test keeps separate progress and leaves the normal
profile unchanged. The [workspace guide](EDITOR_WORKFLOW.md) covers the native editor, viewport, assets and built-in help. Android imports/manages packages; authoring stays on Windows/Linux.

The same templates are available from the repository root:

```sh
python3 tools/modkit/modkit.py new "Health tools" --id yourname.health-tools --author "Your name"
python3 tools/modkit/modkit.py validate yourname.health-tools
```

The Lua example registers a native menu in **Options → Extras → Mods**. It refills health,
changes damage protection and enables the free camera through the existing game code.
Game operations are available from registered callbacks. See `api.json` and the generated
reference for names and arguments.

```lua
game.on("level.ready", function()
    for _, player in ipairs(game.players.list()) do
        game.log.info(player.character)
    end
end)
```

For C++, create a project with `--kind native`, then run `build <id>`. CMake builds the plugin
against the public SDK without compiling the game. `--cmake-arg` passes compiler/toolchain
settings and `--target` selects `windows-x64`, `linux-x64` or `android-arm64`. A package needs
a matching native binary on each platform where it runs. Lua projects use the same source
across platforms.

## Author a mission

Use `new "My route" --template mission-walk --id yourname.route --author "Your name"`
for a commented mission with native menu controls, objective dependencies, failure, retry
points and saved progress. It uses player-position queries and simulation time. The
[mission guide](MISSIONS.md) describes Lua/C++ operations, ownership, saves and reload migrations.
The [combat guide](COMBAT.md) adds original damage/healing operations and owned
native death events. `--template combat-workshop` creates a commented encounter
workshop with targets, objectives, visibility controls, failure, retry and reload rebinding. Campaign
integration, mission HUD work and broader AI authoring remain in the full plan.

## Find the game code behind a feature

The creator CLI shares the existing codegraph index. It scans generated functions and
links their callers, hand-written adapters, reverse-engineering notes and reviewed
descriptive names. No game window, installed game data or compiler is needed.

```sh
python3 tools/modkit/modkit.py code build
python3 tools/modkit/modkit.py code search health
python3 tools/modkit/modkit.py code show ScoreKeeper.GetScore
python3 tools/modkit/modkit.py code callers ScoreKeeper.GetScore
```

Use the linked public operations when writing a mod. A research name describes an internal
function; it does not provide a safe public calling signature. The address symbols stay
intact through regeneration. See [code navigation](CODE_NAVIGATION.md) for source checks,
adding reviewed names and extending the public APIs.

## Install and test

`pack <id>` produces a ZIP with a SHA-256 file inventory. `install <zip>` validates and
extracts it into a new folder, disabled. `enable <id>` checks dependencies and conflicts.
The first requested mod has the highest file priority; dependencies initialize first and
have lower asset priority than their dependents. The runtime merges file lookup and directory
enumeration, including added files. Original game files are not overwritten.

Use `run <id> --exe /absolute/path/to/simpsons --lib-dir /absolute/path/to/runtime` for an
isolated test. It creates separate userdata and prints its location. A test run never uses
your real saves. `--gamedata` selects your own installed game files.

The launcher also imports packages and changes the active profile while the game is closed.
Older game builds cannot enable mods; a build with the mod runtime supplies
`simpsons-modkit.json`. Android uses the same Mods tab, with a system ZIP picker,
background installation, notifications, progress and cancellation. Device qualification is
still pending.

## Translations

Translators can use the local editor without writing Lua or C++. It has a source column,
translation column, search across every table, untranslated filter and progress count. It saves UTF-8 JSON in
the project's `.translations` folder. Keep that folder with the project's source; working
catalogs are excluded from installed ZIPs.

To translate a mod that supplies a source catalog:

```sh
python3 tools/modkit/modkit.py translate new "French health tools" --of yourname.health-tools --locale fr --author "Your name"
python3 tools/modkit/modkit.py translate edit "French health tools"
python3 tools/modkit/modkit.py translate check "French health tools"
python3 tools/modkit/modkit.py translate build "French health tools"
python3 tools/modkit/modkit.py pack "French health tools"
```

The resulting language pack depends on the original mod and credits its translator. Install
it through the normal desktop or Android Mods tab. **Mod text language** follows the game's
language unless a code such as `fr` or `pt-BR` is entered. Missing regional translations fall
back to the parent language, then the mod's source language. Missing keys show their key.

Mod authors put text in `locales/en.json`, declare it in `mod.json`, and use stable keys:

```json
{
  "default_locale": "en",
  "locales": {"en": "locales/en.json", "fr": "locales/fr.json"}
}
```

The catalog is a flat JSON object, for example `{"health.refill": "Refill health"}`.
Use `game.locale.text("health.refill")` in Lua or `game.Text("health.refill")` in C++.
Named values work as `game.locale.text("welcome", {name = "Lisa"})` for `"Hello {name}"`.
Catalogs load before callbacks, so looking up text does not read from disk during a frame.

Game text uses the same editor. Build the creator tools once, then pass their executable:

```sh
cmake -S modkit -B /path/to/modkit-build
cmake --build /path/to/modkit-build -j2 --target modkit_streams
python3 tools/modkit/modkit.py translate --stream-tool /path/to/modkit-build/modkit_streams new "My game translation" --locale fr --author "Your name"
python3 tools/modkit/modkit.py translate edit "My game translation"
python3 tools/modkit/modkit.py translate check "My game translation"
python3 tools/modkit/modkit.py translate --stream-tool /path/to/modkit-build/modkit_streams build "My game translation"
```

The exporter reads your installed archives, including embedded dialogue and in-game cutscene
text. Identical tables are edited once. Building emits a small `locales/game-text.json` catalog;
the game applies it through the original text lookup. Game archives and dialogue identifiers
remain intact. Once a project contains its source identities, building it needs neither the
stream tool nor the original game files. The stream tool is needed for the initial export.
Use the pack's base game language (English by default) when playing it. Embedded table export
currently targets English game data; `--base` selects other named global tables in your copy.

Each table is matched by its original content hash after normalizing runtime pointers.
Each translated line also checks the source string. Unmatched content keeps its original text.
Translation strings are allocated before play and retained with native resources; lookups
perform no filesystem I/O. `text.lookup` reads a loaded game text ID for inspection or a mod
that needs the game's text. It reports `found=false` for unloaded tables.

Button tags (`[RDDown]`), numbered values (`%1`), printf placeholders and named values must
survive translation. Check reports missing or changed tags before a build. Narrow game
tables use Windows-1252; characters needing a different font are reported, never silently
changed to question marks. Unicode font import, right-to-left layout and complete font
coverage remain part of the unfinished asset suite.

## Asset library

The debug menu's **Game-wide library**, also available from Spawner → Entire game, indexes
all installed episodes and enabled mod files. It groups characters, models, animations,
effects, textures, worlds, physics, audio, text, movies and other resources. Named object
definitions link to their models, character banks and mesh parts before spawning. A worker starts
when the library is first requested. Search, initial-letter filters, paging and source
locations help inspect large categories without building thousands of menu rows per frame.

`assets.search` exposes the same bounded queries to creator scripts. Check `ready` while
indexing. Friendly names retain their original artist names and source paths in the details.
Open a linked object and choose **Load and spawn** to use its original source data.
Resource identities describe archive content; they are not live entity handles.
The spawner uses validated live objects and retained definitions from fully loaded native
streams. Destroying the original object need not remove its template. It keeps distinct
behavior records separate and discovers additional placeable classes through the native
registry; extra categories appear automatically. **Spawner → Objects from other levels**
is an experimental native source loader: choose an episode archive, wait for its catalog
and dependencies, then use the regular spawner categories. Up to eight archives can stay
loaded in one retained session. Additions are serialized and preserve earlier objects;
a failed or canceled addition removes only its new native files. Mods share the session
through `assets.load_source` and `assets.source_status`; `assets.source_streams` lists its
accepted archives, and `assets.templates` supplies current spawn handles once ready.
Spawning from a retained source automatically acquires a lease for the spawning mod.
`assets.cancel_source` withdraws a pending addition while preserving an earlier lease.
The commented **Library objects** creator project demonstrates this path. Require its
new operations in your manifest so older hosts reject unsupported projects clearly.
Each owner releases only its lease; all accepted archives and dependencies stay together
until the last release. The original loader then removes their copies and files while
preserving other level files, including later streamed files. Exact saves retain every
archive's load identities and validate mod claims before restoring guest memory. Legacy
single-source exact saves remain readable.
This does not establish scene-specific AI or boss behavior for every class;
see [object ownership](ENTITIES.md).

Owned objects also have full world-pose authoring through
`entities.set_transform`: position, orientation and scale use the original
class matrix setter, including cached scale and model/controller notifications.
The **Owned objects** example includes native menu actions for moving, rotating,
growing and shrinking a prop. See [transforms](ENTITIES.md#transforms) for parent
conversion, supported inputs and collision/script limits.

An optional native reference check validates compression against the game's own generated
decoder, with no game window or graphics backend:

```sh
cmake -S modkit -B /path/to/modkit-build -DCMAKE_BUILD_TYPE=Release -DSIMPSONS_MODKIT_REFERENCE_CHECKS=ON
cmake --build /path/to/modkit-build -j2 --target modkit_refpack_check
/path/to/modkit-build/modkit_refpack_check --self-test /path/to/self-test.json
/path/to/modkit-build/modkit_refpack_check /path/to/gamedata /path/to/game-streams.json
```

A single STR file can be supplied instead of the game folder. Each compressed entry is
decoded with both implementations, re-encoded, and checked again through the original
whole-buffer and asynchronous game routines. The step checks compare output, cursor
updates, command boundaries, return values, saved registers and untouched memory. Zero-byte,
small, block-sized and live-loader requests are included. Corrupt-input fixtures also check
that a rejected native step restores its buffers and registers before falling back.
Reports include the reference source identities, entry counts, compared bytes and timings.
Those timings exclude guest heap checks and do not measure frame rate.
Re-encoding uses bounded reference matching and calculates the input headroom required
for decoding in place. It also reports the original and re-encoded storage sizes.
The tool uses temporary memory, reads the source files and writes the chosen JSON report.
Add `--decode-only` after the report path to compare installed streams without running
the encoder. Keep the complete check for qualifying an encoder or changed resource writer.

Rebuilt STR files can need larger buffers even when their decoded content is unchanged.
The game's StreamTOC catalogs reserve those buffers before reading a child archive's header.
The merged mod filesystem checks the winning STR headers at startup and corrects the
catalog's allocation and pool fields on reads. This also accounts for overlapping mods;
original archives and installed packages stay untouched. Missing catalog dependencies,
changed entry counts, unknown pools and allocations beyond the catalog format fail before
the game starts. Changed compressed catalogs need a separate rebuilder.

Inspect those corrections without starting the game:

```sh
/path/to/modkit-build/modkit_streams catalogs /path/to/gamedata /path/to/catalog-report.json --overlay /path/to/your-mod/gamedata
```

Additional `--overlay` folders use highest-to-lowest priority. Private Linux runs qualify
recompressed global text and a resized LH2 edit through the native loader. This does not
qualify arbitrary meshes, dependencies or every edited resource type. Those typed asset
pipelines remain under development.

The [model tools](MODELS.md) export native MMdl definitions as readable parts,
properties, states and references. They build source-checked ModelPart variants
into small credited packages, preserving original resource ownership and the
existing dependency set. The [mesh attribute tools](MESHES.md) export supported
geometry as OBJ and package bounded vertex, normal and UV edits through the
native loader. Reviewed character UV streams can change while their skinning,
positions and normals stay intact. Topology, materials and resource ownership
are preserved. New topology,
rig import and live device qualification remain in the full kit gate.

## Lifetimes and saves

Game handles are temporary identities, not addresses. Reacquire them after a level unload or
save restore. `players.list` currently enumerates loaded family entities; its `slot` is a list
index, not a controller assignment. `input.players` describes actual input assignments.

## Reading controls

`game.input.state(0)` and `game.input.state(1)` read the controls the game received for
P1 and P2. Keyboard, gamepad and touch inputs follow the existing player assignments.
Each mod sees the same snapshot for a modkit frame; reads do not poll devices or consume
mouse movement. Use `pressed` for a single action and `buttons` for a held control:

```lua
game.on("simulation", function()
    local input = game.input.state(0)
    if input.sampled and input.available and input.pressed.x then
        -- Run your P1 action once for this press.
    end
end)
```

Face names are logical controls, regardless of the glyphs on a particular controller.
Sticks range from -1 to 1 with positive Y upwards; analog triggers range from 0 to 1.
Trigger button flags become held above 30/255. A quick tap between updates can set both
`pressed` and `released`. First samples, reconnects and restored timelines establish a
baseline without inventing presses. A failed poll releases the old held controls.

`sampled` says the game has polled that port since the last reset. `updated` says a poll
arrived this frame. `available` is that poll's result, rather than a physical hotplug
query: a port the game is not polling retains its last values and clears its old edges.
Snapshots may reflect the preceding game poll. Read gameplay actions in `simulation`;
use `ui.action` for native menus. Never store transient input or edges in saved state.

`input.players` also reports device identity, name and kind. Its existing `prompts` field
keeps the configured override, such as `auto`; `prompt_family` resolves that override
to Xbox, PlayStation, Nintendo, Steam Deck or keyboard. These records do not associate
a loaded family entity with a controller. Use `players.controlled(0)` or
`players.controlled(1)` for the currently bound family character, or inspect
`input_player` in `players.list()`. AI, unbound and ambiguous actors have no input
owner. Requery during simulation when characters switch or co-op players join/drop
out; the old handle may remain a valid AI actor. A paused or temporarily blocked
input does not remove its assignment. C++ mods use `Game::ControlledPlayer(player)`,
`Game::Input(player)` and `Game::InputPlayers()`. The generated Lua reference includes
typed player records, snapshot fields and
editor completion. See the [Input observer](examples/input-observer/main.lua) template
for a commented P1/P2 reader and native Options page. New launcher projects stay disabled.

The game adapter observes `sub_82B766A8`, the original XInputGetState wrapper, after its
single original call and the existing camera/menu filters. A scope in the existing
input hook covers its early returns. An offline check compares every guest register and
memory byte with the unchanged reader and equivalent final filters, including failed/null
reads, synthesized menu input, both ports, exception unwinding and resets during a poll.
The ownership reader uses the character's human-control marker and actual input
binding, independently of family order and viewport. Its fixture compares those
reads with the original human-control and button-dispatch routines. These shared
host services are independent of the rendering backend; compilation
and live platform qualification are recorded separately in `coverage.json`.

## Persistent mod state

Store persistent Lua values in `game.state`. State must be JSON-compatible: string keys,
finite numbers, booleans, strings and contiguous arrays. Functions, game handles, threads and
userdata do not belong in saved state. Do not keep a local reference to the state table across
restores; read `game.state` inside callbacks.

`save_mode` is `state`, `stateless` or `checkpoint`. Stateful native mods supply both serializer
callbacks and keep instance data in their plugin context. Initialization must register services
and menus without changing gameplay. Restore preflight recreates mod instances, initializes
their menus, and loads state before the game memory changes. Gameplay changes belong in
`level.ready`, `simulation`, `ui.action` or `save.restored` callbacks.

Exact saves require an identical ordered mod profile and content fingerprints. Preflight
failure keeps the running mod instances. Checkpoints and autosaves store mod state alongside
the game's slot files; checkpoint-only mods produce checkpoint saves. `save.restored` includes
`kind` (`exact` or `checkpoint`). Reacquire level objects in `level.ready` after a checkpoint.

Source verification runs on workers for both checkpoint and exact loads. Checkpoint
preparation captures its mod sidecar with the verified code, then initializes callbacks
on the game thread before replacing the working slot. Loading an older vanilla checkpoint
without a sidecar initializes fresh mod state. Invalid JSON, mismatched profiles and changed
source reject the load before working-slot replacement, keeping the running mods intact. The native
save menu keeps the load error visible when it reopens. Large-profile callback staging still
needs a separate frame budget; background file verification does not complete that work.

Damage protection and camera requests belong to the calling mod and runtime generation.
The latest active request wins. If it retires, the preceding owner's request becomes active;
when the last owner retires, the original manual value returns. Manual controls supersede
older mod requests, including a manual request for the same value. A failed mod loses its
pages, timers and requests without retiring another mod. Saved ownership includes request
order and baseline values, including authored poses and captured manual camera views.
The [camera guide](CAMERAS.md) covers pose/lens editing and the shared native-menu
Camera workshop. Checkpoints discard old-world camera requests; full-scene exact
camera restoration and cinematic tracks still need qualification.

Desktop and Android launchers give each ordered mod combination its own progress folder.
The first launch copies vanilla checkpoint progress into that folder; snapshots are not copied.
Disabling all mods returns to vanilla progress. Changing mod code or priority selects a separate
playthrough. These save paths are still under live qualification; this development kit is not
release ready.

## Script environment

Lua 5.4.9 is compiled into the game. Each mod has its own state, a 32 MiB allocation limit,
and bounded callbacks. Standard table, string, math and UTF-8 functions are available.
`require("helpers.combat")` loads this mod's `helpers/combat.lua` during initialization.
Register handlers with `game.on`; supported events are listed in the generated completion file.
Lua entrypoints and modules are verified and copied into an immutable source set before the
first game-thread callback. `require` reads that memory, not a file during play. Source is
limited to 32 MiB per mod, with at most 4 MiB in one file. Edits on disk take effect on a new
launch; exact/checkpoint restore still checks the source fingerprint before accepting it.

File/process/debug libraries, dynamic Lua loading, `__gc` finalizers and nested protected calls
are not exposed. Lua disables instruction hooks in finalizers, so they cannot meet callback limits.
The host owns protected entrypoints so script errors and budget interrupts can end a callback.
Use `game.system.try_call("world.spawn", {template = id, position = p})` for an
expected host rejection. It returns `{ok = true, value = ...}` or
`{ok = false, code = ..., error = ...}` while preserving the target's argument,
ownership, save and preflight rules. It cannot run a script function or catch a
callback timeout. C++ plugins can use `Game::TryCall` or catch `Error` from `Call`.

Persistent JSON storage is namespaced by mod ID. It is loaded before callbacks, read from memory,
and written atomically by a background worker. Reads see a queued write immediately. Check
`storage.status()` for pending writes and disk errors; shutdown drains accepted writes. Limits
are 1 MiB per file, 256 files and 16 MiB per mod, and 64 MiB across active mods. Store settings
here; put playthrough progress in `game.state` so loading a save restores it too.

Native plugins execute as native code in the game process and must catch their own exceptions
before returning through the C ABI. Their shutdown callback must finish their own threads and
release resources before returning. Save callbacks may inspect state but cannot change gameplay,
storage, pages or timers. Native images are limited to 64 MiB per library and 256 MiB per mod.

## Timers and creator inspection

Use simulation timers for cooldowns and scripted sequences rather than counting display
frames or reading a wall clock. Pausing stops them; changing the display frame rate does not
speed them up. Each mod owns its timer ids. Reusing an id replaces its pending event.

```lua
game.on("initialize", function()
    game.timers.schedule("reminder", 5, 0, {message = "Ready"})
end)

game.on("timer", function(event)
    if event.id == "reminder" then game.log.info(event.data.message) end
end)
```

An optional positive interval repeats a timer. A slow frame delivers one event and reports
the number of skipped intervals; it does not run a catch-up loop. Dispatch has a shared frame
budget and rotates through overdue timers. Timers keep their remaining delay and JSON data
in exact saves and checkpoints. `timers.cancel(id)` removes a timer; `timers.list()` inspects
this mod's pending work. Limits are 64 timers per mod, 1024 overall, 64 KiB of data per timer,
and delays up to a day. Native plugins use `Game::ScheduleTimer` and `Game::CancelTimer`.

`run --developer` enables the local creator bridge in an isolated test. The game log prints
its `session.json` path. Ordinary play does not create a session or poll for requests.
Use that path while the game runs:

```sh
python3 tools/modkit/modkit.py dev --session /path/to/session.json status
python3 tools/modkit/modkit.py dev --session /path/to/session.json call players.list
python3 tools/modkit/modkit.py dev --session /path/to/session.json state yourname.health-tools
python3 tools/modkit/modkit.py dev --session /path/to/session.json menu yourname.health-tools:tools heal --player 1
```

Status includes menu row identities, creator credits, callback cost, storage errors, pending
timers, owned host resources and a bounded lifecycle/event log. `call` accepts `--arguments` as a JSON object; game
mutations and namespaced operations require `--owner` with an active mod id. Menu activation
uses the same validation and logical-player attribution as actual input. `wait` waits for a
loaded level; `--timeout` accepts up to 30 seconds. `status --after N` filters the event log.

Use `source` for object loading without constructing a JSON call:

```sh
python3 tools/modkit/modkit.py dev --session /path/to/session.json --timeout 30 source load brt/brt/challenge_mode/challenge_mode_design.str --owner yourname.library-objects --wait
python3 tools/modkit/modkit.py dev --session /path/to/session.json source status --owner yourname.library-objects
python3 tools/modkit/modkit.py dev --session /path/to/session.json --timeout 30 source wait --owner yourname.library-objects
python3 tools/modkit/modkit.py dev --session /path/to/session.json source cancel --owner yourname.library-objects
python3 tools/modkit/modkit.py dev --session /path/to/session.json source release --owner yourname.library-objects
```

Use a source path from `assets.templates` and the active mod's actual ID. `load`
requests once; `--wait` prints native file progress to stderr and leaves the final
JSON result on stdout. A timed-out wait leaves accepted loading in progress;
inspect or resume waiting instead of submitting another load. `release` returns
whether that mod released a claim and its current source status. Native cleanup
is asynchronous, and other owners can keep the group loaded.

The worker handles file I/O; the game thread executes named operations. Requests are bounded,
authenticated to one local session and expire if the game cannot service them. A repeated
request id never repeats an action. The CLI does not retry mutations after a timeout. Session
credentials remain in the private userdata folder, and the descriptor is retired at shutdown.
The bridge does not expose arbitrary Lua evaluation, game memory writes or a network listener.

### Reload code

In a creator session, edit Lua source or rebuild a native plugin, then run:

```sh
python3 tools/modkit/modkit.py dev --session /path/to/session.json reload
python3 tools/modkit/modkit.py dev --session /path/to/session.json reload --cancel
```

File hashes and code copies are prepared on a worker while the game continues. The game
thread initializes replacement instances and loads their JSON state before retiring the
running code. Invalid code or staged initialization that changes gameplay fails preflight
and leaves the running instances in place. Native plugins load verified private images so
their source DLL can be rebuilt on Windows while the original instance is running.

Reload preserves remaining timer delays and owned host requests. `code.reloaded` runs after
commit, with `level_ready` and a new runtime epoch. Reacquire temporary game handles there
and refresh menu values loaded from `game.state`. A failed mod can recover its JSON state
when serialization succeeded before retirement; the host refuses to silently discard state
it could not capture. Native plugins must keep instance state in their context and shut down
their own workers before returning from their shutdown callback.

Use `state.loaded` to validate saved fields, add new defaults or migrate your state schema
before commit. Its `kind` is `reload`, `exact` or `checkpoint`. This callback can update
`game.state`, staged menus and this instance's timers; it cannot change gameplay or persistent
storage. An error rejects the replacement while the old state and pages remain active.

Reload covers code in the current ordered profile. Changed metadata, dependencies, translations
or game data require a restart so loaded native resources stay consistent. Installed packages
with checksum inventories must still pass those checks; edit a creator project for development.
Status reports preparation progress and the last result. A CLI timeout leaves an accepted job
visible in status; it never retries the reload automatically. Full resource remounting and
dependency-aware streaming remain separate work.

Exact-save reads and the native checkpoint load flow verify source files in background
workers, then use immutable prepared code during game-thread preflight. Checkpoint sidecars
are also captured by the worker. Cancellation and failed preflight preserve the current mod
instances; the working slot changes only after the required preparation succeeds.

The save-library experiment is shelved. The original game save menus remain active and
existing library files are preserved. Start with the commented Script starter, Player
actions or Timed events projects instead. See [getting started](GETTING_STARTED.md) and
[language packs](TRANSLATIONS.md) for complete creation and export steps.

## Development checks

```sh
cmake -S modkit -B /path/to/modkit-build -DSIMPSONS_MODKIT_TESTS=ON
cmake --build /path/to/modkit-build -j2
ctest --test-dir /path/to/modkit-build --output-on-failure
python3 modkit/tests/test_packages.py
python3 modkit/tests/test_translations.py
python3 modkit/tests/test_developer.py
```

Set `MODKIT_CHECK_EXE` to the built `modkit_check` when running the Python checks to compare
content fingerprints across both implementations. Tests use synthetic content and private
temporary folders. Live renderer and device checks are recorded separately.

`tools/modkit/generate_api.py --output <directory>` generates the embedded API catalog,
Lua completion and Markdown reference from the same definitions.

Lua's copyright and MIT license are retained under `thirdparty/lua`. The JSON and SHA-256
implementations use the existing SDK dependencies and retain their respective licenses.
