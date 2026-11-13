-- Modders: this is a native camera authoring starter, not a screen overlay.
-- Open Options > Extras > Mods > Camera workshop. Fly the camera using its
-- normal controls, capture two views, then preview either without moving the
-- family. Positions use world coordinates with Y up; angles and the vertical
-- field of view use degrees. No guest addresses or renderer-specific code.
game.state.schema = 1
game.state.shots = {}
game.state.epoch = 0
game.state.freeze = false
game.state.status = "Load a level to begin"

local function page()
    -- Native menu rows have one line. Keep status text short; put detailed
    -- diagnostics in the mod log rather than wrapping over the next row.
    local mode = game.camera.status()
    game.ui.register_page({id = "camera", title = "Camera workshop", rows = {
        {id = "status", label = game.state.status, type = "text"},
        {id = "free", label = "Fly the camera", type = "toggle", value = mode.free},
        {id = "photo", label = "Freeze the scene", type = "toggle", value = mode.photo},
        {id = "capture_a", label = "Capture shot A"},
        {id = "preview_a", label = "Preview shot A"},
        {id = "capture_b", label = "Capture shot B"},
        {id = "preview_b", label = "Preview shot B"},
        {id = "wide", label = "Wide lens (80 degrees)"},
        {id = "normal", label = "Medium lens (60 degrees)"},
        {id = "close", label = "Close lens (35 degrees)"},
        {id = "release", label = "Finish and release my camera"}
    }})
end

local function refresh_world()
    local world = game.world.status()
    if world.ready and game.state.epoch ~= world.epoch then
        -- World-space captures belong to this particular scene. A checkpoint
        -- loads a fresh world; never silently apply an old scene's coordinates.
        -- Authored cinematic tracks need their own proven level dependencies.
        game.state.shots = {}
        game.state.epoch = world.epoch
        game.state.status = "Ready to capture a shot"
    end
    return world.ready
end

local function request(name, arguments)
    -- Expected host failures are recoverable. Lua errors and callback budgets
    -- still retire this mod; the kit then releases its camera automatically.
    local result = game.system.try_call(name, arguments or {})
    if not result.ok then
        game.log.info(result.error)
        game.state.status = "Camera request failed"
    end
    return result.ok
end

game.on("initialize", page)
game.on("level.ready", function() refresh_world(); page() end)
game.on("level.unloading", function()
    game.state.status = "Scene unloading"
    page()
end)
game.on("ui.action", function(event)
    if event.page ~= "camera" then return end
    if event.row == "release" then
        -- Release only the camera. This mod's other services, if you add any,
        -- stay active. Another mod's preceding camera request can resume.
        if request("camera.release") then game.state.status = "Camera released" end
    elseif not refresh_world() then
        game.state.status = "Load a level first"
    elseif event.row == "free" then
        if request("camera.free", {enabled = event.value}) then
            game.state.status = "Fly, then capture a shot"
        end
    elseif event.row == "photo" then
        if request("camera.photo", {enabled = event.value}) then
            game.state.freeze = event.value
            game.state.status = event.value and "Scene frozen" or "Scene running"
        end
    elseif event.row == "capture_a" or event.row == "capture_b" then
        local pose = game.camera.pose()
        if pose then
            local slot = event.row == "capture_a" and "a" or "b"
            -- Store values in game.state, not a local reference to that table.
            -- Code reload restores the table and keeps these shots in this world.
            game.state.shots[slot] = pose
            game.state.status = "Captured shot " .. string.upper(slot)
        else game.state.status = "Camera not ready" end
    elseif event.row == "preview_a" or event.row == "preview_b" then
        local slot = event.row == "preview_a" and "a" or "b"
        local pose = game.state.shots[slot]
        if not pose then game.state.status = "Capture shot " .. string.upper(slot) .. " first"
        elseif request("camera.set_pose", {pose = pose}) then
            request("camera.photo", {enabled = game.state.freeze})
            game.state.status = "Previewing shot " .. string.upper(slot) .. ""
        end
    elseif event.row == "wide" or event.row == "normal" or event.row == "close" then
        local pose = game.camera.pose()
        if pose then
            pose.fov_degrees = event.row == "wide" and 80 or (event.row == "close" and 35 or 60)
            if request("camera.set_pose", {pose = pose}) then
                game.state.status = "Lens changed"
            end
        else game.state.status = "Camera not ready" end
    end
    page()
end)

local function finite(value, low, high)
    return type(value) == "number" and value == value and value >= low and value <= high
end
local function valid_pose(pose)
    if type(pose) ~= "table" or type(pose.position) ~= "table" or type(pose.rotation) ~= "table" then
        return false
    end
    for _, axis in ipairs({"x", "y", "z"}) do
        if not finite(pose.position[axis], -1000000, 1000000) then return false end
    end
    return finite(pose.rotation.pitch, -90, 90) and finite(pose.rotation.yaw, -180, 180)
        and finite(pose.rotation.roll, -180, 180) and finite(pose.fov_degrees, 1, 120)
end
game.on("state.loaded", function()
    -- Validate saved values before a code reload commits. This callback must
    -- not apply a pose: gameplay writes are forbidden during restore preflight.
    if game.state.schema ~= 1 or type(game.state.shots) ~= "table"
        or not finite(game.state.epoch, 0, 9007199254740991)
        or game.state.epoch % 1 ~= 0 or type(game.state.freeze) ~= "boolean"
        or type(game.state.status) ~= "string" or #game.state.status > 1024 then
        error("Camera workshop state needs migration")
    end
    for slot, pose in pairs(game.state.shots) do
        if (slot ~= "a" and slot ~= "b") or not valid_pose(pose) then
            error("Camera workshop shot needs migration")
        end
    end
    game.state.status = "Shots restored"
    page()
end)
game.on("code.reloaded", page)
game.on("save.restored", function() refresh_world(); page() end)
