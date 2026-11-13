-- Modders: start here. Change the words in locales/en.json and the action
-- below; the menu stays inside the game's native Options > Extras > Mods.
-- Stable text keys let other people translate this project without editing Lua.
local function page()
    game.ui.register_page({id = "starter", title = game.locale.text("title"), rows = {
        {id = "status", type = "text", label = game.locale.text("count", {count = game.state.count or 0})},
        {id = "hello", label = game.locale.text("action")}
    }})
end

local function restored()
    -- State is replaced on reload/restore. Read game.state each time instead
    -- of keeping a reference to an old table or any temporary game handle.
    local count = game.state.count
    if type(count) ~= "number" or count ~= count or count < 0 or count > 1000000 then
        game.state.count = 0
    end
    page()
end

game.on("initialize", restored)
game.on("state.loaded", restored)
game.on("code.reloaded", page)
game.on("ui.action", function(event)
    if event.page ~= "starter" or event.row ~= "hello" then return end
    game.state.count = (game.state.count or 0) + 1
    game.log.info(game.locale.text("message"))
    page()
end)
