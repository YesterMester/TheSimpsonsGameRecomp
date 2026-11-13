# Game API

Generated from `modkit/api.json`. Calls run in game-thread callbacks.

## `game.scenes.document()`

Read this mod package's validated scene and frozen source fingerprint without filesystem access. An absent scene returns an empty authoring document. Scene objects use original definition GUIDs, stream paths, stable per-owner tags and world poses, never temporary handles.

Returns: scene project.

## `game.scenes.validate(document)`

Validate and canonicalize a portable scene document. Enforces unique tags, an unambiguous episode root, original GUID/class/source identities, bounded nonsingular float32 poses, radius limits and at most 64 objects. Does not spawn or change storage.

Returns: validated scene document.

## `game.scenes.save(document)`

Queue this mod's validated scene draft in persistent storage, with its frozen source fingerprint. Check storage.status for completion. The launcher's Apply scene draft action copies a matching creator-Test draft into the editable package before export. This does not write code or replace game files.

Returns: validated scene document.

## `game.world.tagged(tag)`

Find this generation's live owned object by its stable tag. Returns a fresh entity handle or nil; pending removals are excluded. Tags survive supported code reload and exact restore. This capability identifies builds that support tagged world.spawn.

Returns: entity handle or null.

## `game.timers.schedule(id, seconds, interval, data)`

Schedule or replace this mod's simulation timer. Pauses and menu redraws do not advance it. A timer event carries id, data and skipped intervals; repeats coalesce after a slow frame instead of catching up in a loop. Timers are saved with mod state.

Returns: timer id.

## `game.timers.cancel(id)`

Cancel a timer owned by this mod. Returns whether it was pending.

Returns: boolean.

## `game.timers.list()`

List this mod's pending timers, remaining simulation seconds, repeat intervals and event data.

Returns: array of timers.

## `game.text.lookup(id)`

Read a loaded game text ID as UTF-8, including active native language packs. IDs are listed by the translation exporter; unloaded entries report found=false.

Returns: found and text.

## `game.assets.search(category, text, offset, limit, resource_id)`

Search the game-wide resource library by category, name or a 32-digit native resource_id. Locations include native resource GUIDs when the named header provides one. Indexing is asynchronous; asset identities are not loaded entity templates.

Returns: index progress, categories and a bounded page of named resources.

## `game.assets.templates(asset, offset, limit)`

List original object definitions using an indexed asset, including native model/character references and source stream paths. Friendly names are available before loading. A supported definition becomes a resident_template after its source is ready; pass that current handle to world.spawn. Geometry references can have several original object variants.

Returns: ready, total and paged source object records.

## `game.assets.load_source(stream)`

Asynchronously add a source archive and its original native dependencies to a shared retained session. Up to eight archives can coexist; additions are serialized and a failed addition rolls back without removing earlier objects or resources. Each generation leases the whole session, with its latest requested root reported by source_status. Poll readiness, then reacquire a resident handle from assets.templates or world.catalog. Source actors stay inactive. Require assets.source_streams for multi-source capability on older builds.

Returns: boolean.

## `game.assets.source_status()`

Report this generation's selected source, readiness and request errors. Existing sources stay ready while another owner adds an archive. streams lists every committed archive; pending_stream names the current addition. Native progress counts the shared session. A failed addition restores preceding leases and reports failure only to its waiting owners.

Returns: source claim and progress.

## `game.assets.source_streams()`

List committed archive paths in the shared native source session. This capability identifies builds that support combining sources. Shared dependencies remain resident until the last session owner releases; individual archives are not independently unloaded.

Returns: string array.

## `game.assets.cancel_source()`

Withdraw only this generation's pending addition and restore its preceding lease, including one acquired by a successful spawn from an old source. Existing objects and archives stay loaded. Native rollback starts when every waiting owner withdraws. Does not release a committed session; use release_source for that.

Returns: boolean.

## `game.assets.release_source()`

Release this generation's whole source-session lease. Original native retirement runs after the last owner releases, removing the session's objects and archives together. Other owners keep all retained resources. Failed additions expire only their own temporary data. Reload and exact saves retain committed roots and claims; checkpoint/world changes discard them.

Returns: boolean.

## `game.locale.current()`

Return the active mod language code, such as en or pt-br.

Returns: string.

## `game.locale.text(key, values)`

Look up this mod's localized text with regional and source-language fallback; substitute named {values}. Button tags remain dynamic.

Returns: string.

## `game.system.describe()`

Describe every implemented operation and its arguments.

Returns: API catalog.

## `game.mods.status()`

List loaded mods, creator credits, errors and callback costs.

Returns: array of mod records.

## `game.system.try_call(method, arguments)`

Invoke one named host API with a named-argument object and return {ok=true,value=...} or {ok=false,code=...,error=...} for an expected host failure. Target validation, ownership, save read-only and preflight restrictions still apply. Cannot wrap itself or run a Lua function; interpreter errors and callback limits remain fatal. Use this for expected spawn or transform rejection, rather than pcall (which the sandbox disables).

Returns: host call result.

## `game.log.info(message)`

Write a message attributed to this mod in the game log.

Returns: nil.

## `game.storage.read(path)`

Read this mod's cached persistent JSON; missing files return nil. Storage is loaded before callbacks start.

Returns: saved value or nil.

## `game.storage.write(path, value)`

Queue an atomic JSON write to this mod's persistent storage without blocking the game thread. Reads see the queued value immediately; use storage.status to check completion and errors.

Returns: nil.

## `game.storage.status()`

Report this mod's pending persistent writes and disk errors. Pending zero with no errors means all writes have reached the filesystem.

Returns: {pending, errors}.

## `game.world.status()`

Get the current level readiness, location and world epoch. level_root is the unique loaded episode catalog excluding retained foreign sources; empty means it cannot be proven, so portable scenes must wait. streaming reports unfinished native file/manager work; scene autoload waits for it before requesting additional sources.

Returns: world record.

## `game.players.list()`

List the loaded family, including AI actors, with temporary handles, character names, positions, health and input_player. A slot is a family-list index; input_player is logical port 0/1 or null for AI, unbound or ambiguous ownership.

Returns: array of player records.

## `game.players.controlled(player)`

Get the family character currently bound to logical input player 0 or 1, or nil when no level/actor is ready or ownership is ambiguous. Requery during simulation after character switches, co-op joins, dropouts and restores; a previous actor handle may still identify a live AI character. This reads the game binding without polling devices. Paused or temporarily blocked controls do not change the binding.

Returns: player record or nil.

## `game.players.teleport(player, position)`

Move one loaded player using the game's two world transforms.

Returns: nil.

## `game.players.refill_health(player)`

Refill a living family member through the original healing message and recovery callbacks, up to its normal healing limit. Does not resurrect a dead actor.

Returns: nil.

## `game.players.god_mode(enabled)`

Request family damage protection for this mod. The latest active request wins; retirement restores the previous owner or manual value. Manual controls supersede older requests.

Returns: nil.

## `game.scores.get(name)`

Read a named ScoreKeeper score. A missing score currently follows the game's zero result.

Returns: number.

## `game.scores.set(name, value)`

Set a named score through ScoreKeeper, including its original watchers.

Returns: nil.

## `game.scores.issue(name)`

Issue a named ScoreKeeper event through the original game logic.

Returns: nil.

## `game.camera.free(enabled)`

Request the native development camera in a running level. This mod owns its request; retirement reveals the preceding request or manual state.

Returns: nil.

## `game.camera.photo(enabled)`

Request photo mode with its pause and HUD handling. Camera requests are retired with this mod; checkpoints discard old-world camera requests.

Returns: nil.

## `game.camera.status()`

Read the current free-camera and photo-mode state.

Returns: camera record.

## `game.camera.pose()`

Read viewport 0's last applied native camera pose, or nil when no supported level camera is running. Coordinates use Y up; rotation and vertical fov_degrees are in degrees. A set_pose request appears here after the next native camera update.

Returns: ModCameraPose or nil.

## `game.camera.set_pose(pose)`

Request an owned viewport 0 camera pose through the original native camera writer and enable the free camera. Position axes are finite and within +/-1000000, pitch is -90..90 degrees, yaw/roll are finite within +/-360000 degrees and normalize to one turn, and vertical fov_degrees is 1..120. Photo mode keeps its current value. Manual camera input takes control; release/retirement restores the preceding explicit request or captured manual baseline. Checkpoints discard old-world requests.

Returns: canonical requested ModCameraPose.

## `game.camera.release()`

Release only this mod generation's camera request, revealing the preceding owner or captured manual baseline. Other owned services remain active. Repeating release is safe.

Returns: nil.

## `game.characters.list()`

List the character templates the existing character selector supports.

Returns: array of character records.

## `game.characters.apply(player1, player2)`

Apply character templates to both players using the existing checkpoint restart.

Returns: nil.

## `game.world.catalog()`

List live and retained native entity templates from fully loaded source streams. Distinct definition records stay separate even when class, model and display names match. Each record includes class, seen (live source count, possibly zero) and model_resource (a native GUID or an empty string for unsupported layouts). The native registry supplies additional observed placeable classes. Plain catalog queries do not start disk scans. Retained handles expire with their source load or world, rather than requiring a live source instance. source_stream names the exact loaded native file that owns the original definition; an empty path cannot be exported as a portable scene source.

Returns: array of spawnable records.

## `game.world.spawn(template, position, radius, tag)`

Clone a validated resident template with the original factory, unique world registration, post-spawn initialization and class position setter. Position is in world coordinates. Unsupported transforms fail without scanning object memory. The calling mod owns the object; reload transfers ownership and retirement queues safe game-thread cleanup. Reacquire handles with world.owned after reload or exact restore. Optional tags are unique per mod generation, including pending removal, and survive reload/exact restore. Require world.tagged when using tags.

Returns: temporary entity handle.

## `game.world.owned()`

List this mod generation's live spawned objects as id/class records. Excludes pending removals. Requery after reload or exact restore; checkpoints start a new world and mods recreate their objects. Each record also includes tag (empty for untagged objects).

Returns: array of owned entity records.

## `game.world.destroy(entity)`

Queue removal of an object spawned by this mod. Runs the original class/subscriber destructor after callbacks, including collision and world unregister. Returns true once, false if already queued; foreign or expired handles fail.

Returns: boolean.

## `game.entities.transform(entity)`

Read a live object owned by this mod as a world-space position and right/up/forward basis. Resolves pending frame and parent updates through the original getter. Basis vectors retain scale; they are not Euler angles. Pending removals, foreign owners and expired handles fail.

Returns: entity transform.

## `game.entities.move(entity, position)`

Move this mod's live object to a finite world position while preserving its basis. Uses the class's original position setter and its controller/model notifications. Rotated, scaled and sheared parents are converted to local coordinates; singular or invalid parents fail before the setter. Does not sweep for collision clearance or stop an authored controller from moving it afterward.

Returns: nil.

## `game.entities.set_transform(entity, transform)`

Set this mod's live object's complete world-space position and right/up/forward basis through the class's original matrix setter. Includes orientation, scale, cached native scale and model/controller notifications. Converts the whole matrix through rotated, scaled or sheared parents. Nonfinite, singular or native-length overflow/underflow bases reject before dispatch; invalid frames or unavailable setters fail without a raw-write fallback. Does not sweep for collision clearance or stop authored movement.

Returns: nil.

## `game.ui.register_page(page)`

Register or replace this mod's native Options page. Supports action, toggle, slider, select and text rows; actions arrive as ui.action events.

Returns: namespaced page id.

## `game.ui.remove_page(id)`

Remove a native Options page owned by this mod.

Returns: nil.

## `game.input.players()`

Read physical assignments, device identities and resolved prompt families for logical players 0 and 1. prompts retains the configured override (including auto); prompt_family is its resolved family. This does not poll devices or map family-list entities to controllers.

Returns: array of input records.

## `game.input.state(player)`

Read the last routed game input for logical player 0 or 1. Buttons, pressed/released edges, normalized sticks and triggers are frozen for this modkit frame; reads do not poll or consume input. sampled says a poll has been observed; updated says it arrived this frame; available is the last poll's success, not a physical-connection query. First samples and reconnects establish a baseline without synthetic presses. Unpolled channels retain their last values without repeating edges. Read during simulation for gameplay actions; use ui.action for native menus.

Returns: input snapshot with buttons, pressed, released, left_stick, right_stick, left_trigger, right_trigger and sampling metadata.

## `game.settings.get(name)`

Read an existing runtime setting with its current value and restart metadata.

Returns: setting record.

## `game.missions.register(definition)`

Register or refresh this mod's authored mission definition during initialize or state.loaded. Objectives have stable ids, positive goals, prerequisites and optional flags. Live goal changes need a state migration or a project restart.

Returns: mission definition and current objective state.

## `game.missions.list()`

List this mod's authored missions and objective progress. Other mods' missions remain isolated.

Returns: array of missions.

## `game.missions.get(id)`

Read an authored mission by its local id, including active/locked/completed objectives and retry-point availability.

Returns: mission state.

## `game.missions.start(id)`

Start an idle authored mission while a level is loaded. Progress and notifications are saved with this mod. This does not replace the base campaign.

Returns: active mission state.

## `game.missions.advance(id, objective, amount)`

Add nonnegative progress to an active, unlocked objective. Progress clamps to its goal; completing every required objective completes the mission. Changes coalesce into an owned mission.changed event.

Returns: updated mission state.

## `game.missions.fail(id, reason)`

Fail an active authored mission with a reason. It may then be retried from its recorded objective checkpoint or restarted from the beginning.

Returns: failed mission state.

## `game.missions.checkpoint(id)`

Record the active mission's current objective progress as a retry point. This records authored mission state; it does not save or reset the game world.

Returns: mission state with a retry point.

## `game.missions.restart(id, from_checkpoint)`

Retry an authored mission. from_checkpoint defaults to true; false replays it from the beginning. Scripts must restore any associated world changes through their own supported APIs.

Returns: active mission state.

## `game.missions.restore(state)`

During state.loaded only, validate and migrate this mod's saved mission service state from the event's missions field. Unhandled state is restored automatically after the callback. Invalid migrations reject the replacement before commit.

Returns: restored authored missions.

## `game.players.health(player)`

Read the original health component, or nil when it is absent or unsupported. maximum is the component's normal healing limit; dead follows its native death policy. Query players.controlled or players.list for a current family handle.

Returns: ModHealth or nil.

## `game.players.damage(player, amount)`

Apply scripted damage through the original eligibility, hit, event and death callbacks. Damage protection remains active; a rejected or absorbed hit can leave health unchanged. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Query players.controlled or players.list for a current family handle.

Returns: ModHealthChange.

## `game.players.heal(player, amount)`

Heal through the original eligibility and recovery callbacks, capped at the normal healing limit. Dead actors are not resurrected. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Query players.controlled or players.list for a current family handle.

Returns: ModHealthChange.

## `game.entities.render_state(entity)`

Read this mod's object's native hide count, model availability and scene membership. Null means its visibility layout is unsupported. Scene membership is not proof of camera visibility or finished NPC behavior.

Returns: native render state or null.

## `game.entities.set_visible(entity, visible)`

Hide or reveal this mod's object through the original visibility methods. The host balances one hide request; repeated calls are idempotent and reveal preserves the game's other requests. Reload transfers the request without applying it twice. A native force-show cancels it. Does not suspend AI, collision, audio or damage. Unsupported layouts fail before mutation; null means a class callback removed the object.

Returns: native render state or null if removed.

## `game.entities.health(entity)`

Read the original health component, or nil when it is absent or unsupported. maximum is the component's normal healing limit; dead follows its native death policy. Only this mod's active owned objects are accessible.

Returns: ModHealth or nil.

## `game.entities.damage(entity, amount)`

Apply scripted damage through the original eligibility, hit, event and death callbacks. Damage protection remains active; a rejected or absorbed hit can leave health unchanged. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Only this mod's active owned objects are accessible.

Returns: ModHealthChange.

## `game.entities.heal(entity, amount)`

Heal through the original eligibility and recovery callbacks, capped at the normal healing limit. Dead actors are not resurrected. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Only this mod's active owned objects are accessible.

Returns: ModHealthChange.

## `game.projects.document(kind)`

Read this package's frozen scene or mission document and source fingerprint. This uses prepared metadata without reading source files in a callback. Missing documents return an empty authoring document.

Returns: project document and fingerprint.

## `game.projects.validate(kind, document)`

Validate and canonicalize a portable scene or mission document. Mission graph rules use the native mission service without registering a live mission. Rejects unknown fields, temporary handles, invalid episode roots, cycles and out-of-bounds points or durations.

Returns: validated project document.

## `game.projects.save(kind, document)`

Queue a validated scene or mission draft in namespaced persistent storage, tied to this frozen source revision. Check storage.status for completion. Apply saved drafts in the launcher merges matching scene and mission drafts atomically before exporting the project.

Returns: validated project document.

## `game.missions.remove(id)`

Remove this mod's mission definition, progress, retry point and pending change notification. Returns false if absent. This lets an editor stop a preview before changing its contract; it does not reset the game world. Allowed during private initialization/restore preflight; save callbacks remain read only.

Returns: whether a mission was removed.

## `game.missions.advance_batch(id, updates)`

Atomically advance up to 32 distinct objectives that were already unlocked before the batch. Every id and amount validates before progress changes. Returns one mission snapshot and coalesces one notification, avoiding repeated full-state copies for simultaneous goals.

Returns: updated mission state.

## `game.projects.save_all(documents)`

Validate a scene and/or mission document before queuing one atomic project-drafts.json replacement. Both documents share the frozen source fingerprint. Use this for a project containing both editors, then wait for storage.status before applying saved drafts in the launcher.

Returns: validated documents keyed by kind.

## `game.physics.knockdowns(options)`

Lease experimental whole-character knockdown settings for this mod. Requires all switches enabled, run_impact, punch_impact, push_downed, player_enabled and force_scale (0.25 to 4). Pauses stop the effect; disabling, unloading or failing a mod releases its request. This is a root-motion prototype, not a jointed ragdoll or terrain collision solver.

Returns: null.

## `game.physics.release_knockdowns()`

Release this mod's knockdown request, revealing the preceding owner or manual baseline. Call from normal game-thread callbacks, not initialization or restore preflight. Restoration of surviving character poses occurs on the game thread.

Returns: null.

## `SimpsonsCallResult`

A single host API result. Only typed host failures are recoverable; this does not protect a Lua callback.

| Field | Type | Meaning |
| --- | --- | --- |
| `ok` | `boolean` |  |
| `value` | `any` | The target's return value on success, including nil. |
| `code` | `string?` | Stable failure code when ok is false. |
| `error` | `string?` | Readable failure description when ok is false. |

## `SimpsonsVec3`

A three-dimensional vector in the game's coordinates: X/Z ground plane, Y height.

| Field | Type | Meaning |
| --- | --- | --- |
| `x` | `number` |  |
| `y` | `number` |  |
| `z` | `number` |  |

## `SimpsonsPlayer`

A loaded family character. Handles are transient; family order and controller assignment are independent.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | `string` | Temporary actor handle; reacquire after unloading, deletion or restore. |
| `slot` | `integer` | Index in the loaded family list, not an input assignment. |
| `input_player` | `integer\|nil` | Logical port 0/1, or nil for AI, unbound or ambiguous ownership. |
| `character` | `string` |  |
| `health` | `number` |  |
| `max_health` | `number` |  |
| `position` | `SimpsonsVec3` |  |

## `SimpsonsInputButtons`

Logical game controls. These names stay the same when controller glyphs change. Trigger buttons become held above 30/255; not every device exposes guide.

| Field | Type | Meaning |
| --- | --- | --- |
| `a` | `boolean` |  |
| `b` | `boolean` |  |
| `x` | `boolean` |  |
| `y` | `boolean` |  |
| `dpad_up` | `boolean` |  |
| `dpad_down` | `boolean` |  |
| `dpad_left` | `boolean` |  |
| `dpad_right` | `boolean` |  |
| `start` | `boolean` |  |
| `back` | `boolean` |  |
| `left_stick` | `boolean` |  |
| `right_stick` | `boolean` |  |
| `left_shoulder` | `boolean` |  |
| `right_shoulder` | `boolean` |  |
| `guide` | `boolean` |  |
| `left_trigger` | `boolean` |  |
| `right_trigger` | `boolean` |  |

## `SimpsonsInputStick`

Normalized routed stick values, without an extra modkit dead zone.

| Field | Type | Meaning |
| --- | --- | --- |
| `x` | `number` | Left -1, centered 0, right 1. |
| `y` | `number` | Down -1, centered 0, up 1. |

## `SimpsonsInputSnapshot`

One logical player in the current modkit frame. Do not store physical input in saved game.state.

| Field | Type | Meaning |
| --- | --- | --- |
| `player` | `integer` | Logical player 0 or 1; not a family-list index. |
| `frame` | `integer` | Monotonic snapshot frame; both players share it. |
| `sampled` | `boolean` | An original game poll has been observed since the last reset. |
| `updated` | `boolean` | An original game poll completed for this player during this frame. |
| `available` | `boolean` | The last observed poll succeeded. This is not a hotplug query; an unpolled port may retain its last sample. |
| `buttons` | `SimpsonsInputButtons` | Controls held in the latest sample. |
| `pressed` | `SimpsonsInputButtons` | Transitions to held since the previous snapshot. Initial/reconnected samples do not create presses. |
| `released` | `SimpsonsInputButtons` | Transitions to released, including disconnects. A quick tap may appear in both pressed and released. |
| `left_stick` | `SimpsonsInputStick` |  |
| `right_stick` | `SimpsonsInputStick` |  |
| `left_trigger` | `number` | Analog pressure from 0 to 1. |
| `right_trigger` | `number` | Analog pressure from 0 to 1. |

## `SimpsonsInputPlayer`

A logical input assignment. Device metadata is read without polling controls; a configured device can differ from the last completed sample.

| Field | Type | Meaning |
| --- | --- | --- |
| `player` | `integer` | Logical player 0 or 1. |
| `assignment` | `string` | Configured physical-device id, auto or none. |
| `prompts` | `string` | Configured prompt override, including auto. |
| `prompt_family` | `'xbox'\|'playstation'\|'nintendo'\|'steamdeck'\|'keyboard'` | Resolved prompt style. |
| `device` | `string` | Active or selected physical-device id; empty if none is known. |
| `name` | `string` | Device display name. |
| `kind` | `'gamepad'\|'keyboard'\|'touch'\|'none'` | Device category. |

## `SimpsonsOwnedEntity`

A currently live object owned by the calling mod. Handles are transient.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | `string` | Use with entities.transform, entities.move and world.destroy; reacquire after reload or restore. |
| `class` | `string` | Loaded factory class name. |
| `tag` | `string` | Stable owner tag, or empty for an untagged object. |

## `SimpsonsEntityTransform`

World-space transform resolved through the original RenderWare frame getter. Basis vectors include scale and can be sheared.

| Field | Type | Meaning |
| --- | --- | --- |
| `position` | `SimpsonsVec3` | World position. |
| `right` | `SimpsonsVec3` | World X basis, including scale. |
| `up` | `SimpsonsVec3` | World Y basis, including scale. |
| `forward` | `SimpsonsVec3` | World Z basis, including scale. |

## `ModHealth`

A snapshot of a verified native health component. Healing, filtering and death behavior remain owned by the game.

| Field | Type | Meaning |
| --- | --- | --- |
| `current` | `number` | Current hit points. |
| `maximum` | `number` | Normal healing limit, not the separate low-health reference value. |
| `dead` | `boolean` | The component's native death predicate; some classes remain alive at zero hit points. |
| `enabled` | `boolean` | The component's native enabled flag. Other hit filters can still reject damage. |

## `ModHealthChange`

A completed native damage or healing request. An unchanged value can indicate an ignored hit; removal does not by itself establish a kill.

| Field | Type | Meaning |
| --- | --- | --- |
| `before` | `ModHealth` |  |
| `after` | `ModHealth\|nil` | Requeried after callbacks only while the same target lifetime is alive. |
| `removed` | `boolean` | The target lifetime ended during the native request. |

## `SimpsonsEntityRenderState`

Native visibility requests for a mod-owned object. Scene membership does not establish whether a camera sees it.

| Field | Type | Meaning |
| --- | --- | --- |
| `hidden_by_mod` | `boolean` | This owner currently contributes one native hide request. |
| `hide_count` | `integer` | Total native hide requests, including authored scripts; 0 through 255. |
| `has_model` | `boolean` | The entity has a readable native model. |
| `scene_attached` | `boolean` | The native spatial node is attached to the scene. Camera culling and occlusion are independent. |

## `SimpsonsSpawnTemplate`

A resident native entity definition or live template, not a cross-level asset-loading request. Display labels may change when the resource library finishes; use the current handle for spawning. seen may be zero for retained definitions.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | `string` | Temporary template handle. |
| `name` | `string` | Display label; ambiguous variants include a source suffix until metadata resolves them. |
| `category` | `string` |  |
| `class` | `string` | Original readable entity class; it does not prove natural combat behavior. |
| `model_resource` | `string` | Native model resource GUID (32 hexadecimal digits), or an empty string when unsupported. Query assets.search to find matching source metadata. |
| `seen` | `integer` | Observed live source instances for this native definition; zero for a retained definition without a live source object. |
| `source_guid` | `string` | Original definition GUID, matching assets.templates. It is a source identity, not a live guest pointer or persistent template handle. |
| `source_stream` | `string` | Loaded native file owning this definition, or empty if unproven. |

## `ModCameraRotation`

Angles in degrees. Positive yaw turns toward +X from +Z; positive pitch looks down.

| Field | Type | Meaning |
| --- | --- | --- |
| `pitch` | `number` | -90..90 degrees. |
| `yaw` | `number` | Normalized to -180..180 degrees. |
| `roll` | `number` | Normalized to -180..180 degrees. |

## `ModCameraPose`

A native viewport 0 view. Y is up; positions are world coordinates. This record contains no guest pointers or handles.

| Field | Type | Meaning |
| --- | --- | --- |
| `position` | `SimpsonsVec3` | World position. |
| `rotation` | `ModCameraRotation` | Pitch, yaw and roll in degrees. |
| `fov_degrees` | `number` | Vertical field of view, 1..120 degrees. |

## `SimpsonsAssetObject`

An original compiled object definition linked to an indexed model, character bank or mesh part. A metadata GUID is never a guest address or spawn handle.

| Field | Type | Meaning |
| --- | --- | --- |
| `source_guid` | `string` | Exact original definition GUID, independent of live instances. |
| `stream` | `string` | Relative native source archive; pass to assets.load_source. |
| `class_hash` | `integer` | Original registered native class hash. |
| `class` | `string` | Reviewed placeable class name, or empty when unsupported. |
| `supported` | `boolean` | Whether this class has a native spawn adapter in the current runtime. |
| `name` | `string` | Readable model label, available before source loading. |
| `model_resource` | `string` | Native MetaModel GUID when known. |
| `resident_template` | `string` | Current world.spawn handle once fully resident, otherwise empty. Reacquire after source loading, teardown or restore. |

## `SimpsonsAssetTemplates`

A bounded page of original object definitions for one asset.

| Field | Type | Meaning |
| --- | --- | --- |
| `ready` | `boolean` | The asynchronous game-wide index is ready. |
| `total` | `integer` | Number of linked original definitions before paging. |
| `items` | `SimpsonsAssetObject[]` | Definitions in this page, including unsupported classes for inspection. |

## `SimpsonsSourceStatus`

A source group and the calling mod generation's ownership. Several owners may retain the same additional group.

| Field | Type | Meaning |
| --- | --- | --- |
| `phase` | `string` | idle, preparing, loading, ready, releasing or failed. |
| `stream` | `string` | This generation's claimed relative path, otherwise empty. |
| `owned` | `boolean` | This generation currently retains a claim. |
| `ready` | `boolean` | This generation owns the matching fully loaded group. |
| `message` | `string` | Current preparation, native load, retirement or failure detail. |
| `files` | `integer` | Additional native streams in the group. |
| `loaded` | `integer` | Streams whose native loads have completed. |
| `definitions` | `integer` | Retained original object definitions. |
| `streams` | `string[]` | Every committed archive in the shared session. |
| `pending_stream` | `string` | The current addition, otherwise empty. Existing owners can remain ready while it loads. |

## `SimpsonsSceneSource`

An original compiled definition, rather than a live object address.

| Field | Type | Meaning |
| --- | --- | --- |
| `guid` | `string` |  |
| `class` | `string` |  |
| `stream` | `string` |  |

## `SimpsonsSceneObject`

A portable scene object with a stable owner tag.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | `string` |  |
| `name` | `string` |  |
| `source` | `SimpsonsSceneSource` |  |
| `transform` | `SimpsonsEntityTransform` |  |
| `radius` | `number` |  |

## `SimpsonsSceneDocument`

A validated scene of up to 64 original native objects in one episode.

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | `integer` |  |
| `name` | `string` |  |
| `level_root` | `string` |  |
| `autoload` | `boolean` |  |
| `objects` | `SimpsonsSceneObject[]` |  |

## `SimpsonsSceneProject`

A frozen package scene and source revision for saving creator drafts.

| Field | Type | Meaning |
| --- | --- | --- |
| `document` | `SimpsonsSceneDocument` |  |
| `fingerprint` | `string` |  |

## `SimpsonsProjectDocument`

Frozen creator metadata. kind selects a scene or mission document; handles never appear in authored documents.

| Field | Type | Meaning |
| --- | --- | --- |
| `document` | `table` |  |
| `fingerprint` | `string` |  |

## `SimpsonsMissionDocument`

Portable mission authoring data for one episode, with up to 32 reach/stay/wait objectives. Progress is saved separately by the mission service.

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | `integer` |  |
| `name` | `string` |  |
| `description` | `string` |  |
| `level_root` | `string` |  |
| `revision` | `integer` |  |
| `time_limit` | `number` |  |
| `fail_on_defeat` | `boolean` |  |
| `auto_start` | `boolean` |  |
| `objectives` | `SimpsonsMissionPoint[]` |  |

## `SimpsonsMissionPoint`

Original-world point and stable objective identity. requires lists prerequisite objective ids; stay requires the same controlled player to remain nearby continuously.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | `string` |  |
| `title` | `string` |  |
| `kind` | `string` |  |
| `position` | `SimpsonsVec3` |  |
| `radius` | `number` |  |
| `seconds` | `number` |  |
| `requires` | `string[]` |  |
| `optional` | `boolean` |  |
| `checkpoint` | `boolean` |  |

## `SimpsonsMissionAdvance`

One objective progress increment in an atomic authored-mission update.

| Field | Type | Meaning |
| --- | --- | --- |
| `objective` | `string` |  |
| `amount` | `number` |  |

## Events

Register handlers during module initialization with `game.on(name, callback)`.

- `initialize`: Register menus and services; gameplay mutations wait until a running level.
- `frame`: Every host update; display redraws do not advance simulation timers.
- `simulation`: The original simulation advanced; seconds is its delta.
- `level.ready`: A level has begun; reacquire temporary game handles.
- `level.unloading`: The current world is retiring; discard temporary handles.
- `ui.action`: A mod-owned menu row was activated with its logical player.
- `save.restored`: Host state was restored; kind is exact or checkpoint.
- `state.loaded`: Validate or migrate game.state and refresh staged menus before commit; kind is exact, checkpoint or reload. Gameplay and persistent storage mutations are blocked. An error rejects the prepared replacement.
- `code.reloaded`: Verified code replaced the prior instances. JSON state and timer delays are retained; reacquire temporary game handles. level_ready reports whether a level is running.
- `timer`: A simulation timer belonging to this mod elapsed; id, data, skipped and frame identify it.
- `mission.changed`: This mod's authored mission changed. Data contains mission, change and revision. Changes coalesce per mission, rotate through a shared frame budget, and are delivered after the calling callback; pending notifications survive saves and reloads.
- `entity.removed`: An owned object was removed by the game or through world.destroy. Contains entity (now stale) and reason (game or removed). Delivered to its owning runtime generation after cleanup; use level.unloading for world teardown. Requery world.owned rather than saving handles.
- `entity.died`: The original health death handler ran for an owned object. Contains entity (possibly already stale). Delivered once per object lifetime to its owning runtime generation, before removal notices and after native callbacks return. This is a native death signal, not an inference from disappearance. world.destroy does not count as a kill. Reacquire owned handles after reload; use level.unloading for world teardown.
