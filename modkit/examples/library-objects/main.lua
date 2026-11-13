-- Modders: this GUID identifies an original MetaModel, not a guest address.
-- Find another with assets.search or the debug menu's Game-wide library.
-- This example loads original object data, including its native dependencies;
-- copying a mesh alone would omit collision, animation and behavior settings.
local resource = "2090294A95092EDA95092EDA95092EDA" -- Jet engine updraft
local asset, variants, selected = nil, {}, 1
local status, waiting, frames = "Open this page in a level, then inspect the library", false, 0

local function page()
    local choice = variants[selected]
    game.ui.register_page({id = "library", title = "Library objects", rows = {
        {id = "asset", type = "text", label = asset and asset.name or "Library example"},
        {id = "inspect", label = "Inspect original object definitions"},
        {id = "variant", type = "text", label = choice and
            (selected .. "/" .. #variants .. ": " .. choice.name .. " (" .. choice.class .. ")") or "No definition selected"},
        {id = "source", type = "text", label = choice and choice.stream or "No source loaded"},
        {id = "retained", type = "text", label = #game.assets.source_streams() .. "/8 source archives retained"},
        {id = "previous", label = "Previous original variant"},
        {id = "next", label = "Next original variant"},
        {id = "load", label = "Load this variant's source"},
        {id = "cancel", label = "Cancel my pending source addition"},
        {id = "spawn", label = "Spawn beside my character"},
        {id = "status", type = "text", label = status},
        {id = "clear", label = "Remove my objects and release my sources"}
    }})
end

local function definitions()
    if not asset then return end
    variants = {}
    -- Page results in larger tools. This small example uses the first 100
    -- definitions of one model; each retains its original GUID and source.
    local result = game.assets.templates(asset.id, 0, 100)
    for _, item in ipairs(result.items) do
        if item.supported then variants[#variants + 1] = item end
    end
    selected = math.max(1, math.min(selected, #variants))
end

local function inspect()
    local result = game.assets.search(nil, nil, 0, 10, resource)
    if not result.ready then
        waiting = true
        status = "Indexing game files: " .. result.files .. "/" .. result.total_files
        page()
        return
    end
    waiting = false
    asset = result.items[1]
    definitions()
    status = asset and "Choose a variant, load its source, then spawn" or "Model absent from installed files"
    page()
end

game.on("initialize", page)
game.on("level.ready", function() asset, variants, selected = nil, {}, 1; page() end)
game.on("level.unloading", function()
    -- Handles from the old world have expired; native teardown owns removal.
    asset, variants, waiting = nil, {}, false
    status = "Load a level before using source objects"
    page()
end)
game.on("code.reloaded", inspect)
game.on("save.restored", inspect)

game.on("frame", function()
    if not waiting then return end
    frames = frames + 1
    if frames % 30 ~= 0 then return end
    if not asset then inspect(); return end
    local source = game.assets.source_status()
    status = source.message
    if source.ready then
        waiting = false
        -- Reacquire current handles after loading; disk asset IDs are never
        -- world.spawn handles, and an unloaded template must not be reused.
        definitions()
        status = "Source ready; choose Spawn beside my character"
    elseif source.phase == "failed" or not source.owned then waiting = false end
    page()
end)

game.on("ui.action", function(event)
    if event.page ~= "library" then return end
    if event.row == "inspect" then inspect(); return end
    if not game.world.status().ready then status = "Load a level first"; page(); return end
    if event.row == "previous" and #variants > 0 then selected = (selected - 2) % #variants + 1 end
    if event.row == "next" and #variants > 0 then selected = selected % #variants + 1 end
    local choice = variants[selected]
    if event.row == "load" and choice then
        -- Additions are serialized, and up to eight archives share one native
        -- dependency session. Loading another source keeps our older objects.
        -- Another mod's pending addition can report busy; do not retry per frame.
        local result = game.system.try_call("assets.load_source", {stream = choice.stream})
        waiting = result.ok
        status = result.ok and "Loading original source" or result.error
    elseif event.row == "cancel" then
        local canceled = game.assets.cancel_source()
        waiting = false
        status = canceled and "Addition canceled; earlier sources stay loaded" or "No pending addition to cancel"
    elseif event.row == "spawn" and choice then
        definitions()
        choice = variants[selected]
        local actor = game.players.controlled(event.player)
        if actor and choice and choice.resident_template ~= "" then
            local p = actor.position
            local result = game.system.try_call("world.spawn", {template = choice.resident_template,
                position = {x = p.x + 4, y = p.y + 1, z = p.z}, radius = 1})
            status = result.ok and "Spawned " .. choice.name or result.error
        else status = "Wait for this variant's source to finish loading" end
    elseif event.row == "clear" then
        for _, object in ipairs(game.world.owned()) do game.world.destroy(object.id) end
        game.assets.release_source()
        waiting = false
        status = "My removal requested; other owners keep the source session"
    end
    page()
end)

-- Do not put template or entity handles in game.state. The host transfers
-- supported source claims and owned objects on reload/exact restore. Checkpoint
-- loads start a fresh world. New scenes still need authored placement and AI.
