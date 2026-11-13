# Mesh attribute editing

The mesh tools export supported preinstanced geometry to OBJ and build edited
positions, normals and UVs into a small, source-checked mod. The native loader applies the
edit before it creates the original rendering resources. The game keeps ownership
of the model, materials, buffers and teardown.

The writer preserves vertex counts, order, triangulated topology and materials.
Normals and UVs stay unchanged unless their import is selected. Edited static
positions must stay inside the original box and sphere. Reviewed skinned streams
support UV-only edits, with positions, normals and weights kept intact.
Unreviewed declarations and multipart native sections remain read-only.
New topology, imported characters, automatic normal generation,
rigs, animation and collision are separate work in the [full kit](PLAN.md).

## Inspect your source

Build the standalone tool without rebuilding the game:

```sh
cmake -S modkit -B /path/to/modkit-build -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/modkit-build -j2 --target modkit_streams
```

Find a mesh in the asset library or a deep stream index. Use its exact resource
GUID when multiple resources share a name. For example, this reads the coffee
table from your own installed files:

```sh
python3 tools/modkit/modkit.py --gamedata /path/to/gamedata mesh inspect gamehub/gamehub/zone13.str 6 9D47543EE15FF766E15FF766E15FF766 /path/to/table.json --stream-tool /path/to/modkit-build/modkit_streams
python3 tools/modkit/modkit.py mesh export /path/to/table.json /path/to/table.obj
```

The JSON records position `editable`, `normals_editable` and `uvs_editable`
capabilities with their reasons. It also records
the resource identity, exact source hash, original positions, triangle indices
and bounds. Keep those inspection fields intact. The native vertex count can
differ from the old source count because preinstancing can split or compact
vertices; the OBJ uses the allocation the renderer actually draws.

## Edit and package

Edit the OBJ's positions in your modeling tool. Preserve `geometry_0` and other
exported object names, vertex order, face winding and triangles. Disable vertex
welding, reordering and coordinate conversion on import/export. UV and normal
records are imported only when requested. Export a triangulated OBJ.

A small uniform shrink around the original sphere center is the qualified
starting example. Position-only deformation still uses the original normals and
collision. Use the normal workflow below when the shape needs new lighting directions;
physical interaction keeps its original collision.
The writer rejects positions outside the original culling bounds.

```sh
python3 tools/modkit/modkit.py mesh import /path/to/table.json /path/to/edited-table.obj /path/to/edited-table.json
python3 tools/modkit/modkit.py new "My table" --kind data --id yourname.table --author "Your name"
python3 tools/modkit/modkit.py --gamedata /path/to/gamedata mesh build yourname.table /path/to/edited-table.json --stream-tool /path/to/modkit-build/modkit_streams
python3 tools/modkit/modkit.py validate yourname.table
python3 tools/modkit/modkit.py pack yourname.table
```

`mesh build` verifies the source, geometry layout and complete edit before
updating `assets/mesh-edits.json` and the manifest's `mesh_edits` path. Building
another resource adds it to the same catalog; rebuilding an identity replaces
its previous edit. The package stores compact attribute edits and fingerprints,
without copying a game archive or DFF. Keep your full inspection JSON and OBJ
separately for further editing. Re-exporting an edited document includes its
current changes. `--geometry geometry:N` exports a supported subset; importing
that subset preserves pending edits to other geometries.

Test through the [isolated creator workflow](CREATOR_WORKFLOW.md), then import
the ZIP through Mods. Imported packages keep their creator credit and remain
disabled until enabled. Restart the game after changing mesh assets. Code reload
reports that a restart is required and retains the active prepared selection.

## Import matching normals

Inspect `normals_editable` and `normals_reason` for each geometry. The reviewed
static stream can be included in OBJ with `--normals`:

```sh
python3 tools/modkit/modkit.py mesh export /path/to/table.json /path/to/table-with-normals.obj --normals
python3 tools/modkit/modkit.py mesh import /path/to/table.json /path/to/edited-table-with-normals.obj /path/to/edited-table.json --normals
```

Edit or recalculate normals in your modeling tool, then export face-corner
normal references. Keep the original vertex splits: one preserved vertex cannot
have different normals on different faces. The importer rejects that conflict
instead of choosing one direction. Unreferenced vertices retain their previous
normals, and position-only reimports preserve pending normal edits.

Normals must be finite unit vectors; the validator allows the original packed
stream's quantization tolerance. The native writer stores signed ten-bit XYZ,
preserving the fourth component and every unrelated byte. An unchanged OBJ
normal round-trip preserves the complete original coffee-table resource exactly.
Earlier position-only inspection documents still work; inspect again to obtain
the added normal metadata before using `--normals`.

## Edit texture coordinates

Use `--uvs` to export and import the reviewed primary texture-coordinate stream.
For example, Frink's first geometry supports UV editing while its skinned
positions stay read-only:

```sh
python3 tools/modkit/modkit.py --gamedata /path/to/gamedata mesh inspect gamehub/gamehub.str 19 2C04E125EA57CADCEA57CADCEA57CADC /path/to/frink.json --stream-tool /path/to/modkit-build/modkit_streams
python3 tools/modkit/modkit.py mesh export /path/to/frink.json /path/to/frink.obj --uvs --geometry geometry:0
python3 tools/modkit/modkit.py mesh import /path/to/frink.json /path/to/edited-frink.obj /path/to/edited-frink.json --uvs
```

Build and package that document with the same `mesh build` workflow above.
Export selects geometries supporting the requested streams; `--geometry` chooses
a specific subset. `--normals --uvs` includes both where both are supported.

UVs use the game's original U/V orientation, without an implicit image flip or
0–1 clamp. Negative coordinates and repeats are preserved. The importer requires
two finite float32 components and preserves the original UV seams. Different
face-corner UVs cannot be assigned to one preserved vertex; that needs a topology
edit, which this writer does not perform.

On UV-only geometry, changing a preview vertex position is rejected. Unchanged
positions are omitted from normal/UV import documents, and unreferenced vertices
keep their prior attributes. Reimporting one stream preserves pending changes to
the other streams. Earlier inspection documents remain compatible; inspect again
to obtain UV capability metadata. This edits texture mapping, with the existing
textures, materials and rig retained. Texture-image and rig import are separate.

## Validation and priority

The first requested mod wins a whole resource; dependencies have lower priority.
Conflicts appear as `mesh:<resource-id>`. Edits to the same GUID are not silently
combined. A source mismatch leaves the resource unchanged and identifies the
mod in the log.

Catalogs are verified and captured before play. Each mod supports up to 64 mesh
resources in a 16 MiB catalog; a profile supports up to 1,024 entries and 64 MiB
of prepared catalogs. The loader checks the exact named-resource fingerprint,
supported geometry and requested attributes before copying same-size
bytes into the game-owned input. It then follows the original load path. The callback does not
read package files or allocate replacement GPU resources.

For an extracted named chunk, `modkit_streams mesh-inspect RESOURCE [OUTPUT]`
describes it and `mesh-edit RESOURCE DOCUMENT OUTPUT` writes an edited copy.
These outputs require new paths. The artist-facing commands above select the
resource directly from the installed STR and preserve its source location.

## Qualification

The original relocation and copy/rebase routines agree with the host reader on
all 5,533 mesh resources and 20,490 native blocks in the private installed-game
inventory, with no failures. The reader identifies 3,145 static geometries within
the first writer's supported layouts. That is format coverage, rather than a
claim that every geometry has been edited and played through.

The normal codec also round-trips 1,881,360 packed normals across 2,825 supported
static geometries without changing any bits. Native field literals, alternate
negative-one encoding, the preserved fourth component and malformed directions
have focused fixture checks.

The UV writer also preserves all bytes in no-op edits across 3,313 resources:
15,321 geometries and 6,436,832 UV pairs, including 12,176 skinned geometries.
Position and normal coverage remains unchanged. A private Linux Vulkan Frink
edit verifies all 36,624 bytes of the bound vertex stream: only the requested
UV bytes differ, with skinning, positions, normals and other attributes intact.
The owned NPC renders from both sides, retains the stream through simulation,
removes with world count restored and exits cleanly with the audio pause fix.

A private Linux Vulkan test loads edited pickup and coffee-table resources. It
compares all 363 coffee-table vertices with the imported edit, renders front and
back views of an owned prop using that geometry, removes it and restores the
original world count before exiting cleanly. A second run changes the table's
shape and normals together, compares all 363 native position/normal pairs,
renders both views and repeats the balanced removal and clean exit.
Runtime fixtures cover source drift,
immutable capture, priority, restart requirements and optional loader failures.
Native Windows, other renderer game runs and Pixel hardware qualification remain
separate. See [coverage.json](coverage.json) for the full acceptance boundary and
the [native format notes](../simpsons/re/native_mesh_data.md) for source evidence.
