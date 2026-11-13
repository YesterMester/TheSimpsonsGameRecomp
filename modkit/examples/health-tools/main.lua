-- Modders: this script runs on the game thread and uses named game operations.
-- Put playthrough values in game.state so loading a save restores them. Do not
-- hold a local reference to game.state: the host replaces it when restoring.
game.state.visits = 0

local function page()
    -- Modders: stable text keys let a language pack translate this page without
    -- changing the script. The host falls back to locales/en.json for gaps.
    local text = game.locale.text
    game.ui.register_page({
        id = "tools", title = text("page.title"),
        rows = {
            {id = "heal", label = text("health.refill")},
            {id = "god", label = text("health.protect"), type = "toggle",
             value = game.state.protected or false},
            {id = "camera", label = text("camera.free"), type = "toggle",
             value = game.camera.status().free}
        }
    })
end

game.on("initialize", page)
-- Level handles expire on unload. Query the family in each action instead of
-- saving those handles in game.state or keeping them through a checkpoint load.
game.on("level.ready", function()
    game.state.visits = game.state.visits + 1
    game.log.info(game.locale.text("level.ready", {visits = game.state.visits}))
    if game.state.protected then game.players.god_mode(true) end
end)
game.on("ui.action", function(event)
    if event.page ~= "tools" then return end
    if event.row == "heal" then
        for _, player in ipairs(game.players.list()) do
            game.players.refill_health(player.id)
        end
    elseif event.row == "god" then
        game.state.protected = event.value
        game.players.god_mode(event.value)
    elseif event.row == "camera" then
        game.camera.free(event.value)
    end
end)
game.on("save.restored", page)
-- Modders: this callback runs before a restore or reload commits. Validate or
-- migrate game.state here; an error keeps the running instances unchanged.
-- Menus and this instance's timers may change, but gameplay/storage may not.
game.on("state.loaded", page)
-- Modders: reload preserves game.state and timer delays. Rebuild displayed
-- values after the new instance receives that state; query fresh handles here
-- if your mod retains any. Use creator reload to apply code-only edits in play.
game.on("code.reloaded", page)
