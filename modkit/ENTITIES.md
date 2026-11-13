# Objects, source loading and ownership

Mods create an object with `game.world.spawn(template.id, position, radius)` using
a resident template from `game.world.catalog()`. The return value is a temporary
handle. The host records its mod and runtime generation; guest addresses are not
public object identities.

Catalog records include the original `class` and `model_resource`, a native
resource GUID for supported models. Different model resources stay separate
even when their display labels match. An empty model ID means that layout has
no reviewed identity adapter. A class or shared model does not establish every
scripted behavior, costume override or collision dependency. Distinct native
definitions remain separate even when both the class and model agree.

The catalog also includes reviewed placeable definitions retained by a fully
loaded source stream. These can have `seen = 0`: no live source object is
required. Their handles retain the original definition GUID, stream-load key
and world generation. Destroying a source instance need not expire its
definition; unloading or reloading its source stream does. Source data stays
owned by the original loader, and clones join that source's native teardown
list. Plain catalog queries do not request archives or start another level.

The debug menu's **Game-wide library** links named models, character banks and
mesh parts to the original object definitions that use them. Names come from
compiled property groups and model metadata before an instance is spawned.
Artist names get readable spacing and known aliases; the original resource name
and source path remain visible. Different original behavior variants stay separate.
Open an object and choose **Load and spawn** to load its source and spawn it in
front of your character. The operation continues with the menu closed. Textures,
audio and other standalone resources are still resources, rather than entities.

The debug menu's **Spawner → Objects from other levels** browser can retain up
to eight additional native archives and their dependencies. Sources are grouped
by episode for browsing. Their actors stay inactive while the host retains their
definitions for the regular spawner and `world.catalog()`. Loading another source
keeps earlier objects. Additions are serialized; a failed or canceled addition
retires only its new files and protects the accepted session and current level.
Close the menu to keep a source loaded. **Cancel source load** preserves earlier
sources and copies; **Remove sources and their objects** releases the menu's
lease and removes its copies. Other owners keep their objects and shared files.

Mods use the same path through `assets.templates`, `assets.load_source`,
`assets.source_status`, `assets.source_streams` and `assets.release_source`.
Each generation leases the whole session; `source_status().stream` identifies
its latest requested root, while `source_streams()` lists all accepted roots.
Native archives can share dependencies, so accepted archives stay together
until the last owner releases the session. A new request reports `busy` while
another addition is in progress and `resource_limit` after eight roots.
`assets.cancel_source()` withdraws the caller's pending request, restoring its
earlier lease. With multiple requesters, another owner's request can continue.
Require `assets.source_streams` in a manifest that depends on these semantics;
older single-source hosts do not expose that operation.

A successful `world.spawn` from a retained source automatically acquires a lease
for the spawning mod, even if another mod originally loaded it. Removing the
loader mod preserves the new owner's copy and resources. An already loaded
dependency joins its original source without another native stream load.
Failed additions restore previous claims, including a lease acquired by spawning
from an accepted source during preparation. Objects cannot be created from new
files until the addition commits or from a session being retired.

Reload transfers supported claims and objects. Exact saves retain all archive
paths, per-file ownership and native load identities alongside guest memory;
preflight checks every claim before replacing that memory. Legacy single-source
exact saves remain readable. Checkpoint loads start their own level normally.
Wait for additions, cancellations and releases to finish before an exact save.
Cleanup checks each file's load key, address and name and protects other current
level files, including files streamed after the session began.

The [Library objects example](examples/library-objects/main.lua) contains comments
for modders and an original in-game Options page. Create it from Modkit's
creator tools, or with `modkit.py new "Library objects" --template library-objects`. It never
spawns automatically: inspect, choose an original variant, load, then spawn.
Reacquire `resident_template` after loading or restoring; an asset ID or source
GUID is metadata, not a temporary entity template handle.

This cross-level path is experimental. Loading a resource does not establish
the scene setup a boss, scripted encounter or other specialized class needs.
The Linux qualification combines eight archives and three independently posed
ScriptableProps, including shared-resource preservation, native-IO cancellation,
exact save/restore, code reload and last-owner retirement.
Native Windows and Pixel behavior remain separate acceptance work.

The native class registry supplies class names and sizes. Existing registered
objects with a native frame can establish additional placeable classes;
non-world factory services are excluded. The debug spawner shows additional
categories automatically. Broader class-specific physics, AI and script behavior
still require acceptance checks; a factory registration alone is not that proof.

Use `game.assets.search(nil, nil, 0, 20, template.model_resource)` to find the
model's named source metadata. Asset locations include `resource_id`; native
IDs contain 32 hexadecimal digits and are separate from temporary entity or
asset handles. This asynchronous library search does not load a model into
the level. Catalog labels normally come from the game's own named-resource
load, with matching library metadata available after its scan. Plain
`world.catalog()` calls never start a whole-game disk scan. Names are owned
strings tied to the current native payload; ambiguous IDs do not supply a
guessed label. See the [native model notes](../simpsons/re/entity_assets.md).

The [Combat workshop](COMBAT.md#combat-workshop) includes a native-menu template
picker. It keeps the selected resource across label changes and code reload,
and preserves the encounter's model separately from a template being browsed.
If a checkpoint's model is no longer loaded, retry fails without substitution.

`game.world.owned()` lists this generation's live objects as `{id, class}` records.
`game.world.destroy(id)` queues removal, returning true for its first request and
false for another request before cleanup. A foreign owner receives `not_owner`;
an expired identity receives `stale_handle`. Players, stock level entities and
another mod's objects cannot be removed through this operation.

```lua
-- Choose a suitable loaded prop, rather than an unloaded assets.search result.
local actor = game.players.controlled(0)
if actor then
    local p = actor.position
    game.world.spawn(template.id, {x = p.x + 4, y = p.y + 1, z = p.z}, 1)
end
for _, object in ipairs(game.world.owned()) do
    game.world.destroy(object.id)
end
```

## Transforms

`game.entities.transform(id)` reads an owned object's current world-space
`position`, `right`, `up` and `forward` vectors. The basis includes scale and may
be sheared; it is not a set of angles. The original RenderWare getter resolves
pending frame and parent updates before returning the transform.

`game.entities.move(id, position)` changes world position through the class's
original setter. This preserves the basis and includes the class's controller
and model notifications. Rotated, scaled and sheared parents are converted to
local coordinates first. Invalid frames and singular parents fail with
`unsupported` before calling the setter. Placement never scans memory for
coordinates or rewrites unrelated fields that happen to contain similar values.

`game.entities.set_transform(id, transform)` changes the complete world-space
pose: position and all three basis vectors. The original class matrix setter
updates its cached scale, queues the frame update and notifies its model;
character overrides also receive their controller update. All four vectors are
converted through the parent before dispatch. Zero or dependent axes, nonfinite
values and scales outside the original float32 length calculation's range fail
before the setter. Unsupported frames, singular parents and missing setters
have no direct-memory fallback.

```lua
-- Read a current identity from world.owned(), rather than keeping a saved ID.
local objects = game.world.owned()
if objects[1] then
    local object = objects[1].id
    local transform = game.entities.transform(object)
    local p = transform.position
    game.entities.move(object, {x = p.x + 2, y = p.y, z = p.z})
end
```

```lua
-- Make my first object 10% larger while keeping its current world position.
local objects = game.world.owned()
if objects[1] then
    local id = objects[1].id
    local pose = game.entities.transform(id)
    for _, name in ipairs({"right", "up", "forward"}) do
        local v = pose[name]
        pose[name] = {x = v.x * 1.1, y = v.y * 1.1, z = v.z * 1.1}
    end
    game.entities.set_transform(id, pose)
end
```

The commented **Owned objects** creator project adds native menu actions to
move, rotate around world Y, grow and shrink the latest owned prop. Clear also
releases a source automatically retained by a cross-level spawn. C++ plugins
use `Game::Transform`, `Game::Move` and `Game::SetTransform` with the same rules.

These operations reject another mod's owner, an older runtime generation and
objects whose removal is already queued. They are placement operations: they
do not sweep for collision clearance or disable an object's scripted movement.
Author safe destinations and class behavior. Arbitrary shape, rig and collision
imports remain separate authoring work.
Player teleports and the debug camera's family placement use the same class
dispatch, including the character controller's original placement call.

For a template that may reject cloning, use
`game.system.try_call("world.spawn", {template = template.id, position = p})`.
Its reply contains `ok` and either `value` or `code`/`error`. It handles one named
host operation while retaining argument, ownership and lifecycle restrictions.
Lua `pcall` is unavailable in this bounded sandbox; callback errors and exhausted
budgets still retire the mod. The commented creator example shows both spawn
and transform failure handling without protecting arbitrary script functions.

The limits are 64 objects per owner and 1,024 per profile, checked before calling
the factory. Pending removal still counts until destruction completes, but is
excluded from live queries and saved ownership so reload cannot bring it back.

## Visibility and encounter setup

`game.entities.render_state(id)` reports `hidden_by_mod`, `hide_count`,
`has_model` and `scene_attached`. An unsupported native layout returns `nil`.
Scene attachment means the object belongs to the renderer's spatial scene;
camera culling, occlusion, model resources and authored behavior still determine
what is drawn. A template's class label is not a costume or faction guarantee.

`game.entities.set_visible(id, false)` adds one native hide request for this
owner. Calling it again does not add another. `set_visible(id, true)` releases
that request, preserving other native hide requests. It does not force an
authored hidden object to appear. The native scene/model callbacks perform
attachment and removal; mods do not edit render flags or draw over the game.

```lua
local object = game.world.owned()[1]
if object then
    local state = game.entities.render_state(object.id)
    if state then
        game.entities.set_visible(object.id, state.hidden_by_mod)
    end
end
```

The request transfers on code reload and exact ownership restore without
incrementing the native counter again. A native force-show cancels it, so a
later mod reveal cannot consume a newer authored hide request. Checkpoints
create a fresh world; recreate the desired state with the new objects. Unknown
layouts reject a visibility change with `unsupported`; a full native counter
rejects with `resource_limit`. A callback that removes the object returns `nil`.

Visibility leaves AI, collision, damage and audio running. Use it for staging
or visual changes; it does not suspend enemies or establish an encounter's
navigation and behavior. Combat workshop includes native menu actions to hide
and reveal its own targets.

## Removal and lifecycle

The [combat guide](COMBAT.md) covers typed owned health, native damage/healing
and `entity.died`. Ordinary removal does not establish a defeat. These services
share the lifetime and ownership rules below.

Cleanup runs on the game thread after callbacks return. The original helper
chooses the class deleting destructor or subscriber destruction path, allowing
normal physics, component, subscription and world-list cleanup. Each request is
claimed once. A class that completes later remains tracked without receiving
another destruction request. Batches stop at 16 requests or approximately 2 ms,
apart from the request already in progress.

A callback fault retires that mod's objects; dependents retire on their next
dispatch. Unrelated owners remain active. Shutdown on a worker queues host state
only, without calling stopped guest threads. The game owns final world teardown.

The owner receives `entity.removed` after a gameplay deletion or completed
`world.destroy`. It carries `entity` (now stale) and `reason` (`game` or `removed`).
Notifications do not cross runtime generations. Use `level.unloading` for whole
world teardown; it does not replay individual removal events.

The base destructor invalidates a lifetime before memory is freed. Reusing the
same address, GUID and class still produces a new identity. Player, entity and
template handles reject reuse before reading the old allocation. Spawned copies
do not replace their source templates in the catalog.

## Reload and saves

Reload transfers live objects without a despawn/respawn cycle. Preparation acquires
nothing. Once the old runtime retires, the replacement adopts its own matching
objects before cleanup drains. Discarding failed preparation cannot release a live
owner. Requery `world.owned()` in `code.reloaded`; earlier handles have expired.

A checkpoint starts a new world. Its old objects cannot be adopted into that world
or destroyed through stale pointers. An authored mission must recreate intended
objects from its saved mission data and supported loaded templates. Do not save
object handles in `game.state`.

Exact-snapshot ownership records contain GUIDs, class names and type signatures,
without guest addresses. Commit drops old host identities and resolves reserved
mod GUIDs in the restored direct entity index, adopting restored allocations
instead of making copies. Isolated ownership and original-routine fixtures cover
this contract; full in-game exact-snapshot qualification remains required.
The loaded-template catalog is invalidated on rewind. Reconstructing it from
snapshot metadata is still required for complete spawn authoring after restore.

## Creator project and boundaries

Choose **Owned objects** in desktop or Android **File → New project** in Modkit. Its commented
Lua project uses **Options → Extras → Mods** to select loaded props, spawn beside
the character using the page, move its latest object and remove its own objects.
Projects start disabled.
This development example uses the public object-ownership APIs and starts disabled.

The factory and world registration are separate loader steps. Clones receive a
unique GUID before registration, enter the direct entity index and join the source
layer's normal teardown list. They do not borrow or modify streaming GUID nodes.
An initializer that already registered its new instance is detached through the
original unregister routine before unique registration. A factory returning its
existing source instance rejects cloning without destroying that source.

This does not load cross-level dependencies or prove every class safe to duplicate.
Encounters, collectibles and scripted objects may need class-specific setup.
Rotation/scale editing, general component/lifecycle queries and cross-level
streaming remain in the full kit plan. Native Windows and Pixel runs are
qualified separately from compilation and Proton fixtures.

The private Linux GAME_HUB check covers ScriptableProp placement and movement,
unchanged basis and source templates, independent owners, menu actions, player
teleport, reload and retirement with a clean exit. The sampled prop has no bodies
in its owned Havok body list; collision-rich classes need their own acceptance
checks. The original-routine fixture separately covers affine and nested parents,
controller dispatch, invalid hierarchies and singular transforms. These checks
do not qualify arbitrary class physics, scripted behavior or the whole campaign.
