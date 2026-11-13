-- Modders: logical players are 0/1, not positions in the family list.
-- This menu action affects the player who presses it. Requery the controlled
-- character on every action: switching characters can leave the old one as AI.
local cooldowns = {0, 0}
local shortcuts = false

local function page()
    game.ui.register_page({id = "player-actions", title = game.locale.text("title"), rows = {
        {id = "heal", label = game.locale.text("heal")},
        {id = "shortcuts", type = "toggle", label = game.locale.text("shortcuts"), value = shortcuts}
    }})
end

local function heal(player)
    if player ~= 0 and player ~= 1 then return end
    if cooldowns[player + 1] > 0 then return end
    local actor = game.players.controlled(player)
    if not actor then
        game.log.info(game.locale.text("no_actor"))
        return
    end
    -- Replace this call with another documented operation for your ability.
    -- The original health service preserves death rules; this isn't a revive.
    game.players.refill_health(actor.id)
    cooldowns[player + 1] = 5
end

game.on("initialize", page)
game.on("code.reloaded", page)
game.on("ui.action", function(event)
    if event.page ~= "player-actions" then return end
    if event.row == "heal" then heal(event.player or 0) end
    if event.row == "shortcuts" then shortcuts = event.value; page() end
end)
game.on("simulation", function(event)
    -- Simulation time stops while paused; display FPS doesn't alter cooldowns.
    for player = 0, 1 do
        cooldowns[player + 1] = math.max(0, cooldowns[player + 1] - event.seconds)
        if shortcuts then
            local input = game.input.state(player)
            -- The shortcut is off by default. Reading controls doesn't consume
            -- their original actions: pick an appropriate combo for your mod.
            if input.sampled and input.available and input.buttons.left_shoulder and input.pressed.y then
                heal(player)
            end
        end
    end
end)
game.on("level.unloading", function() cooldowns = {0, 0} end)
