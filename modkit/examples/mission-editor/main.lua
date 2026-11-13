-- Modders: build routes in the original Options menu, then edit their names
-- and rules in the launcher's Mission settings. The scene editor is the same
-- public example, included as a frozen Lua module instead of a second copy
-- of its implementation. Both documents export in one credited package.
require("scene")

local id = "authored-mission"
local selected, prerequisite, step = nil, "", 1
local undo, redo, ticks, page_dirty = {}, {}, 0, false
local saving, pending_start, message, last_message = false, false, "Add an objective in a loaded level", nil
local page, start, synchronize

local function copy(value)
    if type(value) ~= "table" then return value end
    local result = {}; for k, v in pairs(value) do result[k] = copy(v) end
    return result
end
local function caption(value, limit)
    if utf8.len(value) <= limit then return value end
    return string.sub(value, 1, utf8.offset(value, limit - 2) - 1) .. "..."
end
local function doc() return game.state.mission_document end
local function index_of(key)
    for i, value in ipairs(doc().objectives) do if value.id == key then return i end end
end
local function item() local i = index_of(selected); return i and doc().objectives[i], i end
local function mission()
    local result = game.system.try_call("missions.get", {id = id})
    return result.ok and result.value or nil
end
local function failure(result)
    message = result.error or "The operation could not finish"
    game.log.info((result.code or "mission") .. ": " .. message)
end
local function definition(document)
    local result = {id = id, title = document.name, description = document.description,
        revision = document.revision, objectives = {}}
    for _, value in ipairs(document.objectives) do
        result.objectives[#result.objectives + 1] = {id = value.id, title = value.title,
            goal = value.kind == "reach" and 1 or value.seconds, requires = value.requires, optional = value.optional}
    end
    return result
end
synchronize = function()
    if #doc().objectives > 0 then game.missions.register(definition(doc()))
    else game.missions.remove(id) end
end
local function editable()
    if game.state.preview then message = "Stop preview before editing its rules"; return false end
    return true
end
local function world()
    local value = game.world.status()
    if not value.ready or value.level_root == "" then message = "Load a known episode first"; return end
    if #doc().objectives > 0 and value.level_root ~= doc().level_root then
        message = "Load " .. doc().level_root .. " to edit this route"; return
    end
    return value
end
local function record(before)
    undo[#undo + 1] = before; if #undo > 64 then table.remove(undo, 1) end
    redo = {}
end
local function apply(candidate, remember)
    local checked = game.system.try_call("projects.validate", {kind = "mission", document = candidate})
    if not checked.ok then failure(checked); return false end
    -- A preview is explicitly stopped before replacing its native contract.
    -- Document validation uses the same native objective graph rules first.
    game.missions.remove(id)
    if remember then record(copy(doc())) end
    game.state.mission_document = checked.value
    game.state.elapsed, game.state.holds = 0, {}
    game.state.sample_seconds = 0
    game.state.checkpoint_elapsed, game.state.checkpoint_holds = 0, {}
    synchronize()
    return true
end
local function edit(change)
    if not editable() then return end
    local candidate = copy(doc())
    if candidate.revision >= 4294967295 then message = "Mission revision limit reached"; return end
    candidate.revision = candidate.revision + 1
    change(candidate)
    if apply(candidate, true) then message = "Mission draft updated" end
end
local function next_id()
    repeat game.state.objective_sequence = game.state.objective_sequence + 1
    until not index_of("point-" .. game.state.objective_sequence)
    return "point-" .. game.state.objective_sequence
end
local function add(player, duplicate)
    if not editable() then return end
    local current = world(); if not current then return end
    if #doc().objectives >= 32 then message = "A route has at most 32 objectives"; return end
    local actor = game.players.controlled(player)
    if not actor then message = "Assign a controller to a family member"; return end
    local original = item()
    local value = duplicate and original and copy(original) or {kind = "reach", position = copy(actor.position),
        radius = 3, seconds = 2, optional = false, checkpoint = false, requires = {}}
    value.id = next_id(); value.title = "Checkpoint " .. game.state.objective_sequence
    if original then value.requires = {original.id} end
    if duplicate then value.position.x = value.position.x + math.max(step, value.radius * 2 + 1) end
    edit(function(candidate)
        candidate.level_root = current.level_root
        candidate.objectives[#candidate.objectives + 1] = value
    end)
    if index_of(value.id) then selected = value.id end
end
local function scene_ready()
    local scene = game.state.scene
    if not scene or not scene.autoload or #scene.objects == 0 or scene.level_root ~= doc().level_root then return true end
    local tags = {}
    for _, value in ipairs(game.world.owned()) do tags[value.tag] = true end
    for _, value in ipairs(scene.objects) do if not tags[value.id] then return false end end
    return true
end
start = function(retry)
    if not world() then return false end
    if #doc().objectives == 0 then message = "Add an objective first"; return false end
    if not scene_ready() then message = "Wait for the scene preview to load"; return false end
    synchronize()
    local current = mission()
    if current.status == "idle" then game.missions.start(id)
    else game.missions.restart(id, retry == true) end
    game.state.preview = true
    game.state.elapsed = retry and current.has_checkpoint and game.state.checkpoint_elapsed or 0
    game.state.holds = retry and current.has_checkpoint and copy(game.state.checkpoint_holds) or {}
    if not (retry and current.has_checkpoint) then game.state.checkpoint_elapsed, game.state.checkpoint_holds = 0, {} end
    game.state.sample_seconds, pending_start = 0, false
    message = retry and "Preview retried" or "Preview started"
    return true
end
local function checkpoint()
    game.missions.checkpoint(id)
    game.state.checkpoint_elapsed = game.state.elapsed
    game.state.checkpoint_holds = copy(game.state.holds)
end
local function stop()
    game.missions.remove(id)
    game.state.preview, pending_start, game.state.sample_seconds = false, false, 0
    game.state.elapsed, game.state.holds = 0, {}
    game.state.checkpoint_elapsed, game.state.checkpoint_holds = 0, {}
    synchronize()
    message = "Preview stopped; edit the mission"
end
page = function()
    if not doc() then return end
    page_dirty = false
    if not index_of(selected) then selected = doc().objectives[1] and doc().objectives[1].id end
    local choices, parents = {}, {{label = "None", value = ""}}
    for i, value in ipairs(doc().objectives) do
        local label = i .. ": " .. caption(value.title, 20)
        choices[#choices + 1] = {label = label, value = value.id}
        if value.id ~= selected then parents[#parents + 1] = {label = label, value = value.id} end
    end
    if #choices == 0 then choices = {{label = "Add an objective", value = ""}} end
    if not index_of(prerequisite) or prerequisite == selected then prerequisite = "" end
    local current, objective = mission(), item()
    local rows = {
        {id = "summary", type = "text", label = #doc().objectives .. "/32 objectives; " .. (current and current.status or "authoring")},
        {id = "episode", type = "text", label = doc().level_root ~= "" and caption(doc().level_root, 32) or "Add a point to choose an episode"},
        {id = "add", label = "Add objective at my character"},
        {id = "objective", type = "select", label = "Objective", choices = choices, value = selected or ""},
        {id = "step", type = "select", label = "Movement step", value = step,
            choices = {{label = "0.1", value = .1}, {label = "1", value = 1}, {label = "5", value = 5}, {label = "10", value = 10}}}
    }
    if objective then
        rows[#rows + 1] = {id = "kind", type = "select", label = "Rule", value = objective.kind, choices = {
            {label = "Reach point", value = "reach"}, {label = "Stay at point", value = "stay"}, {label = "Wait", value = "wait"}}}
        if objective.kind ~= "wait" then
            rows[#rows + 1] = {id = "stamp", label = "Move point to my character"}
            for _, axis in ipairs({"x", "y", "z"}) do rows[#rows + 1] = {id = "position-" .. axis, type = "slider",
                label = "Point " .. string.upper(axis), value = objective.position[axis], min = -1000000, max = 1000000, step = step} end
            rows[#rows + 1] = {id = "radius", type = "slider", label = "Radius", value = objective.radius, min = .1, max = 1000, step = .5}
        end
        if objective.kind ~= "reach" then rows[#rows + 1] = {id = "seconds", type = "slider", label = "Seconds",
            value = objective.seconds, min = .1, max = 86400, step = .5} end
        rows[#rows + 1] = {id = "parent", type = "select", label = "Prerequisite", choices = parents, value = prerequisite}
        rows[#rows + 1] = {id = "link", label = "Add or remove prerequisite"}
        rows[#rows + 1] = {id = "links", type = "text", label = caption("Needs: " .. (#objective.requires == 0 and "none" or table.concat(objective.requires, ", ")), 32)}
        rows[#rows + 1] = {id = "optional", type = "toggle", label = "Optional", value = objective.optional}
        rows[#rows + 1] = {id = "retry-point", type = "toggle", label = "Retry point after completion", value = objective.checkpoint}
        for _, row in ipairs({{id = "duplicate", label = "Duplicate objective"}, {id = "remove", label = "Remove objective"},
            {id = "up", label = "Move objective up"}, {id = "down", label = "Move objective down"}}) do rows[#rows + 1] = row end
        if current then
            for _, value in ipairs(current.objectives) do if value.id == selected then
                local hold = game.state.holds[value.id]
                rows[#rows + 1] = {id = "progress", type = "text", label = caption(value.state .. ": " .. string.format("%.1f/%.1f", hold and hold.seconds or value.progress, value.goal), 32)}
            end end
        end
    end
    for _, row in ipairs({
        {id = "undo", label = "Undo (" .. #undo .. ")"}, {id = "redo", label = "Redo (" .. #redo .. ")"},
        {id = "time-limit", type = "slider", label = "Time limit (0 is off)", value = doc().time_limit, min = 0, max = 86400, step = 5},
        {id = "defeat", type = "toggle", label = "Fail when all players fall", value = doc().fail_on_defeat},
        {id = "auto", type = "toggle", label = "Start on episode entry", value = doc().auto_start},
        {id = "start", label = "Play from beginning"}, {id = "retry", label = "Retry preview checkpoint"},
        {id = "checkpoint", label = "Set preview checkpoint"}, {id = "fail", label = "Fail preview"},
        {id = "stop", label = "Stop preview and edit"}, {id = "save", label = "Save mission and scene drafts"},
        {id = "new", label = "New empty mission"}, {id = "status", type = "text", label = caption(message, 32)}
    }) do rows[#rows + 1] = row end
    if message ~= last_message then game.log.info(message); last_message = message end
    game.ui.register_page({id = "mission", title = "Mission editor", rows = rows})
end
local function state_loaded()
    game.state.mission_document = game.projects.validate("mission", doc())
    local function bounded(value, high)
        return type(value) == "number" and value == value and value >= 0 and value <= high
    end
    assert(type(game.state.preview) == "boolean" and bounded(game.state.elapsed, 1e12)
        and bounded(game.state.checkpoint_elapsed, 1e12) and bounded(game.state.sample_seconds, .1)
        and bounded(game.state.objective_sequence, 1e9) and game.state.objective_sequence % 1 == 0,
        "Saved mission editor clocks or sequence are invalid")
    for _, holds in ipairs({game.state.holds, game.state.checkpoint_holds}) do
        assert(type(holds) == "table", "Saved mission hold state is invalid")
        local count = 0
        for key, hold in pairs(holds) do
            count = count + 1
            local index = index_of(key)
            assert(count <= 32 and index and doc().objectives[index].kind == "stay"
                and type(hold) == "table" and (hold.player == 0 or hold.player == 1)
                and bounded(hold.seconds, doc().objectives[index].seconds), "Saved mission hold ownership is invalid")
        end
    end
    -- Recreate definitions before the host restores objective progress. Do
    -- not reset elapsed time, hold ownership or the saved retry point here.
    synchronize()
    undo, redo, pending_start = {}, {}, false
    page()
end
game.on("initialize", function()
    local frozen = game.projects.document("mission")
    local draft = game.storage.read("mission-draft.json")
    game.state.mission_document = draft and draft.fingerprint == frozen.fingerprint and draft.mission or frozen.document
    game.state.preview, game.state.elapsed, game.state.holds = false, 0, {}
    game.state.sample_seconds = 0
    game.state.checkpoint_elapsed, game.state.checkpoint_holds, game.state.objective_sequence = 0, {}, 0
    state_loaded()
end)
game.on("state.loaded", state_loaded)
game.on("code.reloaded", function() message = "Mission progress rebound after reload"; page() end)
game.on("save.restored", function() message = "Mission and retry point restored"; page() end)
game.on("level.ready", function()
    if doc().auto_start and #doc().objectives > 0 and game.world.status().level_root == doc().level_root then
        local current = mission(); pending_start = not current or current.status == "idle"
    end
    page()
end)
game.on("level.unloading", function()
    game.state.sample_seconds, pending_start = 0, false
    local current = mission()
    if game.state.preview and current and current.status == "active" then game.missions.fail(id, "Left the mission's episode") end
end)
game.on("mission.changed", function() page_dirty = true end)
game.on("frame", function()
    ticks = ticks + 1
    if ticks % 15 ~= 0 then return end
    if pending_start and scene_ready() then start(false); page() end
    if saving and game.storage.status().pending == 0 then
        saving = false
        message = #game.storage.status().errors == 0 and "Drafts saved; close Test, then Apply saved drafts" or "Draft write failed; check storage errors"
        page()
    end
    if page_dirty then page() end
end)
game.on("simulation", function(event)
    if not game.state.preview then return end
    local current = mission()
    if not current or current.status ~= "active" then return end
    local status = game.world.status()
    if not status.ready or status.level_root ~= doc().level_root then return end
    -- Simulation seconds pause with the game. Only logical P1/P2 controllers
    -- count: an AI family member waiting at a point cannot complete it.
    game.state.sample_seconds = game.state.sample_seconds + event.seconds
    if game.state.sample_seconds < .1 then return end
    local delta = game.state.sample_seconds; game.state.sample_seconds = 0
    local actors, any_alive, count = {}, false, 0
    for player = 0, 1 do
        local actor = game.players.controlled(player)
        if actor then
            count = count + 1
            if actor.max_health <= 0 or actor.health > 0 then actors[#actors + 1] = {player = player, actor = actor}; any_alive = true end
        end
    end
    if doc().fail_on_defeat and count > 0 and not any_alive then game.missions.fail(id, "All controlled players fell"); return end
    if #actors == 0 then return end
    game.state.elapsed = game.state.elapsed + delta
    if doc().time_limit > 0 and game.state.elapsed >= doc().time_limit then game.missions.fail(id, "Time limit reached"); return end
    -- Snapshot availability before processing. A newly unlocked point is
    -- evaluated on the next sample, not completed by the same visit twice.
    local updates, retry_points = {}, {}
    for i, objective in ipairs(doc().objectives) do
        local progress = current.objectives[i]
        if progress.state == "active" then
            local amount = 0
            if objective.kind == "wait" then amount = delta
            else
                local inside = {}
                for _, value in ipairs(actors) do
                    local p, target = value.actor.position, objective.position
                    local dx, dy, dz = p.x - target.x, p.y - target.y, p.z - target.z
                    if dx * dx + dy * dy + dz * dz <= objective.radius * objective.radius then inside[value.player] = true end
                end
                if objective.kind == "reach" then
                    if inside[0] or inside[1] then amount = 1 end
                else
                    local hold = game.state.holds[objective.id]
                    if not hold or not inside[hold.player] then
                        local player = inside[0] and 0 or inside[1] and 1
                        hold = player and {player = player, seconds = 0} or nil
                    end
                    if hold then
                        hold.seconds = math.min(objective.seconds, hold.seconds + delta)
                        if hold.seconds >= objective.seconds then amount = objective.seconds; hold = nil end
                    end
                    game.state.holds[objective.id] = hold
                end
            end
            if amount > 0 then
                updates[#updates + 1] = {objective = objective.id, amount = amount}
                if objective.checkpoint then retry_points[objective.id] = true end
            end
        end
    end
    -- One native transaction validates every update before committing and
    -- returns one snapshot, rather than copying all objectives 32 times.
    if #updates > 0 then
        local result = game.missions.advance_batch(id, updates)
        if result.status == "active" then
            for _, value in ipairs(result.objectives) do
                if retry_points[value.id] and value.state == "completed" then checkpoint(); break end
            end
        end
    end
    page_dirty = true
end)
game.on("ui.action", function(event)
    if event.page ~= "mission" then return end
    if event.row == "objective" then selected = event.value; page(); return end
    if event.row == "parent" then prerequisite = event.value; page(); return end
    if event.row == "step" then step = event.value; page(); return end
    if event.row == "save" then
        local result = game.system.try_call("projects.save_all", {documents = {mission = doc(), scene = game.state.scene}})
        if result.ok then
            saving = true; message = "Saving mission and scene drafts"
        else failure(result) end
    elseif event.row == "start" or event.row == "retry" then start(event.row == "retry")
    elseif event.row == "stop" then stop()
    elseif event.row == "checkpoint" or event.row == "fail" then
        local current = mission()
        if current and current.status == "active" then
            if event.row == "checkpoint" then checkpoint(); message = "Preview checkpoint recorded"
            else game.missions.fail(id, "Failed from the mission editor") end
        else message = "Start a preview first" end
    elseif editable() then
        local objective, index = item()
        if event.row == "add" then add(event.player, false)
        elseif event.row == "duplicate" then add(event.player, true)
        elseif event.row == "new" then
            local empty = game.projects.document("mission").document
            empty.objectives, empty.level_root, empty.auto_start = {}, "", false
            if apply(empty, true) then selected = nil; message = "New empty mission" end
        elseif event.row == "undo" or event.row == "redo" then
            local from, to = event.row == "undo" and undo or redo, event.row == "undo" and redo or undo
            local target = from[#from]
            if target then
                local previous = copy(doc())
                if apply(target, false) then table.remove(from); to[#to + 1] = previous; message = "Mission edit restored" end
            end
        elseif event.row == "time-limit" or event.row == "defeat" or event.row == "auto" then
            edit(function(value) value[event.row == "time-limit" and "time_limit" or event.row == "defeat" and "fail_on_defeat" or "auto_start"] = event.value end)
        elseif objective then
            if event.row == "stamp" then
                local actor = game.players.controlled(event.player)
                if actor and world() then edit(function(value) value.objectives[index].position = copy(actor.position) end) end
            elseif event.row == "remove" then
                edit(function(value)
                    table.remove(value.objectives, index)
                    for _, other in ipairs(value.objectives) do
                        for i = #other.requires, 1, -1 do if other.requires[i] == objective.id then table.remove(other.requires, i) end end
                    end
                end)
            elseif event.row == "up" or event.row == "down" then
                local to = index + (event.row == "up" and -1 or 1)
                if to >= 1 and to <= #doc().objectives then edit(function(value) value.objectives[index], value.objectives[to] = value.objectives[to], value.objectives[index] end) end
            elseif event.row == "link" and prerequisite ~= "" then
                edit(function(value)
                    local deps, found = value.objectives[index].requires, false
                    for i = #deps, 1, -1 do if deps[i] == prerequisite then table.remove(deps, i); found = true end end
                    if not found then deps[#deps + 1] = prerequisite end
                end)
            elseif string.sub(event.row, 1, 9) == "position-" then
                edit(function(value) value.objectives[index].position[string.sub(event.row, 10)] = event.value end)
            elseif event.row == "kind" or event.row == "radius" or event.row == "seconds" or event.row == "optional" or event.row == "retry-point" then
                edit(function(value) value.objectives[index][event.row == "retry-point" and "checkpoint" or event.row] = event.value end)
            end
        end
    end
    page()
end)
