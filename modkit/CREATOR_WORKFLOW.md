# Make a mod with the desktop editor

The separate Windows/Linux **Modkit** application has **File → New project**. It uses
the same project format, commented scripts and package validation as the creator CLI.
A current game build with Modkit support is required for Test.

## Create and edit

1. Open **File → New project**. Choose **Gameplay tools**, **There and back**,
   **Mission editor**, **Input observer**, **Owned objects**, **Library objects**, **Combat workshop**,
   **Camera workshop**, **Scene editor**, or **File replacements**.
2. Enter a name, a stable id such as yourname.my-route, your creator credit and a
   description. The id identifies saved mod progress; keep it when updating the same mod.
3. Create the project. It starts disabled and does not change the installed mod profile.
4. Use **Project files** to open the commented Lua, language catalog, README or mod.json.
   **Save source** writes your edits. The editor refuses to overwrite a file that
   another editor changed after you opened it; reopen that file before continuing.

Gameplay tools adds a page in the original Options movie with health, protection and
camera controls. There and back is an authored route with dependent objectives, a time
limit, failure, an objective retry point and saved progress. Read the [mission guide](MISSIONS.md)
before changing its rules. File replacements starts with an empty gamedata folder.
Copy only the edited resources your mod needs, using paths matching your own installed game.
The [asset tools](README.md#asset-library) cover container inspection and rebuilding.

**Mission editor** authors reach, continuous stay and wait objectives in the native
menu, alongside the shared prop Scene editor. Save both drafts, close Test, then
choose **Apply saved drafts**. **Mission settings** offers a guided form for
titles, rules and prerequisites; **Edit JSON** opens the same source document.
Read the [editor guide](MISSION_EDITOR.md) for retry, pause and package behavior.

Camera workshop captures and previews native camera poses, changes lenses and releases
only its camera request. It uses the original Options menu and keeps captures through
code reload in the same scene. Read the [camera guide](CAMERAS.md) for coordinate,
ownership, lens and checkpoint rules.

**Scene editor** authors a prop scene through the game's Options menu: position,
rotation, scale, duplication and undo/redo. Use **Save scene draft**, wait for
confirmation and close Test. **Apply scene draft** copies that test's validated
document into mod.json for review and export, preserving your creator credit.
A draft made before source changes is rejected; re-test the new source first.
See [scene authoring](SCENES.md) for original-resource identity, autoload and limits.

The source editor supports existing UTF-8 Lua, JSON, Markdown, text, configuration and
XML files up to 128 KiB. Character, map, mission and cinematic editors remain
in development. Desktop projects can also be opened in an external
editor at the displayed folder. Lua projects include local completion definitions for
the named game APIs.

## Validate and test

**Validate** checks the package's metadata, files, checksums, dependencies and load order.
It reports overlapping file replacements. This does not prove Lua behavior, resource
compatibility or game correctness; use **Test** for that.

**Test** starts the project and its declared dependencies with a private profile and a
separate progress folder. Other selected mods and normal progress stay intact. The first
test starts with fresh progress. Later tests of the same id retain that test progress.
For scripted route projects, enter a playable level and use **Options → Extras → Mods**
to start the route. Testing does not automatically complete the campaign or start a mission.

Close the game before using the launcher source editor again. Code reload during play is
also available through the [creator bridge](README.md#timers-and-creator-inspection);
changing file replacements or translations needs a restart. A failed code replacement
keeps the running instance.

Native mod pages refresh when background work changes their rows or titles.
Editing a value keeps that control selected; a page that becomes shorter
returns to a valid page. Unchanged pages keep their native animations running.

Generated projects declare the operations and events their templates use. The native
validator checks these requirements before loading callbacks. Desktop Test also checks
the selected build's capability descriptor, so an older runtime produces an update
message rather than trying to run unsupported mission APIs.

The manifest fields requires_operations and requires_events are arrays of public API
names. Keep them up to date when adding a new operation or handler to your project.
They describe requirements, not permission to call APIs outside registered callbacks.

## Export and install

Save your edits, update the version and description in mod.json, then choose **Export ZIP**.
Desktop writes the package to the mods folder's exports directory and shows its location.
Android opens the document picker so you can save directly to Downloads or another provider.
The package contains public source and creator credit, with a checksummed inventory.
Local editor metadata and completion caches stay outside the package.

Another player installs that ZIP through **Mods → Install mod from ZIP**. It stays disabled
until they choose Enable. They see the creator, version and description; dependencies are
resolved before play. Share your own changes and required resources. Do not redistribute
default.xex, an extracted game, or unmodified game archives.

## Development checks

Regenerate the launcher bundle after changing either template or the generated Lua reference:

    python3 tools/modkit/generate_templates.py
    python3 tools/modkit/generate_templates.py --check

The source templates remain in modkit/examples. The desktop editor and CLI consume the
same generated bundle; they do not keep separate copies of the example scripts.
The Input observer template demonstrates independent P1/P2 buttons and analog controls
through `game.input.state` and finds each player's current family character through
`game.players.controlled`, with a native Options page and no gameplay changes.
Generated Lua completion includes typed players and snapshots. As with other projects, creation is disabled
until the creator chooses to test or enable it.

The UI smoke check uses QtWebEngine and an empty temporary install. It creates a mission,
edits source and metadata, validates, exports and imports it, checks keyboard dropdown
navigation, and captures desktop and narrow layouts without starting a game:

    python3 tools/bench/modkit_launcher_check.py --output /path/to/private-check

Pixel WebView, Android document providers, native Windows and game behavior still need
their own qualification. This check does not substitute for those runs. The
[full suite](PLAN.md) remains the release gate, including visual editors, typed assets,
world streaming and encounter integration. The save-system example is shelved; use the
small starter projects and the [language-pack workspace](TRANSLATIONS.md) first.

**Combat workshop** adds a commented encounter project through the same desktop
and Android source tools. It combines owned loaded brawlers, original health
operations, native death events, objectives, failure, retry and code reload.
See [combat authoring](COMBAT.md) for supported behavior and remaining campaign,
AI, collision and device qualification. Required named calls inside
`system.try_call` are included in project capabilities so older builds cannot
silently offer an incompatible template.

The [editor workspace guide](EDITOR_WORKFLOW.md) covers dock panels, the real game viewport, live assets, source shortcuts and offline help. Android imports/enables mod ZIPs; it does not include creator tools.
