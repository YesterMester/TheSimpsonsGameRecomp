-- Modders: this is an encounter authoring workshop, not a campaign replacement.
-- Choose a loaded NPCMelee template, including its original resources and
-- class callbacks. Author safe arena positions for your mission;
-- generic cloning alone does not establish every enemy's AI/script dependencies.
local mission_id, goal = "brawler-workshop", 2
local targets, sample_seconds = {}, 0
local templates, selected = {}, 1
local status = "Load a level and choose an NPC target template"

local function display_name(item)
    -- Keep the native menu readable even for a mod's long resource filename.
    if #item.name > 96 then return item.name:sub(1, 93) .. "..." end
    return item.name
end

local function template_key(item)
    -- Native resource GUIDs survive display-name changes and code reload.
    -- The name fallback keeps this example readable on older hosts; new
    -- projects should use model_resource when selecting a particular asset.
    if item.model_resource and item.model_resource ~= "" then return item.model_resource end
    return "name:" .. item.name
end

local function refresh_templates()
    templates = {}
    if not game.world.status().ready then selected = 1; return end
    for _, item in ipairs(game.world.catalog()) do
        if item.class == "NPCMelee" or
            (not item.class and item.category == "Enemies" and item.name == "Brawler") then
            templates[#templates + 1] = item
        end
    end
    selected = math.max(1, math.min(selected, #templates))
    for i, item in ipairs(templates) do
        if template_key(item) == game.state.template_resource then selected = i; break end
    end
end

local function page()
    local mission = game.missions.get(mission_id)
    local template = templates[selected]
    game.ui.register_page({id = "combat", title = "Combat workshop", rows = {
        {id = "state", type = "text", label = "Encounter: " .. mission.status},
        {id = "progress", type = "text", label = "Defeated: " .. mission.objectives[1].progress .. " / " .. goal},
        {id = "status", type = "text", label = status},
        {id = "template", type = "text", label = "Target: " .. (template and display_name(template) or "Load a level")},
        {id = "previous", label = "Previous loaded target template"},
        {id = "next", label = "Next loaded target template"},
        {id = "refresh", label = "Refresh loaded template names"},
        {id = "start", label = "Start beside my character"},
        {id = "retry", label = "Retry from the encounter checkpoint"},
        {id = "hit", label = "Apply a test hit (10 HP)"},
        {id = "heal", label = "Heal the remaining targets"},
        {id = "hide", label = "Hide my targets"},
        {id = "reveal", label = "Reveal my targets"},
        {id = "cancel", label = "End the encounter and remove my targets"}
    }})
end

local function cleanup()
    -- Mark our local set empty before requesting removal: removal is not a
    -- defeat, and the later entity.removed event must not complete objectives.
    targets = {}
    if not game.world.status().ready then return end
    for _, object in ipairs(game.world.owned()) do
        game.system.try_call("world.destroy", {entity = object.id})
    end
end

local function fail(reason)
    if game.missions.get(mission_id).status == "active" then
        game.missions.fail(mission_id, reason)
    end
    cleanup()
    status = reason
    page()
end

local function spawn_remaining(player)
    cleanup()
    local actor = game.players.controlled(player)
    if not actor then fail("Assign a player and load a playable level before retrying"); return end
    refresh_templates()
    local template
    for _, item in ipairs(templates) do
        if template_key(item) == game.state.encounter_template_resource then template = item; break end
    end
    if not template then
        fail("The encounter template is no longer loaded; choose a target and start again"); return
    end
    local mission = game.missions.get(mission_id)
    local remaining = goal - mission.objectives[1].progress
    local p = actor.position
    for i = 1, remaining do
        local spawned = game.system.try_call("world.spawn", {template = template.id,
            position = {x = p.x + 5, y = p.y + 0.25, z = p.z + (i - 1) * 3}, radius = 1})
        if not spawned.ok then fail("The native factory rejected this encounter template"); return end
        local health = game.system.try_call("entities.health", {entity = spawned.value})
        if not health.ok or not health.value or health.value.dead then
            fail("This template has no supported living health component"); return
        end
        targets[spawned.value] = true
    end
    status = display_name(template) .. ": use Test hit to inspect native damage; natural AI needs scene setup"
    page()
end

local function rebind()
    targets = {}
    refresh_templates()
    if not game.world.status().ready then page(); return end
    for _, object in ipairs(game.world.owned()) do targets[object.id] = true end
    -- Reload transfers objects; it must not spawn replacements. Exact saves
    -- rebind supported objects, while checkpoint loads create a fresh world.
    page()
end

game.on("initialize", function()
    game.missions.register({id = mission_id, title = "Brawler workshop", objectives = {
        {id = "defeat", title = "Defeat the encounter targets", goal = goal}
    }})
    game.state.elapsed, game.state.checkpoint_elapsed, game.state.player = 0, 0, 0
    game.state.template_resource, game.state.encounter_template_resource = "", ""
    page()
end)
game.on("state.loaded", page)
game.on("level.ready", rebind)
game.on("code.reloaded", rebind)
game.on("save.restored", function(event)
    if event.kind == "checkpoint" and game.missions.get(mission_id).status == "active" then
        game.missions.restart(mission_id)
        game.state.elapsed = game.state.checkpoint_elapsed or 0
        spawn_remaining(game.state.player or 0)
    else rebind() end
end)
game.on("level.unloading", function()
    targets, templates, selected = {}, {}, 1
    if game.missions.get(mission_id).status == "active" then
        game.missions.fail(mission_id, "The level changed; restart in the new playable area")
    end
    status = "Load a playable level before starting"
    page()
end)

game.on("ui.action", function(event)
    if event.page ~= "combat" then return end
    if event.row == "previous" or event.row == "next" or event.row == "refresh" then
        refresh_templates()
        if #templates > 0 then
            if event.row == "previous" then selected = (selected - 2) % #templates + 1 end
            if event.row == "next" then selected = selected % #templates + 1 end
            game.state.template_resource = template_key(templates[selected])
        end
        page()
    elseif event.row == "start" or event.row == "retry" then
        if not game.world.status().ready then status = "Load a level first"; page(); return end
        refresh_templates()
        if event.row == "start" then
            local template = templates[selected]
            if not template then status = "This level has no loaded NPCMelee target templates"; page(); return end
            game.state.template_resource = template_key(template)
            -- Browsing another template later must not replace this encounter's
            -- remaining targets during a retry or checkpoint restore.
            game.state.encounter_template_resource = template_key(template)
        end
        local mission = game.missions.get(mission_id)
        if event.row == "retry" and not mission.has_checkpoint then
            status = "Start the encounter to create its checkpoint"; page(); return
        end
        if mission.status == "idle" then game.missions.start(mission_id)
        else game.missions.restart(mission_id, event.row == "retry") end
        game.state.player = event.player -- The initiating logical P1/P2, not family-list order.
        game.state.elapsed = event.row == "retry" and (game.state.checkpoint_elapsed or 0) or 0
        if event.row == "start" then
            game.state.checkpoint_elapsed = 0
            game.missions.checkpoint(mission_id)
        end
        spawn_remaining(event.player)
    elseif event.row == "hit" then
        for _, object in ipairs(game.world.owned()) do
            if targets[object.id] then
                local hit = game.system.try_call("entities.damage", {entity = object.id, amount = 10})
                if not hit.ok then game.log.info(hit.code .. ": " .. hit.error) end
                break
            end
        end
    elseif event.row == "heal" then
        for _, object in ipairs(game.world.owned()) do
            if targets[object.id] then game.entities.heal(object.id, 1000000) end
        end
    elseif event.row == "hide" or event.row == "reveal" then
        -- Visibility is independent of AI, damage and collision. Repeated
        -- requests are safe: the host balances one hide request per object,
        -- preserves the game's other hide requests, and transfers it on reload.
        for _, object in ipairs(game.world.owned()) do
            if targets[object.id] then
                local result = game.system.try_call("entities.set_visible",
                    {entity = object.id, visible = event.row == "reveal"})
                if not result.ok then game.log.info(result.code .. ": " .. result.error) end
            end
        end
    elseif event.row == "cancel" then fail("Encounter ended by the player") end
end)

game.on("entity.died", function(event)
    if not targets[event.entity] then return end
    targets[event.entity] = nil
    local mission = game.missions.get(mission_id)
    if mission.status == "active" then
        -- Only the original native death signal advances this objective.
        -- Ordinary removal, streaming and world.destroy never count as kills.
        mission = game.missions.advance(mission_id, "defeat", 1)
        if mission.status == "active" then
            game.missions.checkpoint(mission_id)
            game.state.checkpoint_elapsed = game.state.elapsed
        else status = "Encounter complete" end
    end
    -- The class may already have deleted itself. An expired handle is an
    -- expected outcome, so retire any remaining body through a bounded call.
    game.system.try_call("world.destroy", {entity = event.entity})
    page()
end)
game.on("entity.removed", function(event)
    if not targets[event.entity] then return end
    targets[event.entity] = nil
    fail("A target left without a native defeat; retry the encounter")
end)
game.on("simulation", function(event)
    if game.missions.get(mission_id).status ~= "active" then return end
    game.state.elapsed = game.state.elapsed + event.seconds
    sample_seconds = sample_seconds + event.seconds
    if sample_seconds < 0.2 then return end
    sample_seconds = 0
    if game.state.elapsed >= 90 then fail("Time ran out; retry from the checkpoint"); return end
    local assigned, alive = false, false
    for player = 0, 1 do
        local actor = game.players.controlled(player)
        if actor then
            assigned = true
            local health = game.players.health(actor.id)
            alive = alive or not health or not health.dead
        end
    end
    if assigned and not alive then fail("Both active players are down; retry from the checkpoint") end
end)
