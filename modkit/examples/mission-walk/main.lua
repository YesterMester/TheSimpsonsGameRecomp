-- Modders: this project runs through public APIs on every supported host.
-- It creates an authored mission beside the campaign, without editing the
-- campaign's scripts, objectives or save files. Start it in Options -> Mods.
local id = "there-and-back"
local sample_seconds = 0

local function text(key, values)
    return game.locale.text(key, values or {})
end

local function page()
    local mission = game.missions.get(id)
    local rows = {
        {id = "status", label = text("page.state", {
            state = text("status." .. mission.status)
        }), type = "text"}
    }
    for _, objective in ipairs(mission.objectives) do
        rows[#rows + 1] = {
            id = objective.id,
            type = "text",
            label = text("page.progress", {
                title = objective.title,
                progress = math.floor(objective.progress),
                goal = objective.goal
            })
        }
    end
    if mission.reason ~= "" then
        rows[#rows + 1] = {id = "reason", label = mission.reason, type = "text"}
    end
    rows[#rows + 1] = {id = "start", label = text(
        mission.status == "idle" and "page.start" or "page.replay"
    )}
    if mission.status ~= "idle" and mission.has_checkpoint then
        rows[#rows + 1] = {id = "retry", label = text("page.retry")}
    end
    if mission.status == "active" then
        rows[#rows + 1] = {id = "checkpoint", label = text("page.checkpoint")}
    end
    game.ui.register_page({id = "route", title = text("page.title"), rows = rows})
end

game.on("initialize", function()
    -- Stable objective ids belong in saves. Display text can be translated
    -- or reordered without invalidating progress. Goals/prerequisites need
    -- a state.loaded migration when an existing project changes them.
    game.missions.register({
        id = id, title = text("mission.title"),
        description = text("mission.description"),
        objectives = {
            {id = "outbound", title = text("objective.outbound"), goal = 12},
            {id = "return", title = text("objective.return"), requires = {"outbound"}}
        }
    })
    game.state.elapsed = 0
    game.state.checkpoint_elapsed = 0
    game.state.travellers = {}
    page()
end)

game.on("ui.action", function(event)
    if event.page ~= "route" then return end
    local players = game.players.list()
    if #players == 0 then
        game.log.info(text("error.no_level"))
        return
    end
    if event.row == "start" then
        -- Store coordinates, not a player handle: handles expire on restore.
        game.state.origins = {}
        for _, player in ipairs(players) do
            game.state.origins[player.character] = player.position
        end
        game.state.elapsed, game.state.checkpoint_elapsed = 0, 0
        game.state.travellers = {}
        if game.missions.get(id).status == "idle" then
            game.missions.start(id)
        else
            game.missions.restart(id, false)
        end
    elseif event.row == "retry" then
        -- This restores objective progress only. A larger mission should
        -- also restore its owned objects through supported world APIs.
        game.missions.restart(id)
        game.state.elapsed = game.state.checkpoint_elapsed or 0
    elseif event.row == "checkpoint" then
        game.missions.checkpoint(id)
        game.state.checkpoint_elapsed = game.state.elapsed
    end
    page()
end)

game.on("simulation", function(event)
    local mission = game.missions.get(id)
    if mission.status ~= "active" then return end
    -- Simulation seconds pause with the game and do not depend on display FPS.
    game.state.elapsed = game.state.elapsed + event.seconds
    sample_seconds = sample_seconds + event.seconds
    if sample_seconds < 0.1 then return end
    sample_seconds = 0
    if game.state.elapsed >= 90 then
        game.missions.fail(id, text("error.timeout"))
        return
    end
    local players = game.players.list()
    if #players == 0 then return end
    local alive = false
    local nearest_return, farthest = math.huge, 0
    local outward = mission.objectives[1]
    for _, player in ipairs(players) do
        alive = alive or player.max_health <= 0 or player.health > 0
        local p = player.position
        -- Each member starts from their own position. An inactive character
        -- already far from the first player must not advance the route.
        local origin = game.state.origins[player.character]
        if not origin then
            origin = p
            game.state.origins[player.character] = origin
        end
        local dx, dy, dz = p.x - origin.x, p.y - origin.y, p.z - origin.z
        local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
        farthest = math.max(farthest, distance)
        -- players.list enumerates loaded family entities, not logical input
        -- assignments. An idle member waiting at home must not complete the
        -- return stage for somebody else. Track members who actually went out.
        if distance >= outward.goal then
            game.state.travellers[player.character] = true
        end
        if game.state.travellers[player.character] then
            nearest_return = math.min(nearest_return, distance)
        end
    end
    if not alive then
        game.missions.fail(id, text("error.health"))
        return
    end
    if outward.state == "active" and farthest > outward.progress then
        mission = game.missions.advance(id, "outbound",
            math.min(outward.goal, farthest) - outward.progress)
        if mission.objectives[1].state == "completed" then
            game.missions.checkpoint(id)
            game.state.checkpoint_elapsed = game.state.elapsed
        end
    end
    -- Co-op: either live family member may advance the route. One player
    -- remaining at the origin cannot finish both stages in the same sample.
    if outward.state == "completed" and nearest_return <= 2 then
        game.missions.advance(id, "return", 1)
    end
end)

game.on("mission.changed", function(event)
    if event.mission.id ~= id then return end
    page()
    game.log.info(text("log.change", {
        title = event.mission.title, state = text("status." .. event.mission.status)
    }))
end)

game.on("level.unloading", function()
    if game.missions.get(id).status == "active" then
        game.missions.fail(id, text("error.level"))
    end
end)
game.on("level.ready", page)
game.on("save.restored", page)
game.on("code.reloaded", page)
game.on("state.loaded", function(event)
    -- The host restores unchanged mission definitions after this callback.
    -- To change a goal, migrate event.missions and call missions.restore
    -- here. An invalid migration keeps the current running mod untouched.
    sample_seconds = 0
    game.state.travellers = game.state.travellers or {}
    assert(type(game.state.travellers) == "table", "Saved route participants are invalid")
    if game.state.elapsed ~= nil then
        assert(type(game.state.elapsed) == "number" and game.state.elapsed >= 0,
            "Saved mission time is invalid")
    end
    for _, saved in ipairs(event.missions.missions) do
        if saved.definition.id == id and saved.status ~= "idle" then
            assert(type(game.state.origins) == "table", "Saved route origins are invalid")
            for _, origin in pairs(game.state.origins) do
                assert(type(origin) == "table" and type(origin.x) == "number"
                    and type(origin.y) == "number" and type(origin.z) == "number",
                    "Saved route origin is invalid")
            end
        end
    end
end)
