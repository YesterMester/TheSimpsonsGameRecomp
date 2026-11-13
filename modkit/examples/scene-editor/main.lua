-- Modders: this editor uses public operations only. A scene keeps original
-- definition GUIDs, native classes, source paths and world poses. Temporary
-- entity/template handles stay in local variables and never enter game.state.
-- New projects are disabled. Use the launcher's Test action, open this page
-- through Options > Extras > Mods, then Save draft before closing the game.
local templates, template_index, template_page = {}, 1, 1
local definitions = {}
local template_indices = {}
local selected, undo, redo, pending = nil, {}, {}, nil
local message, step, turn_step, scale_step = "Load a level and choose a prop", 1, 15, 1.1
local ticks, saving, bound, last_message = 0, false, 0, nil
local page, refresh, begin_load
local MAX_OBJECTS, PAGE_SIZE = 64, 40

-- The original TV menu has single-row labels. Keep UTF-8 names readable
-- without wrapping into its next button; documents retain the full identity.
local function caption(text, limit)
    if utf8.len(text) <= limit then return text end
    return string.sub(text, 1, utf8.offset(text, limit - 2) - 1) .. "..."
end

local function copy(value)
    if type(value) ~= "table" then return value end
    local result = {}
    for k, v in pairs(value) do result[k] = copy(v) end
    return result
end
local function document() return game.state.scene end
local function object_index(id)
    for i, item in ipairs(document().objects) do if item.id == id then return i end end
end
local function item() local i = object_index(selected); return i and document().objects[i], i end
local function tagged(id) return game.world.tagged(id) end
local function same_source(a, b)
    return a.guid == b.guid and a.class == b.class
end
local function template_for(source)
    -- Reuse the current refresh, rather than converting hundreds of native
    -- catalog records to Lua again for every placement, duplicate and undo.
    -- These handles remain local and are rebuilt after loads and restores.
    return definitions[source.guid .. ":" .. source.class] or nil
end
local function fail(result)
    message = result.error or "The native operation could not finish"
    game.log.info((result.code or "scene") .. ": " .. message)
end
local function validate(value)
    local result = game.system.try_call("scenes.validate", {document = value})
    if not result.ok then fail(result); return end
    return result.value
end
local function history(before, after, index)
    undo[#undo + 1] = {before = copy(before), after = copy(after), index = index}
    if #undo > 64 then table.remove(undo, 1) end
    redo = {}
end
local function live_count()
    bound = 0
    if not game.world.status().ready then return end
    for _, object in ipairs(game.world.owned()) do if object_index(object.tag) then bound = bound + 1 end end
end
local function current_level()
    local world = game.world.status()
    if not world.ready or world.level_root == "" then
        message = "Wait for a level with a known episode catalog"
        return
    end
    if #document().objects > 0 and document().level_root ~= world.level_root then
        message = "This scene belongs to " .. document().level_root .. "; load it or start a new scene"
        return
    end
    return world
end
local function create(object)
    local template = template_for(object.source)
    if not template then return {ok = false, error = "Load the scene's original source before recreating this prop"} end
    local result = game.system.try_call("world.spawn", {template = template.id,
        position = object.transform.position, radius = object.radius, tag = object.id})
    if not result.ok then return result end
    local entity = result.value
    result = game.system.try_call("entities.set_transform", {entity = entity, transform = object.transform})
    if not result.ok then game.world.destroy(entity); return result end
    return {ok = true, value = entity}
end
local function next_id()
    repeat
        game.state.sequence = (game.state.sequence or 0) + 1
    until not object_index("scene-" .. game.state.sequence)
    return "scene-" .. game.state.sequence
end
local function record(template, position)
    return {id = next_id(), name = template.name, radius = 1,
        source = {guid = template.source_guid, class = template.class, stream = template.source_stream},
        transform = {position = position, right = {x = 1, y = 0, z = 0},
            up = {x = 0, y = 1, z = 0}, forward = {x = 0, y = 0, z = 1}}}
end
local function add(object)
    if #document().objects >= MAX_OBJECTS then message = "This scene already has 64 objects"; return end
    local world = current_level(); if not world then return end
    local candidate = copy(document()); candidate.level_root = world.level_root
    candidate.objects[#candidate.objects + 1] = copy(object)
    candidate = validate(candidate); if not candidate then return end
    local result = create(object)
    if not result.ok then fail(result); return end
    game.state.scene = candidate; selected = object.id
    history(nil, object, #candidate.objects)
    message = "Placed " .. object.name
    live_count()
end
local function edit(change)
    local object, index = item(); if not object then message = "Select a scene object first"; return end
    local entity = tagged(object.id); if not entity then message = "Load this scene's preview first"; return end
    local result = game.system.try_call("entities.transform", {entity = entity})
    if not result.ok then fail(result); return end
    local before, after = copy(object), copy(object)
    before.transform = result.value; after.transform = copy(result.value)
    change(after.transform)
    local candidate = copy(document()); candidate.objects[index] = after
    candidate = validate(candidate); if not candidate then return end
    result = game.system.try_call("entities.set_transform", {entity = entity, transform = after.transform})
    if not result.ok then fail(result); return end
    game.state.scene = candidate; history(before, after, index)
    message = "Updated " .. object.name
end
local function apply_history(entry, reverse)
    local target
    if reverse then target = entry.before else target = entry.after end
    local old
    if reverse then old = entry.after else old = entry.before end
    local id = (target or old).id
    local existing = tagged(id)
    if target and old then
        if not existing then message = "This object is no longer in the preview"; return false end
        local result = game.system.try_call("entities.set_transform", {entity = existing, transform = target.transform})
        if not result.ok then fail(result); return false end
    elseif target then
        if existing then message = "This scene object already exists"; return false end
        local result = create(target)
        if not result.ok then fail(result); return false end
    elseif existing then game.world.destroy(existing) end
    local index = object_index(id)
    if target then
        if index then document().objects[index] = copy(target)
        else table.insert(document().objects, math.min(entry.index, #document().objects + 1), copy(target)) end
        selected = id
    elseif index then
        table.remove(document().objects, index)
        selected = document().objects[math.min(index, #document().objects)]
        selected = selected and selected.id
    end
    live_count()
    return true
end
local function unload()
    if pending then
        game.assets.cancel_source()
        pending = nil
    end
    for _, value in ipairs(game.world.owned()) do if object_index(value.tag) then game.world.destroy(value.id) end end
    game.assets.release_source()
    templates, definitions = {}, {}
    bound, undo, redo = 0, {}, {}
    message = "Preview unloaded; the authored scene is kept"
end
local function rollback(reason)
    for _, id in ipairs(pending.created) do
        local entity = tagged(id)
        if entity then game.world.destroy(entity) end
    end
    game.assets.cancel_source()
    templates, definitions = {}, {}
    pending = nil
    live_count()
    if bound == 0 then game.assets.release_source() end
    message = reason
end
begin_load = function()
    local world = current_level(); if not world then return end
    if #document().objects == 0 then message = "This scene is empty"; return end
    pending = {index = 1, created = {}, requested = nil}
    undo, redo = {}, {}
    message = "Loading scene objects and native resources"
end
refresh = function()
    templates, definitions, template_indices = {}, {}, {}
    if game.world.status().ready then
        for _, value in ipairs(game.world.catalog()) do
            local key = value.source_guid .. ":" .. value.class
            if definitions[key] ~= nil then definitions[key] = false
            else definitions[key] = value end
            -- Start with placeable props. The factory is not proof of an NPC's
            -- encounter, navigation, collision or scripted-scene prerequisites.
            if value.category == "Props" and value.source_stream and value.source_stream ~= ""
                and value.source_guid and value.source_guid ~= string.rep("0", 32) then
                templates[#templates + 1] = value
                template_indices[value.id] = #templates
            end
        end
        live_count()
    else bound = 0 end
    template_index = math.max(1, math.min(template_index, #templates))
    template_page = math.floor((template_index - 1) / PAGE_SIZE) + 1
end
page = function()
    local doc = document(); if not doc then return end
    local choices, objects = {}, {}
    local first = (template_page - 1) * PAGE_SIZE + 1
    for index = first, math.min(#templates, first + PAGE_SIZE - 1) do
        -- Choices carry the exact current template identity. Desktop tools
        -- can select it without rebuilding the whole catalog on every place.
        -- These handles stay local; portable scene records still use GUIDs.
        choices[#choices + 1] = {label = index .. ": " .. caption(templates[index].name, 20), value = templates[index].id}
    end
    if #choices == 0 then choices = {{label = "Refresh loaded props", value = 0}} end
    for index, object in ipairs(doc.objects) do
        objects[#objects + 1] = {label = index .. ": " .. caption(object.name, 20), value = object.id}
    end
    if #objects == 0 then objects = {{label = "No authored objects", value = ""}} end
    if not object_index(selected) then selected = doc.objects[1] and doc.objects[1].id end
    local object = item()
    local rows = {
        {id = "summary", type = "text", label = #doc.objects .. "/64 authored; " .. bound .. " in preview"},
        {id = "level", type = "text", label = doc.level_root ~= "" and caption(doc.level_root, 32) or "Place a prop to choose an episode"},
        {id = "refresh", label = "Refresh loaded props"},
        {id = "template", type = "select", label = "Prop", value = templates[template_index] and templates[template_index].id or 0, choices = choices},
        {id = "previous-page", label = "Previous template page"},
        {id = "next-page", label = "Next template page"},
        {id = "place", label = "Place beside my character"},
        {id = "object", type = "select", label = "Object", value = selected or "", choices = objects},
        {id = "step", type = "select", label = "Movement step", value = step, choices = {
            {label = "0.1", value = 0.1}, {label = "1", value = 1}, {label = "5", value = 5}, {label = "10", value = 10}}},
        {id = "bring", label = "Bring selected object here"}
    }
    if object then
        for _, axis in ipairs({"x", "y", "z"}) do
            rows[#rows + 1] = {id = "position-" .. axis, type = "slider", label = "Position " .. string.upper(axis),
                value = object.transform.position[axis], min = -1000000, max = 1000000, step = step}
        end
    end
    for _, row in ipairs({
        {id = "turn-left", label = "Rotate left 15 degrees"}, {id = "turn-right", label = "Rotate right 15 degrees"},
        {id = "grow", label = "Grow by 10%"}, {id = "shrink", label = "Shrink by 10%"},
        {id = "duplicate", label = "Duplicate selected object"}, {id = "remove", label = "Remove selected object"},
        {id = "undo", label = "Undo (" .. #undo .. ")"}, {id = "redo", label = "Redo (" .. #redo .. ")"},
        {id = "load", label = "Load scene preview"}, {id = "cancel", label = "Cancel scene loading"},
        {id = "unload", label = "Unload preview"},
        {id = "autoload", type = "toggle", label = "Load automatically in this episode", value = doc.autoload},
        {id = "save", label = "Save scene draft"}, {id = "new", label = "Start a new empty scene"},
        {id = "status", type = "text", label = caption(message, 32)}
    }) do rows[#rows + 1] = row end
    if message ~= last_message then game.log.info(message); last_message = message end
    game.ui.register_page({id = "scene", title = "Scene editor", rows = rows})
end
local function initialize()
    if not game.state.scene then
        local project = game.scenes.document()
        game.state.scene = project.document
        local draft = game.storage.read("scene-draft.json")
        if draft and draft.fingerprint == project.fingerprint then game.state.scene = game.scenes.validate(draft.scene) end
    end
    game.state.scene = game.scenes.validate(game.state.scene)
    game.state.sequence = game.state.sequence or 0
    page()
end
game.on("initialize", initialize)
game.on("state.loaded", function()
    game.state.scene = game.scenes.validate(game.state.scene)
    undo, redo, pending = {}, {}, nil
    page()
end)
game.on("level.ready", function()
    refresh(); page()
    local world = game.world.status()
    if document().autoload and document().level_root == world.level_root and #document().objects > 0 then begin_load() end
end)
game.on("level.unloading", function()
    -- Guest teardown owns the old world. Never issue destruction with expired
    -- handles or start loading sources from an unloading callback.
    templates, definitions, template_indices, undo, redo, pending, bound = {}, {}, {}, {}, {}, nil, 0
    message = "Scene kept; load its episode to preview it"
    page()
end)
local function rebound()
    -- Tags identify objects independently of temporary handles. Do not infer
    -- correspondence from list order or object counts after reload or restore.
    pending, undo, redo = nil, {}, {}
    refresh(); message = "Scene state restored; owned objects rebound by tag"; page()
end
game.on("code.reloaded", rebound)
game.on("save.restored", rebound)
game.on("entity.removed", function() live_count(); page() end)
game.on("frame", function()
    ticks = ticks + 1
    if saving and ticks % 15 == 0 then
        local status = game.storage.status()
        if status.pending == 0 then
            saving = false
            message = #status.errors == 0 and (game.state.mission_document
                and "Drafts saved; close Test, then Apply saved drafts"
                or "Draft saved; close Test, then Apply scene draft in the launcher") or "Draft write failed; check storage errors"
            page()
        end
    end
    if not pending then return end
    local object = document().objects[pending.index]
    if not object then pending = nil; refresh(); message = "Scene preview ready"; page(); return end
    if tagged(object.id) then pending.index = pending.index + 1; return end
    if pending.requested then
        local source = game.assets.source_status()
        if source.phase == "failed" then rollback(source.message); page(); return end
        if not source.ready then return end
        pending.requested = nil
        refresh()
        if not template_for(object.source) then rollback("The loaded archive has no matching definition and class"); page(); return end
    end
    if not template_for(object.source) then
        -- A first physics step can precede the final level IO completion.
        -- Keep the preview transaction intact while that work settles.
        if game.world.status().streaming then return end
        local result = game.system.try_call("assets.load_source", {stream = object.source.stream})
        if not result.ok and result.code == "busy" then return end
        if not result.ok then rollback(result.error); page(); return end
        pending.requested = object.source.stream
        message = "Loading " .. object.source.stream; page(); return
    end
    local result = create(object)
    if not result.ok then rollback(result.error); page(); return end
    pending.created[#pending.created + 1] = object.id
    pending.index = pending.index + 1
    live_count()
end)
game.on("ui.action", function(event)
    if event.page ~= "scene" then return end
    if event.row == "save" then
        -- A mission project shares this page. Save both documents in one
        -- storage replacement so neither editor can leave the other behind.
        local result
        if game.state.mission_document then
            result = game.system.try_call("projects.save_all", {documents = {scene = document(), mission = game.state.mission_document}})
            if result.ok then game.state.scene = result.value.scene end
        else
            result = game.system.try_call("scenes.save", {document = document()})
            if result.ok then game.state.scene = result.value end
        end
        if result.ok then saving = true; message = "Saving scene draft" else fail(result) end
        page(); return
    end
    if pending then
        if event.row == "cancel" then rollback("Scene load canceled; earlier preview objects are kept") end
        page(); return
    end
    if event.row == "step" then step = event.value; page(); return end
    if event.row == "object" then selected = event.value; page(); return end
    if event.row == "template" then
        local index = type(event.value) == "number" and event.value or template_indices[event.value]
        if index and templates[index] then template_index = index
        else message = "This template changed; refresh loaded props" end
        page(); return
    end
    if event.row == "autoload" then document().autoload = event.value; page(); return end
    if not game.world.status().ready then message = "Load a level first"; page(); return end
    if event.row == "refresh" then refresh()
    elseif event.row == "previous-page" or event.row == "next-page" then
        local pages = math.max(1, math.ceil(#templates / PAGE_SIZE))
        template_page = (template_page - 1 + (event.row == "next-page" and 1 or -1)) % pages + 1
        template_index = (template_page - 1) * PAGE_SIZE + 1
    elseif event.row == "unload" then unload()
    elseif event.row == "new" then
        unload(); game.state.scene = game.scenes.document().document
        game.state.scene.objects = {}; game.state.scene.level_root = ""; game.state.scene.autoload = false
        selected = nil; message = "New empty scene; Save draft replaces your previous draft"
    elseif not current_level() then page(); return
    elseif event.row == "load" then begin_load()
    elseif event.row == "place" then
        local actor, template = game.players.controlled(event.player), templates[template_index]
        if actor and template then
            local p = actor.position
            add(record(template, {x = p.x + 4, y = p.y + 1, z = p.z}))
        else message = "Choose a loaded prop and a controlled character" end
    elseif event.row == "duplicate" then
        local object = item()
        if object then
            local value = copy(object); value.id = next_id(); value.transform.position.x = value.transform.position.x + step
            add(value)
        end
    elseif event.row == "remove" then
        local object, index = item()
        if object then
            local entity = tagged(object.id); if entity then game.world.destroy(entity) end
            table.remove(document().objects, index); history(object, nil, index)
            selected = nil; message = "Removed " .. object.name; live_count()
        end
    elseif event.row == "undo" or event.row == "redo" then
        local source, target = event.row == "undo" and undo or redo, event.row == "undo" and redo or undo
        local entry = source[#source]
        if entry and apply_history(entry, event.row == "undo") then
            table.remove(source); target[#target + 1] = entry
            message = event.row == "undo" and "Edit undone" or "Edit reapplied"
        end
    elseif event.row == "bring" then
        local actor = game.players.controlled(event.player)
        if actor then edit(function(pose) local p = actor.position; pose.position = {x = p.x + 4, y = p.y + 1, z = p.z} end) end
    elseif string.sub(event.row, 1, 9) == "position-" then
        local axis = string.sub(event.row, 10)
        edit(function(pose) pose.position[axis] = event.value end)
    elseif event.row == "grow" or event.row == "shrink" or event.row == "turn-left" or event.row == "turn-right" then
        edit(function(pose)
            local angle = math.rad(event.row == "turn-left" and -turn_step or turn_step)
            local c, s = math.cos(angle), math.sin(angle)
            for _, name in ipairs({"right", "up", "forward"}) do
                local v = pose[name]
                if event.row == "grow" or event.row == "shrink" then
                    local factor = event.row == "grow" and scale_step or 1 / scale_step
                    pose[name] = {x = v.x * factor, y = v.y * factor, z = v.z * factor}
                else pose[name] = {x = v.x * c + v.z * s, y = v.y, z = v.z * c - v.x * s} end
            end
        end)
    end
    page()
end)
