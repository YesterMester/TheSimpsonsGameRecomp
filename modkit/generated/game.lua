---@meta
-- Generated from modkit/api.json.
game = {}

---A single host API result. Only typed host failures are recoverable; this does not protect a Lua callback.
---@class SimpsonsCallResult
---@field ok boolean
---@field value any The target's return value on success, including nil.
---@field code string? Stable failure code when ok is false.
---@field error string? Readable failure description when ok is false.

---A three-dimensional vector in the game's coordinates: X/Z ground plane, Y height.
---@class SimpsonsVec3
---@field x number
---@field y number
---@field z number

---A loaded family character. Handles are transient; family order and controller assignment are independent.
---@class SimpsonsPlayer
---@field id string Temporary actor handle; reacquire after unloading, deletion or restore.
---@field slot integer Index in the loaded family list, not an input assignment.
---@field input_player integer|nil Logical port 0/1, or nil for AI, unbound or ambiguous ownership.
---@field character string
---@field health number
---@field max_health number
---@field position SimpsonsVec3

---Logical game controls. These names stay the same when controller glyphs change. Trigger buttons become held above 30/255; not every device exposes guide.
---@class SimpsonsInputButtons
---@field a boolean
---@field b boolean
---@field x boolean
---@field y boolean
---@field dpad_up boolean
---@field dpad_down boolean
---@field dpad_left boolean
---@field dpad_right boolean
---@field start boolean
---@field back boolean
---@field left_stick boolean
---@field right_stick boolean
---@field left_shoulder boolean
---@field right_shoulder boolean
---@field guide boolean
---@field left_trigger boolean
---@field right_trigger boolean

---Normalized routed stick values, without an extra modkit dead zone.
---@class SimpsonsInputStick
---@field x number Left -1, centered 0, right 1.
---@field y number Down -1, centered 0, up 1.

---One logical player in the current modkit frame. Do not store physical input in saved game.state.
---@class SimpsonsInputSnapshot
---@field player integer Logical player 0 or 1; not a family-list index.
---@field frame integer Monotonic snapshot frame; both players share it.
---@field sampled boolean An original game poll has been observed since the last reset.
---@field updated boolean An original game poll completed for this player during this frame.
---@field available boolean The last observed poll succeeded. This is not a hotplug query; an unpolled port may retain its last sample.
---@field buttons SimpsonsInputButtons Controls held in the latest sample.
---@field pressed SimpsonsInputButtons Transitions to held since the previous snapshot. Initial/reconnected samples do not create presses.
---@field released SimpsonsInputButtons Transitions to released, including disconnects. A quick tap may appear in both pressed and released.
---@field left_stick SimpsonsInputStick
---@field right_stick SimpsonsInputStick
---@field left_trigger number Analog pressure from 0 to 1.
---@field right_trigger number Analog pressure from 0 to 1.

---A logical input assignment. Device metadata is read without polling controls; a configured device can differ from the last completed sample.
---@class SimpsonsInputPlayer
---@field player integer Logical player 0 or 1.
---@field assignment string Configured physical-device id, auto or none.
---@field prompts string Configured prompt override, including auto.
---@field prompt_family 'xbox'|'playstation'|'nintendo'|'steamdeck'|'keyboard' Resolved prompt style.
---@field device string Active or selected physical-device id; empty if none is known.
---@field name string Device display name.
---@field kind 'gamepad'|'keyboard'|'touch'|'none' Device category.

---A currently live object owned by the calling mod. Handles are transient.
---@class SimpsonsOwnedEntity
---@field id string Use with entities.transform, entities.move and world.destroy; reacquire after reload or restore.
---@field class string Loaded factory class name.
---@field tag string Stable owner tag, or empty for an untagged object.

---World-space transform resolved through the original RenderWare frame getter. Basis vectors include scale and can be sheared.
---@class SimpsonsEntityTransform
---@field position SimpsonsVec3 World position.
---@field right SimpsonsVec3 World X basis, including scale.
---@field up SimpsonsVec3 World Y basis, including scale.
---@field forward SimpsonsVec3 World Z basis, including scale.

---A snapshot of a verified native health component. Healing, filtering and death behavior remain owned by the game.
---@class ModHealth
---@field current number Current hit points.
---@field maximum number Normal healing limit, not the separate low-health reference value.
---@field dead boolean The component's native death predicate; some classes remain alive at zero hit points.
---@field enabled boolean The component's native enabled flag. Other hit filters can still reject damage.

---A completed native damage or healing request. An unchanged value can indicate an ignored hit; removal does not by itself establish a kill.
---@class ModHealthChange
---@field before ModHealth
---@field after ModHealth|nil Requeried after callbacks only while the same target lifetime is alive.
---@field removed boolean The target lifetime ended during the native request.

---Native visibility requests for a mod-owned object. Scene membership does not establish whether a camera sees it.
---@class SimpsonsEntityRenderState
---@field hidden_by_mod boolean This owner currently contributes one native hide request.
---@field hide_count integer Total native hide requests, including authored scripts; 0 through 255.
---@field has_model boolean The entity has a readable native model.
---@field scene_attached boolean The native spatial node is attached to the scene. Camera culling and occlusion are independent.

---A resident native entity definition or live template, not a cross-level asset-loading request. Display labels may change when the resource library finishes; use the current handle for spawning. seen may be zero for retained definitions.
---@class SimpsonsSpawnTemplate
---@field id string Temporary template handle.
---@field name string Display label; ambiguous variants include a source suffix until metadata resolves them.
---@field category string
---@field class string Original readable entity class; it does not prove natural combat behavior.
---@field model_resource string Native model resource GUID (32 hexadecimal digits), or an empty string when unsupported. Query assets.search to find matching source metadata.
---@field seen integer Observed live source instances for this native definition; zero for a retained definition without a live source object.
---@field source_guid string Original definition GUID, matching assets.templates. It is a source identity, not a live guest pointer or persistent template handle.
---@field source_stream string Loaded native file owning this definition, or empty if unproven.

---Angles in degrees. Positive yaw turns toward +X from +Z; positive pitch looks down.
---@class ModCameraRotation
---@field pitch number -90..90 degrees.
---@field yaw number Normalized to -180..180 degrees.
---@field roll number Normalized to -180..180 degrees.

---A native viewport 0 view. Y is up; positions are world coordinates. This record contains no guest pointers or handles.
---@class ModCameraPose
---@field position SimpsonsVec3 World position.
---@field rotation ModCameraRotation Pitch, yaw and roll in degrees.
---@field fov_degrees number Vertical field of view, 1..120 degrees.

---An original compiled object definition linked to an indexed model, character bank or mesh part. A metadata GUID is never a guest address or spawn handle.
---@class SimpsonsAssetObject
---@field source_guid string Exact original definition GUID, independent of live instances.
---@field stream string Relative native source archive; pass to assets.load_source.
---@field class_hash integer Original registered native class hash.
---@field class string Reviewed placeable class name, or empty when unsupported.
---@field supported boolean Whether this class has a native spawn adapter in the current runtime.
---@field name string Readable model label, available before source loading.
---@field model_resource string Native MetaModel GUID when known.
---@field resident_template string Current world.spawn handle once fully resident, otherwise empty. Reacquire after source loading, teardown or restore.

---A bounded page of original object definitions for one asset.
---@class SimpsonsAssetTemplates
---@field ready boolean The asynchronous game-wide index is ready.
---@field total integer Number of linked original definitions before paging.
---@field items SimpsonsAssetObject[] Definitions in this page, including unsupported classes for inspection.

---A source group and the calling mod generation's ownership. Several owners may retain the same additional group.
---@class SimpsonsSourceStatus
---@field phase string idle, preparing, loading, ready, releasing or failed.
---@field stream string This generation's claimed relative path, otherwise empty.
---@field owned boolean This generation currently retains a claim.
---@field ready boolean This generation owns the matching fully loaded group.
---@field message string Current preparation, native load, retirement or failure detail.
---@field files integer Additional native streams in the group.
---@field loaded integer Streams whose native loads have completed.
---@field definitions integer Retained original object definitions.
---@field streams string[] Every committed archive in the shared session.
---@field pending_stream string The current addition, otherwise empty. Existing owners can remain ready while it loads.

---An original compiled definition, rather than a live object address.
---@class SimpsonsSceneSource
---@field guid string
---@field class string
---@field stream string

---A portable scene object with a stable owner tag.
---@class SimpsonsSceneObject
---@field id string
---@field name string
---@field source SimpsonsSceneSource
---@field transform SimpsonsEntityTransform
---@field radius number

---A validated scene of up to 64 original native objects in one episode.
---@class SimpsonsSceneDocument
---@field schema integer
---@field name string
---@field level_root string
---@field autoload boolean
---@field objects SimpsonsSceneObject[]

---A frozen package scene and source revision for saving creator drafts.
---@class SimpsonsSceneProject
---@field document SimpsonsSceneDocument
---@field fingerprint string

---Frozen creator metadata. kind selects a scene or mission document; handles never appear in authored documents.
---@class SimpsonsProjectDocument
---@field document table
---@field fingerprint string

---Portable mission authoring data for one episode, with up to 32 reach/stay/wait objectives. Progress is saved separately by the mission service.
---@class SimpsonsMissionDocument
---@field schema integer
---@field name string
---@field description string
---@field level_root string
---@field revision integer
---@field time_limit number
---@field fail_on_defeat boolean
---@field auto_start boolean
---@field objectives SimpsonsMissionPoint[]

---Original-world point and stable objective identity. requires lists prerequisite objective ids; stay requires the same controlled player to remain nearby continuously.
---@class SimpsonsMissionPoint
---@field id string
---@field title string
---@field kind string
---@field position SimpsonsVec3
---@field radius number
---@field seconds number
---@field requires string[]
---@field optional boolean
---@field checkpoint boolean

---One objective progress increment in an atomic authored-mission update.
---@class SimpsonsMissionAdvance
---@field objective string
---@field amount number

game.assets = {}
game.camera = {}
game.characters = {}
game.entities = {}
game.input = {}
game.locale = {}
game.log = {}
game.missions = {}
game.mods = {}
game.physics = {}
game.players = {}
game.projects = {}
game.scenes = {}
game.scores = {}
game.settings = {}
game.storage = {}
game.system = {}
game.text = {}
game.timers = {}
game.ui = {}
game.world = {}

---Read this mod package's validated scene and frozen source fingerprint without filesystem access. An absent scene returns an empty authoring document. Scene objects use original definition GUIDs, stream paths, stable per-owner tags and world poses, never temporary handles.
---@return SimpsonsSceneProject
function game.scenes.document() end

---Validate and canonicalize a portable scene document. Enforces unique tags, an unambiguous episode root, original GUID/class/source identities, bounded nonsingular float32 poses, radius limits and at most 64 objects. Does not spawn or change storage.
---@param document table
---@return SimpsonsSceneDocument
function game.scenes.validate(document) end

---Queue this mod's validated scene draft in persistent storage, with its frozen source fingerprint. Check storage.status for completion. The launcher's Apply scene draft action copies a matching creator-Test draft into the editable package before export. This does not write code or replace game files.
---@param document table
---@return SimpsonsSceneDocument
function game.scenes.save(document) end

---Find this generation's live owned object by its stable tag. Returns a fresh entity handle or nil; pending removals are excluded. Tags survive supported code reload and exact restore. This capability identifies builds that support tagged world.spawn.
---@param tag string
---@return string?
function game.world.tagged(tag) end

---Schedule or replace this mod's simulation timer. Pauses and menu redraws do not advance it. A timer event carries id, data and skipped intervals; repeats coalesce after a slow frame instead of catching up in a loop. Timers are saved with mod state.
---@param id string
---@param seconds number
---@param interval? number
---@param data? any
function game.timers.schedule(id, seconds, interval, data) end

---Cancel a timer owned by this mod. Returns whether it was pending.
---@param id string
function game.timers.cancel(id) end

---List this mod's pending timers, remaining simulation seconds, repeat intervals and event data.
function game.timers.list() end

---Read a loaded game text ID as UTF-8, including active native language packs. IDs are listed by the translation exporter; unloaded entries report found=false.
---@param id string
function game.text.lookup(id) end

---Search the game-wide resource library by category, name or a 32-digit native resource_id. Locations include native resource GUIDs when the named header provides one. Indexing is asynchronous; asset identities are not loaded entity templates.
---@param category? string
---@param text? string
---@param offset? number
---@param limit? number
---@param resource_id? string
function game.assets.search(category, text, offset, limit, resource_id) end

---List original object definitions using an indexed asset, including native model/character references and source stream paths. Friendly names are available before loading. A supported definition becomes a resident_template after its source is ready; pass that current handle to world.spawn. Geometry references can have several original object variants.
---@param asset string
---@param offset? number
---@param limit? number
---@return SimpsonsAssetTemplates
function game.assets.templates(asset, offset, limit) end

---Asynchronously add a source archive and its original native dependencies to a shared retained session. Up to eight archives can coexist; additions are serialized and a failed addition rolls back without removing earlier objects or resources. Each generation leases the whole session, with its latest requested root reported by source_status. Poll readiness, then reacquire a resident handle from assets.templates or world.catalog. Source actors stay inactive. Require assets.source_streams for multi-source capability on older builds.
---@param stream string
---@return boolean
function game.assets.load_source(stream) end

---Report this generation's selected source, readiness and request errors. Existing sources stay ready while another owner adds an archive. streams lists every committed archive; pending_stream names the current addition. Native progress counts the shared session. A failed addition restores preceding leases and reports failure only to its waiting owners.
---@return SimpsonsSourceStatus
function game.assets.source_status() end

---List committed archive paths in the shared native source session. This capability identifies builds that support combining sources. Shared dependencies remain resident until the last session owner releases; individual archives are not independently unloaded.
---@return string[]
function game.assets.source_streams() end

---Withdraw only this generation's pending addition and restore its preceding lease, including one acquired by a successful spawn from an old source. Existing objects and archives stay loaded. Native rollback starts when every waiting owner withdraws. Does not release a committed session; use release_source for that.
---@return boolean
function game.assets.cancel_source() end

---Release this generation's whole source-session lease. Original native retirement runs after the last owner releases, removing the session's objects and archives together. Other owners keep all retained resources. Failed additions expire only their own temporary data. Reload and exact saves retain committed roots and claims; checkpoint/world changes discard them.
---@return boolean
function game.assets.release_source() end

---Return the active mod language code, such as en or pt-br.
function game.locale.current() end

---Look up this mod's localized text with regional and source-language fallback; substitute named {values}. Button tags remain dynamic.
---@param key string
---@param values? table
function game.locale.text(key, values) end

---Describe every implemented operation and its arguments.
function game.system.describe() end

---List loaded mods, creator credits, errors and callback costs.
function game.mods.status() end

---Invoke one named host API with a named-argument object and return {ok=true,value=...} or {ok=false,code=...,error=...} for an expected host failure. Target validation, ownership, save read-only and preflight restrictions still apply. Cannot wrap itself or run a Lua function; interpreter errors and callback limits remain fatal. Use this for expected spawn or transform rejection, rather than pcall (which the sandbox disables).
---@param method string
---@param arguments table
---@return SimpsonsCallResult
function game.system.try_call(method, arguments) end

---Write a message attributed to this mod in the game log.
---@param message string
function game.log.info(message) end

---Read this mod's cached persistent JSON; missing files return nil. Storage is loaded before callbacks start.
---@param path string
function game.storage.read(path) end

---Queue an atomic JSON write to this mod's persistent storage without blocking the game thread. Reads see the queued value immediately; use storage.status to check completion and errors.
---@param path string
---@param value any
function game.storage.write(path, value) end

---Report this mod's pending persistent writes and disk errors. Pending zero with no errors means all writes have reached the filesystem.
function game.storage.status() end

---Get the current level readiness, location and world epoch. level_root is the unique loaded episode catalog excluding retained foreign sources; empty means it cannot be proven, so portable scenes must wait. streaming reports unfinished native file/manager work; scene autoload waits for it before requesting additional sources.
function game.world.status() end

---List the loaded family, including AI actors, with temporary handles, character names, positions, health and input_player. A slot is a family-list index; input_player is logical port 0/1 or null for AI, unbound or ambiguous ownership.
---@return SimpsonsPlayer[]
function game.players.list() end

---Get the family character currently bound to logical input player 0 or 1, or nil when no level/actor is ready or ownership is ambiguous. Requery during simulation after character switches, co-op joins, dropouts and restores; a previous actor handle may still identify a live AI character. This reads the game binding without polling devices. Paused or temporarily blocked controls do not change the binding.
---@param player number
---@return SimpsonsPlayer|nil
function game.players.controlled(player) end

---Move one loaded player using the game's two world transforms.
---@param player string
---@param position table
function game.players.teleport(player, position) end

---Refill a living family member through the original healing message and recovery callbacks, up to its normal healing limit. Does not resurrect a dead actor.
---@param player string
function game.players.refill_health(player) end

---Request family damage protection for this mod. The latest active request wins; retirement restores the previous owner or manual value. Manual controls supersede older requests.
---@param enabled boolean
function game.players.god_mode(enabled) end

---Read a named ScoreKeeper score. A missing score currently follows the game's zero result.
---@param name string
function game.scores.get(name) end

---Set a named score through ScoreKeeper, including its original watchers.
---@param name string
---@param value number
function game.scores.set(name, value) end

---Issue a named ScoreKeeper event through the original game logic.
---@param name string
function game.scores.issue(name) end

---Request the native development camera in a running level. This mod owns its request; retirement reveals the preceding request or manual state.
---@param enabled boolean
function game.camera.free(enabled) end

---Request photo mode with its pause and HUD handling. Camera requests are retired with this mod; checkpoints discard old-world camera requests.
---@param enabled boolean
function game.camera.photo(enabled) end

---Read the current free-camera and photo-mode state.
function game.camera.status() end

---Read viewport 0's last applied native camera pose, or nil when no supported level camera is running. Coordinates use Y up; rotation and vertical fov_degrees are in degrees. A set_pose request appears here after the next native camera update.
---@return ModCameraPose|nil
function game.camera.pose() end

---Request an owned viewport 0 camera pose through the original native camera writer and enable the free camera. Position axes are finite and within +/-1000000, pitch is -90..90 degrees, yaw/roll are finite within +/-360000 degrees and normalize to one turn, and vertical fov_degrees is 1..120. Photo mode keeps its current value. Manual camera input takes control; release/retirement restores the preceding explicit request or captured manual baseline. Checkpoints discard old-world requests.
---@param pose ModCameraPose
---@return ModCameraPose
function game.camera.set_pose(pose) end

---Release only this mod generation's camera request, revealing the preceding owner or captured manual baseline. Other owned services remain active. Repeating release is safe.
function game.camera.release() end

---List the character templates the existing character selector supports.
function game.characters.list() end

---Apply character templates to both players using the existing checkpoint restart.
---@param player1 string
---@param player2 string
function game.characters.apply(player1, player2) end

---List live and retained native entity templates from fully loaded source streams. Distinct definition records stay separate even when class, model and display names match. Each record includes class, seen (live source count, possibly zero) and model_resource (a native GUID or an empty string for unsupported layouts). The native registry supplies additional observed placeable classes. Plain catalog queries do not start disk scans. Retained handles expire with their source load or world, rather than requiring a live source instance. source_stream names the exact loaded native file that owns the original definition; an empty path cannot be exported as a portable scene source.
---@return SimpsonsSpawnTemplate[]
function game.world.catalog() end

---Clone a validated resident template with the original factory, unique world registration, post-spawn initialization and class position setter. Position is in world coordinates. Unsupported transforms fail without scanning object memory. The calling mod owns the object; reload transfers ownership and retirement queues safe game-thread cleanup. Reacquire handles with world.owned after reload or exact restore. Optional tags are unique per mod generation, including pending removal, and survive reload/exact restore. Require world.tagged when using tags.
---@param template string
---@param position table
---@param radius? number
---@param tag? string
function game.world.spawn(template, position, radius, tag) end

---List this mod generation's live spawned objects as id/class records. Excludes pending removals. Requery after reload or exact restore; checkpoints start a new world and mods recreate their objects. Each record also includes tag (empty for untagged objects).
---@return SimpsonsOwnedEntity[]
function game.world.owned() end

---Queue removal of an object spawned by this mod. Runs the original class/subscriber destructor after callbacks, including collision and world unregister. Returns true once, false if already queued; foreign or expired handles fail.
---@param entity string
---@return boolean
function game.world.destroy(entity) end

---Read a live object owned by this mod as a world-space position and right/up/forward basis. Resolves pending frame and parent updates through the original getter. Basis vectors retain scale; they are not Euler angles. Pending removals, foreign owners and expired handles fail.
---@param entity string
---@return SimpsonsEntityTransform
function game.entities.transform(entity) end

---Move this mod's live object to a finite world position while preserving its basis. Uses the class's original position setter and its controller/model notifications. Rotated, scaled and sheared parents are converted to local coordinates; singular or invalid parents fail before the setter. Does not sweep for collision clearance or stop an authored controller from moving it afterward.
---@param entity string
---@param position table
function game.entities.move(entity, position) end

---Set this mod's live object's complete world-space position and right/up/forward basis through the class's original matrix setter. Includes orientation, scale, cached native scale and model/controller notifications. Converts the whole matrix through rotated, scaled or sheared parents. Nonfinite, singular or native-length overflow/underflow bases reject before dispatch; invalid frames or unavailable setters fail without a raw-write fallback. Does not sweep for collision clearance or stop authored movement.
---@param entity string
---@param transform SimpsonsEntityTransform
function game.entities.set_transform(entity, transform) end

---Register or replace this mod's native Options page. Supports action, toggle, slider, select and text rows; actions arrive as ui.action events.
---@param page table
function game.ui.register_page(page) end

---Remove a native Options page owned by this mod.
---@param id string
function game.ui.remove_page(id) end

---Read physical assignments, device identities and resolved prompt families for logical players 0 and 1. prompts retains the configured override (including auto); prompt_family is its resolved family. This does not poll devices or map family-list entities to controllers.
---@return SimpsonsInputPlayer[]
function game.input.players() end

---Read the last routed game input for logical player 0 or 1. Buttons, pressed/released edges, normalized sticks and triggers are frozen for this modkit frame; reads do not poll or consume input. sampled says a poll has been observed; updated says it arrived this frame; available is the last poll's success, not a physical-connection query. First samples and reconnects establish a baseline without synthetic presses. Unpolled channels retain their last values without repeating edges. Read during simulation for gameplay actions; use ui.action for native menus.
---@param player number
---@return SimpsonsInputSnapshot
function game.input.state(player) end

---Read an existing runtime setting with its current value and restart metadata.
---@param name string
function game.settings.get(name) end

---Register or refresh this mod's authored mission definition during initialize or state.loaded. Objectives have stable ids, positive goals, prerequisites and optional flags. Live goal changes need a state migration or a project restart.
---@param definition table
function game.missions.register(definition) end

---List this mod's authored missions and objective progress. Other mods' missions remain isolated.
function game.missions.list() end

---Read an authored mission by its local id, including active/locked/completed objectives and retry-point availability.
---@param id string
function game.missions.get(id) end

---Start an idle authored mission while a level is loaded. Progress and notifications are saved with this mod. This does not replace the base campaign.
---@param id string
function game.missions.start(id) end

---Add nonnegative progress to an active, unlocked objective. Progress clamps to its goal; completing every required objective completes the mission. Changes coalesce into an owned mission.changed event.
---@param id string
---@param objective string
---@param amount number
function game.missions.advance(id, objective, amount) end

---Fail an active authored mission with a reason. It may then be retried from its recorded objective checkpoint or restarted from the beginning.
---@param id string
---@param reason string
function game.missions.fail(id, reason) end

---Record the active mission's current objective progress as a retry point. This records authored mission state; it does not save or reset the game world.
---@param id string
function game.missions.checkpoint(id) end

---Retry an authored mission. from_checkpoint defaults to true; false replays it from the beginning. Scripts must restore any associated world changes through their own supported APIs.
---@param id string
---@param from_checkpoint? boolean
function game.missions.restart(id, from_checkpoint) end

---During state.loaded only, validate and migrate this mod's saved mission service state from the event's missions field. Unhandled state is restored automatically after the callback. Invalid migrations reject the replacement before commit.
---@param state table
function game.missions.restore(state) end

---Read the original health component, or nil when it is absent or unsupported. maximum is the component's normal healing limit; dead follows its native death policy. Query players.controlled or players.list for a current family handle.
---@param player string
---@return ModHealth|nil
function game.players.health(player) end

---Apply scripted damage through the original eligibility, hit, event and death callbacks. Damage protection remains active; a rejected or absorbed hit can leave health unchanged. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Query players.controlled or players.list for a current family handle.
---@param player string
---@param amount number
---@return ModHealthChange
function game.players.damage(player, amount) end

---Heal through the original eligibility and recovery callbacks, capped at the normal healing limit. Dead actors are not resurrected. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Query players.controlled or players.list for a current family handle.
---@param player string
---@param amount number
---@return ModHealthChange
function game.players.heal(player, amount) end

---Read this mod's object's native hide count, model availability and scene membership. Null means its visibility layout is unsupported. Scene membership is not proof of camera visibility or finished NPC behavior.
---@param entity string
---@return SimpsonsEntityRenderState|nil
function game.entities.render_state(entity) end

---Hide or reveal this mod's object through the original visibility methods. The host balances one hide request; repeated calls are idempotent and reveal preserves the game's other requests. Reload transfers the request without applying it twice. A native force-show cancels it. Does not suspend AI, collision, audio or damage. Unsupported layouts fail before mutation; null means a class callback removed the object.
---@param entity string
---@param visible boolean
---@return SimpsonsEntityRenderState|nil
function game.entities.set_visible(entity, visible) end

---Read the original health component, or nil when it is absent or unsupported. maximum is the component's normal healing limit; dead follows its native death policy. Only this mod's active owned objects are accessible.
---@param entity string
---@return ModHealth|nil
function game.entities.health(entity) end

---Apply scripted damage through the original eligibility, hit, event and death callbacks. Damage protection remains active; a rejected or absorbed hit can leave health unchanged. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Only this mod's active owned objects are accessible.
---@param entity string
---@param amount number
---@return ModHealthChange
function game.entities.damage(entity, amount) end

---Heal through the original eligibility and recovery callbacks, capped at the normal healing limit. Dead actors are not resurrected. Amount is finite and between 0 and 1000000. The result includes before/after snapshots; after can be nil if native logic removes the health component or target. Only this mod's active owned objects are accessible.
---@param entity string
---@param amount number
---@return ModHealthChange
function game.entities.heal(entity, amount) end

---Read this package's frozen scene or mission document and source fingerprint. This uses prepared metadata without reading source files in a callback. Missing documents return an empty authoring document.
---@param kind string
---@return SimpsonsProjectDocument
function game.projects.document(kind) end

---Validate and canonicalize a portable scene or mission document. Mission graph rules use the native mission service without registering a live mission. Rejects unknown fields, temporary handles, invalid episode roots, cycles and out-of-bounds points or durations.
---@param kind string
---@param document table
function game.projects.validate(kind, document) end

---Queue a validated scene or mission draft in namespaced persistent storage, tied to this frozen source revision. Check storage.status for completion. Apply saved drafts in the launcher merges matching scene and mission drafts atomically before exporting the project.
---@param kind string
---@param document table
function game.projects.save(kind, document) end

---Remove this mod's mission definition, progress, retry point and pending change notification. Returns false if absent. This lets an editor stop a preview before changing its contract; it does not reset the game world. Allowed during private initialization/restore preflight; save callbacks remain read only.
---@param id string
---@return boolean
function game.missions.remove(id) end

---Atomically advance up to 32 distinct objectives that were already unlocked before the batch. Every id and amount validates before progress changes. Returns one mission snapshot and coalesces one notification, avoiding repeated full-state copies for simultaneous goals.
---@param id string
---@param updates SimpsonsMissionAdvance[]
function game.missions.advance_batch(id, updates) end

---Validate a scene and/or mission document before queuing one atomic project-drafts.json replacement. Both documents share the frozen source fingerprint. Use this for a project containing both editors, then wait for storage.status before applying saved drafts in the launcher.
---@param documents table
function game.projects.save_all(documents) end

---Lease experimental whole-character knockdown settings for this mod. Requires all switches enabled, run_impact, punch_impact, push_downed, player_enabled and force_scale (0.25 to 4). Pauses stop the effect; disabling, unloading or failing a mod releases its request. This is a root-motion prototype, not a jointed ragdoll or terrain collision solver.
---@param options table
function game.physics.knockdowns(options) end

---Release this mod's knockdown request, revealing the preceding owner or manual baseline. Call from normal game-thread callbacks, not initialization or restore preflight. Restoration of surviving character poses occurs on the game thread.
function game.physics.release_knockdowns() end

game.state = {}
---@param event 'initialize'|'frame'|'simulation'|'level.ready'|'level.unloading'|'ui.action'|'save.restored'|'state.loaded'|'code.reloaded'|'timer'|'mission.changed'|'entity.removed'|'entity.died'
---@param callback fun(data: table)
function game.on(event, callback) end
