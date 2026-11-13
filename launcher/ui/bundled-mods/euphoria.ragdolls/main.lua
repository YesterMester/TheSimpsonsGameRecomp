-- Example of an owned host request. Disabling/failing/unloading this package
-- releases its settings; a menu page name never grants access to global cvars.
-- This is experimental root motion, not a jointed ragdoll physics solver.

local state = {
    enabled = true,
    run_impact = true,
    punch_impact = true,
    push_downed = true,
    player_enabled = true,
    force_scale = 1.0,
}

local function register_menu()
    game.ui.register_page({
        id = "euphoria_ragdolls",
        title = game.locale.text("title"),
        rows = {
            {id = "enabled", type = "toggle", label = game.locale.text("enabled"), value = state.enabled},
            {id = "run_impact", type = "toggle", label = game.locale.text("run_impact"), value = state.run_impact},
            {id = "punch_impact", type = "toggle", label = game.locale.text("punch_impact"), value = state.punch_impact},
            {id = "push_downed", type = "toggle", label = game.locale.text("push_downed"), value = state.push_downed},
            {id = "player_enabled", type = "toggle", label = game.locale.text("player_enabled"), value = state.player_enabled},
        }
    })
end

game.on("initialize", function()
    if game.state.options then state = game.state.options end
    game.state.options = state
    register_menu()
end)

game.on("level.ready", function()
    game.physics.knockdowns(state)
    game.log.info("[euphoria.ragdolls] Experimental knockdowns enabled")
end)

game.on("code.reloaded", register_menu)

game.on("ui.action", function(event)
    if event.page ~= "euphoria_ragdolls" then return end
    if state[event.row] ~= nil and type(event.value) == "boolean" then
        local next_state = {}
        for key, value in pairs(state) do next_state[key] = value end
        next_state[event.row] = event.value
        game.physics.knockdowns(next_state)
        state = next_state
        game.state.options = state
        register_menu()
    end
end)
