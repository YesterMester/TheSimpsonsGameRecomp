# Native model variants

The creator tools read the game's MMdl model definitions as named parts,
properties, states, conditions and resource references. The offline writer can
change reviewed scalar, vector, matrix and existing-reference fields while
preserving the original resource identity and layout.

The live package path currently supports **ModelPart.Model** choices between
already declared DFF references of the same native type. This is useful for
trying an existing model's alternate render parts. The separate
[static mesh tools](MESHES.md) edit supported existing vertex positions. New
topology, skeletons, animations, collision and dependencies remain part of the
[full kit](PLAN.md).

## Inspect and build

Build `modkit_streams` from the standalone tools, without rebuilding the game:

```sh
cmake -S modkit -B /path/to/modkit-build -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/modkit-build -j2 --target modkit_streams
```

Use the asset library or a deep `modkit_streams index` report to find the STR,
entry and exact resource name. Export a model from your own installed files:

```sh
python3 tools/modkit/modkit.py --gamedata /path/to/gamedata model inspect gamehub/gamehub.str 23 frink_geo.meta.xml /path/to/frink.json --stream-tool /path/to/modkit-build/modkit_streams
```

Read `parts`, `fields` and `references` in the JSON. Keep the identity, source
hash and inspection fields intact. Put the intended edits in `changes`; for
the GameHub Frink definition, this selects its already declared second render
model for the first ModelPart:

```json
"changes": [
  {"field": "property:0", "value": "reference:2"}
]
```

These IDs identify records in that exact source, rather than memory addresses.
Do not reuse the ordinals for a different model. The source hash prevents that
kind of accidental cross-edit. A read-only inspection value edited outside
`changes` is rejected.

Create a credited data project, then build its edits:

```sh
python3 tools/modkit/modkit.py new "My model variant" --kind data --id yourname.model-variant --author "Your name"
python3 tools/modkit/modkit.py --gamedata /path/to/gamedata model build yourname.model-variant /path/to/frink.json --stream-tool /path/to/modkit-build/modkit_streams
python3 tools/modkit/modkit.py validate yourname.model-variant
python3 tools/modkit/modkit.py pack yourname.model-variant
```

`model build` checks the selected source resource and live edit policy before
writing `assets/model-edits.json` and adding `model_edits` to `mod.json`. Repeated
builds replace that resource's entry in the catalog. The package contains the
small edits and their source fingerprint, without copying an STR or DFF. Keep
your editable inspection document separately if you want to try more changes.

Use `run` with the current development executable and an absolute runtime path
to test with private progress, as described in the [main guide](README.md).
Restart the session after changing model assets. Code reload reports that a
restart is required and preserves the current instances. Imported packages stay
disabled and keep the creator credit through the existing Mods workflow.

## Priority and loading

Model edits follow whole-resource priority: the first requested mod wins, and
dependencies have lower priority. The package conflict report identifies
overlaps as `model:<resource-id>`. Different patches for one identity are not
silently combined. The winning document must match the resource's exact source
hash; a mismatch leaves that resource unchanged and identifies the mod in the
log.

The runtime captures and verifies catalog bytes before play. At the native
model loader it validates the chosen body and complete edit before committing
same-size bytes, then calls the original parser once. The game continues to
own binding, rendering and teardown. No file reads or replacement GPU buffers
are introduced in that callback. Model changes persist for the loaded resource;
disabling or changing an asset package requires restarting the game.

## Offline research and qualification

For a single extracted named-resource chunk, `model-inspect RESOURCE [OUTPUT]`
exports its description and `model-edit RESOURCE DOCUMENT OUTPUT` writes a new
chunk. Outputs must be new paths. Scalars and reviewed vectors/matrices are
available here for format research; their component behavior is not qualified
by the live variant policy. Unsupported types, shared data and unknown layouts
stay read-only or are rejected.

The reference fixture runs the unchanged original relocation and inverse
routines and compares every payload byte and surrounding guards. A private
installed-game check covers 2,616 definitions, including typed offline edits to
2,351 of them. This qualifies the binary reader/writer and pointer layout.
Runtime fixtures separately cover immutable source capture, source drift,
priority, restart requirements and the production hook's single-call/register/
exception behavior. A private Linux Vulkan comparison also loads the Frink
variant through the original resource binder, verifies its selected native DFF,
renders source/copy views, resumes simulation and removes the owned copy with
the original world count restored. Both runs exit cleanly. This covers that
existing-resource variant; new geometry, broader components and native
Windows/Pixel qualification remain separate. Check [coverage.json](coverage.json)
for the current acceptance boundaries.
