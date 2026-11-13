# Find and extend a game system

The generated C++ already executes natively. Its register operations and address-based
names come from mechanical PowerPC translation. A descriptive name helps navigation;
it does not recover an original class layout, function signature or source implementation.
The modkit's named APIs supply the usable interface for modders.

## Search without running the game

From the source checkout:

```sh
python3 tools/modkit/modkit.py code build
python3 tools/modkit/modkit.py code status
python3 tools/modkit/modkit.py code search health
python3 tools/modkit/modkit.py code show ScoreKeeper.GetScore
python3 tools/modkit/modkit.py code callers ScoreKeeper.GetScore
python3 tools/modkit/modkit.py code callees sub_828A31A8
```

The same index is used by `tools/codegraph/recomp_index.py`. To keep it elsewhere,
put `--db /path/to/code.sqlite` immediately after `code` on each command. Building
with `--source /path/to/checkout` selects another source tree. Queries return JSON
for use in editors and creator scripts. `--limit` bounds a result page; `more`
reports truncation.

`show` accepts a readable name, a `sub_XXXXXXXX` symbol or an exact hexadecimal entry
address. It returns the definition's path and lines, a body hash, indirect-call count,
source references and any reviewed alias. Aliases include their evidence and related
public API names. References cover the hand-written game adapters and research notes.
They help explain the code; the catalog does not claim those notes prove every use.

The index distinguishes direct and original-implementation calls, keeps recursion,
and records each call site's line. Comments and diagnostic string literals are excluded.
Indirect calls are counted separately: function pointers, vtables and runtime dispatch
need additional analysis. An empty direct-caller list does not mean a function is unused.
`gap` shows neighboring starts; it cannot establish function extents.

The registration audit compares address functions with the generated entrypoint table.
Named runtime imports and compiler helpers are counted separately. Missing definitions,
unmapped definitions and mismatched addresses are explicit. These are code-navigation
checks, not proof of campaign correctness or renderer coverage.

## Keep results current

Inputs include generated sources, game adapters, research notes, the API catalog,
the readable-name registry and the indexer itself. SHA-256 inventories detect changed
contents, added files and deleted files, including edits that preserve a timestamp.
Queries refuse stale results. Run `code build` again after editing those inputs.
An unchanged build reuses the database.

Rebuilding uses a private temporary database and checks the inputs again before replacing
the old one. A failed or interrupted build preserves the previous index. The index is
reproducible and does not belong in a mod package. Building and querying do not create
a mod profile or touch saves.

## Add a readable name

`symbols.json` stores reviewed descriptive names. Each entry supplies:

- The stable address symbol and an unambiguous readable name.
- The game system and a concrete description of the behavior.
- The reviewed definition hash from `code show`.
- Source-relative evidence paths with exact quoted passages.
- Related implemented operation names from `api.json`, when available.

Use descriptions such as `ScoreKeeper.GetScore` to make research searchable. Preserve
the original symbol in generated code, hook declarations and dispatch tables. Do not
automatically copy a caller's name onto its callees or infer a class from neighboring
addresses. Library matches, strings, script registrations and vtables can provide leads;
verify their relationship before publishing a descriptive name or typed adapter.

The tool rejects duplicate names, missing definitions, unknown API links and evidence
paths outside the checkout. If a definition or quoted passage changes, the rebuilt entry
is marked `needs_review`. Review the implementation and callers before updating its hash
or evidence. A matching hash means the reviewed text is unchanged; it is not an independent
verification of the behavior.

The initial registry covers progression, menus, entities, player damage, characters,
resources, text, input, simulation and rendering. Unnamed functions remain discoverable
by address, direct calls and source references. They have not all been reconstructed into
high-level source.

## Turn research into a mod API

Start with `api.json` and the generated reference. Extend an existing operation when its
contract fits; avoid creating a second interface for the same behavior.

For a new system, establish argument and return types, valid thread/context, ownership,
initialization and cleanup from the implementation and callers. Add the game-thread adapter,
then expose it through the shared Lua/C++ catalog. Handles must expire on deletion, unload
and restore. Reversible host changes need per-mod ownership and retirement. Persistent
changes need checkpoint, exact-save, code-reload and error behavior.

Generate the completion and reference together:

```sh
python3 tools/modkit/generate_api.py --output modkit/generated
```

Qualify a creator project against that contract before describing the feature as supported.
This keeps the address-level investigation inside the adapters so modders can work with
players, missions, cameras and assets through named operations.
