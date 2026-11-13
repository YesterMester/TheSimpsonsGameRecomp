-- Modders: use simulation timers instead of counting rendered frames.
-- Timers belong to this mod, pause with gameplay and retire with the instance.
-- The host preserves their remaining time during code reload and mod saves.
local function page()
    game.ui.register_page({id = "timers", title = game.locale.text("title"), rows = {
        {id = "start", label = game.locale.text("start")},
        {id = "cancel", label = game.locale.text("cancel")},
        {id = "count", type = "text", label = game.locale.text("count", {count = game.state.completed or 0})}
    }})
end

local function restored()
    local count = game.state.completed
    if type(count) ~= "number" or count ~= count or count < 0 or count > 1000000 then
        game.state.completed = 0
    end
    -- Don't reschedule here: restored timers already have their own deadlines.
    page()
end

game.on("initialize", restored)
game.on("state.loaded", restored)
game.on("code.reloaded", page)
game.on("ui.action", function(event)
    if event.page ~= "timers" then return end
    if event.row == "start" then
        -- Reusing this id replaces the pending timer; it doesn't stack copies.
        game.timers.schedule("reminder", 5)
    elseif event.row == "cancel" then
        game.timers.cancel("reminder")
    end
end)
game.on("timer", function(event)
    if event.id ~= "reminder" then return end
    game.state.completed = (game.state.completed or 0) + 1
    game.log.info(game.locale.text("finished"))
    -- Put a supported game operation here for a delayed gameplay action.
    page()
end)
