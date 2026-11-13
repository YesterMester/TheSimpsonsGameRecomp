-- Modders: a catalog entry is a template already loaded by this level.
-- An asset search result is not a template and does not load dependencies.
-- This example lets you deliberately spawn a prop from its native menu page;
-- it never spawns automatically when a level, save or code reload begins.
local templates, selected, live, removed = {}, 1, 0, 0
local status = "Choose a loaded prop"

local function page()
    local choice = templates[selected]
    game.ui.register_page({id = "objects", title = "Owned objects", rows = {
        {id = "template", type = "text", label = "Template: " .. (choice and choice.name or "Load a level")},
        {id = "previous", label = "Previous loaded prop"},
        {id = "next", label = "Next loaded prop"},
        {id = "spawn", label = "Spawn beside my character"},
        {id = "move", label = "Bring my latest object here"},
        {id = "turn", label = "Rotate latest 15 degrees"},
        {id = "grow", label = "Grow latest by 10%"},
        {id = "shrink", label = "Shrink latest by 10%"},
        {id = "count", type = "text", label = "My objects: " .. live .. "; removed: " .. removed},
        {id = "status", type = "text", label = status},
        {id = "clear", label = "Remove my objects"}
    }})
end

local function refresh()
    templates = {}
    if not game.world.status().ready then live = 0; page(); return end
    for _, item in ipairs(game.world.catalog()) do
        -- Start with props. Other classes can have encounter, collectible or
        -- script requirements; the generic factory alone does not prove them.
        if item.category == "Props" then templates[#templates + 1] = item end
    end
    selected = math.max(1, math.min(selected, #templates))
    live = #game.world.owned()
    page()
end

game.on("initialize", page)
game.on("level.ready", refresh)
game.on("code.reloaded", refresh)
game.on("save.restored", refresh)
game.on("level.unloading", function()
    -- All of the old world's handles are stale. The game tears its objects
    -- down; do not call world.destroy on handles from the previous level.
    templates, selected, live = {}, 1, 0
    page()
end)

game.on("ui.action", function(event)
    if event.page ~= "objects" then return end
    if event.row == "previous" and #templates > 0 then
        selected = (selected - 2) % #templates + 1
    elseif event.row == "next" and #templates > 0 then
        selected = selected % #templates + 1
    elseif event.row == "spawn" then
        local actor = game.players.controlled(event.player)
        local template = templates[selected]
        if not actor or not template then return end
        local p = actor.position
        -- Coordinates use the game's X/Z ground plane and Y height. A real
        -- encounter should author placement, collision clearance and resources.
        -- A class can reject generic cloning. Handle an expected failure so
        -- trying an unsupported template does not retire our other objects.
        local result = game.system.try_call("world.spawn", {template = template.id,
            position = {x = p.x + 4, y = p.y + 1, z = p.z}, radius = 1})
        status = result.ok and "Spawned " .. template.name or "This template could not be cloned"
        if not result.ok then game.log.info(result.code .. ": " .. result.error) end
        live = #game.world.owned()
    elseif event.row == "move" then
        local actor = game.players.controlled(event.player)
        local objects = game.world.owned()
        local latest = objects[#objects]
        if not actor or not latest then return end
        local p = actor.position
        -- Read and move through typed world coordinates. The host converts
        -- parent space and runs the class setter, including its controller.
        -- This is placement, not a collision sweep: author safe destinations.
        -- system.try_call handles a named host failure, not an arbitrary Lua
        -- function. Callback errors and runaway scripts still stop this mod.
        local before = game.system.try_call("entities.transform", {entity = latest.id})
        local result = before
        if before.ok then
            result = game.system.try_call("entities.move", {entity = latest.id,
                position = {x = p.x + 4, y = p.y + 1, z = p.z}})
        end
        if result.ok then
            local old = before.value.position
            status = string.format("Moved from %.1f, %.1f, %.1f", old.x, old.y, old.z)
        else
            status = "This object's transform is unavailable"
            game.log.info(result.code .. ": " .. result.error)
        end
    elseif event.row == "turn" or event.row == "grow" or event.row == "shrink" then
        local objects = game.world.owned()
        local latest = objects[#objects]
        if not latest then return end
        local result = game.system.try_call("entities.transform", {entity = latest.id})
        if result.ok then
            local pose = result.value
            -- Modders: basis vectors include scale. Change every basis vector,
            -- leaving the world position alone. The native full setter updates
            -- its cached scale and notifies the model/controller; writing a
            -- frame's bytes yourself would skip that class behavior.
            local angle = math.rad(15)
            local c, s = math.cos(angle), math.sin(angle)
            local factor = event.row == "grow" and 1.1 or 0.9
            for _, name in ipairs({"right", "up", "forward"}) do
                local v = pose[name]
                if event.row == "turn" then
                    -- Rotate around world Y, the game's vertical axis.
                    pose[name] = {x = v.x * c + v.z * s, y = v.y, z = v.z * c - v.x * s}
                else
                    pose[name] = {x = v.x * factor, y = v.y * factor, z = v.z * factor}
                end
            end
            result = game.system.try_call("entities.set_transform", {entity = latest.id, transform = pose})
        end
        status = result.ok and "Updated my object's orientation and scale" or "This transform could not be applied"
        if not result.ok then game.log.info(result.code .. ": " .. result.error) end
    elseif event.row == "clear" then
        for _, object in ipairs(game.world.owned()) do game.world.destroy(object.id) end
        -- A borrowed cross-level template automatically retains its source
        -- for this mod. Release that claim too; other owners keep theirs.
        game.assets.release_source()
        -- Removal is queued until this callback returns; it is already absent
        -- from world.owned(). Never free a guest address or call a destructor.
        live = #game.world.owned()
        status = "Removal requested"
    end
    page()
end)

game.on("entity.removed", function(event)
    -- event.entity has expired. An NPC, pickup or destructible can disappear
    -- through normal gameplay too. Observe the event, then query current IDs.
    removed = removed + 1
    live = #game.world.owned()
    page()
end)

-- Do not keep object handles in game.state. Code reload transfers live owned
-- objects to the replacement generation. Exact restore rebinds saved GUIDs;
-- a checkpoint loads a fresh world, so an authored mission must recreate its
-- intended objects from supported templates. Failures retire this owner and
-- its dependents; another mod's objects and the original level stay separate.
