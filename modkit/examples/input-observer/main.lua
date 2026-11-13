-- Modders: logical players are 0 and 1. They are not indices into
-- players.list(), which includes the loaded family, including inactive actors.
-- players.controlled(player) finds the current human character. Requery it:
-- an old handle can remain alive as an AI after the player switches away.
-- These snapshots already include keyboard/controller/touch assignment and
-- the game's held-input guards. Reading never polls a driver or consumes input.
local recording = true
local counts, last = {0, 0}, {"none", "none"}
local characters = {"Unassigned", "Unassigned"}
local elapsed, dirty = 0, false
local controls = {
    "a", "b", "x", "y", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "left_shoulder", "right_shoulder", "left_trigger", "right_trigger",
    "left_stick", "right_stick", "start", "back", "guide"
}

local function page()
    game.ui.register_page({id = "inputs", title = "Input observer", rows = {
        {id = "p1", type = "text", label = "P1 presses: " .. counts[1] .. "; last: " .. last[1]},
        {id = "p2", type = "text", label = "P2 presses: " .. counts[2] .. "; last: " .. last[2]},
        {id = "actor1", type = "text", label = "P1 character: " .. characters[1]},
        {id = "actor2", type = "text", label = "P2 character: " .. characters[2]},
        {id = "record", type = "toggle", label = "Record gameplay controls", value = recording},
        {id = "clear", label = "Clear counts"}
    }})
    dirty = false
end

game.on("initialize", page)

game.on("simulation", function(event)
    for player = 0, 1 do
        local actor = game.players.controlled(player)
        local name = actor and actor.character or "Unassigned"
        if characters[player + 1] ~= name then
            characters[player + 1] = name
            dirty = true
        end
        -- A custom ability could now affect actor.id, not an arbitrary member
        -- of players.list(). Do not store this handle in saved game.state.
    end
    -- A gameplay action belongs in simulation, not frame: pause/menu redraws
    -- must not activate it. Use ui.action for buttons in your native menu page.
    if recording then
        for player = 0, 1 do
            local input = game.input.state(player)
            if input.sampled and input.available then
                for _, control in ipairs(controls) do
                    if input.pressed[control] then
                        local index = player + 1 -- Lua arrays begin at 1.
                        counts[index] = counts[index] + 1
                        last[index] = control
                        dirty = true
                    end
                end
                -- For continuous actions, read input.buttons.x, for example.
                -- Analog axes are input.left_stick.x/y and right_stick.x/y,
                -- from -1 to 1 (up is positive Y); triggers range from 0 to 1.
                -- Do not save these transient controls or input edges in game.state.
            end
        end
    end
    -- Updating menu text does not need to run at the game's frame rate.
    elapsed = elapsed + event.seconds
    if dirty and elapsed >= 0.25 then
        elapsed = 0
        page()
    end
end)

game.on("ui.action", function(event)
    if event.page ~= "inputs" then return end
    -- event.player identifies the logical player who used this page, including
    -- P2. It does not change which controller the input service reads.
    if event.row == "record" then recording = event.value end
    if event.row == "clear" then counts, last = {0, 0}, {"none", "none"} end
    page()
end)

game.on("level.unloading", function()
    counts, last = {0, 0}, {"none", "none"}
    characters = {"Unassigned", "Unassigned"}
    elapsed = 0
    page()
end)
