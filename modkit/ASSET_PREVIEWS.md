# Asset previews and format support

Open **Assets → Whole game** to browse the installed files without starting
Play. The background index uses the same native resource reader as the runtime.
It reports progress, keeps the original archives in place and includes the
current project's file replacements. Inspector shows resource GUIDs, source
locations and types.

Pause over a model row for a rotating 3D hover card. **3D preview** opens a
larger view: drag left to orbit, drag right or middle to pan, use the wheel to
zoom, and press **F** to frame the geometry. Spin can be turned off. Moving
between rows delays decoding briefly; a bounded cache reuses recently viewed
geometry and checks its source timestamps. Hidden previews stop animating.

These views draw the real decoded vertices and triangles. They show geometry
in its bind pose with neutral inspection lighting. Native materials, animation,
effects and behavior are checked in the map/Play renderer. A model's first
declared render part supplies its geometry preview; collision references stay
separate. Types or vertex declarations without a reviewed reader show their
specific limitation instead of a substitute model.

## Put a library object in a map

1. Open a Scene or Mission project and choose **Load level** above the viewport.
   The editor loads the original map and pauses its camera with fresh private
   progress. Your normal playthrough is separate.
2. Drag an object from the whole-game library onto the map viewport. Choose its
   original definition when the model has several variants.
3. The native source loader retains the archive and dependencies, reports
   progress and supports cancellation. Placement then uses the existing Scene
   operations and undo history. Current qualified Scene placement is for props;
   other factories report their unsupported behavior rather than guessing.
4. Use Inspector for position, rotation, scale, duplication and removal. Save
   live drafts, stop the workspace and apply the saved drafts to keep the mod.

**Native prop preview** is temporary: it frames an owned prop in the loaded map
and hides supported neighboring actors. **Clear preview** removes only that
temporary instance. It is never silently added to the Scene document.

## Extend a missing type

Use the named format layers instead of patching generated game functions:

- `modkit/src/streams.cpp` identifies resource types and reports GUIDs, source
  locations, references and object definitions. Keep unknown bytes intact.
- Typed readers such as `models.cpp`, `meshes.cpp` and `textures.cpp` describe
  original resources. Add a reader beside them for a new format, with explicit
  capability/reason fields for unsupported layouts.
- `modkit/src/stream_tool.cpp` exposes standalone inspection/export commands.
  The desktop `AssetStore` dispatches readers, checks exact identities and
  decodes off the UI thread. `MeshView` consumes real geometry buffers.
- A writer must validate the source fingerprint, identity, counts, bounds and
  untouched fields before producing a compact mod. Preserve topology until a
  new import path has its own native loader and collision qualification.
- Gameplay support belongs in a named operation in `modkit/api.json` with its
  adapter in `simpsons/src/modkit_game.cpp`. Reuse owned handles, source retention
  and normal teardown. Extend commented templates and regenerate the API help
  and completion when adding an operation.

Start with an inspect-only reader and a real original resource. Prove unchanged
round trips and malformed-input rejection before enabling edits. Then qualify
native load, save/reload and retirement. Source/type fixtures can cover large
inventories without playing through the campaign; behavior needs representative
native execution. Mesh, model, texture, scene and ownership fixtures provide
concrete examples.

Full offline map authoring, textured/animated previews, native studio backgrounds,
camera-correct placement and transform gizmos remain in
[the editor plan](EDITOR_PLAN.md). A geometry hover card does not qualify those
separate workflows.
