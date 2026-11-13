# Authored missions

The mission service keeps a mod's objectives, prerequisites, progress, failure state and
retry points in native code. Lua and C++ use the same operations. Your gameplay logic
checks conditions through supported game APIs and advances the appropriate objective.

This service adds authored missions alongside the campaign. Original campaign game-flow
integration, general encounter hooks and mission HUD presentation remain separate
parts of the full kit. The [Mission editor](MISSION_EDITOR.md) authors route
objectives and shared prop scenes through the native menus. [Combat workshop](COMBAT.md)
combines this service with owned targets and real native death notifications;
ordinary removal does not award a defeat.

## Start with the working project

```sh
python3 tools/modkit/modkit.py new "My route" --id yourname.route --author "Your name" --template mission-walk
python3 tools/modkit/modkit.py validate "My route"
```

The project creates **Options → Extras → Mods → Mission example**. Load a level, then
start the mission there. Move a family member 12 units from their starting point and
bring an explorer back. Each loaded member has their own origin; an inactive member
already elsewhere, or waiting at home, cannot finish somebody else's route. The mission
has a simulation-time limit, a recorded objective retry point, failure and replay actions.
The source comments explain every part. Creation and package import leave it disabled.

The family list is not a logical controller assignment. The example uses character identity
for its small shared route and reacquires live handles for each query. More elaborate mods
need the player/entity lifecycle and assignment APIs appropriate to their roles.

## Define objectives

Register definitions during `initialize`, or recreate them during `state.loaded` when
they depend on restored mod state:

```lua
game.on("initialize", function()
    game.missions.register({
        id = "delivery", title = "Special delivery", revision = 1,
        objectives = {
            {id = "collect", title = "Collect three items", goal = 3},
            {id = "deliver", title = "Deliver the items", requires = {"collect"}}
        }
    })
end)
```

Ids are local to the mod, so two mods can both use `delivery`. Goals default to 1.
Prerequisites must refer to existing objectives and cannot form cycles. An objective
marked `optional = true` does not block mission completion. At least one objective
must be required. Put displayed text in locale catalogs for translations.

Start an idle mission from a gameplay or menu callback with `game.missions.start("delivery")`.
When your script has verified a condition, call
`game.missions.advance("delivery", "collect", 1)`. Progress clamps to the goal. Locked
objectives reject progress; completing every required objective completes the mission.
The service does not manufacture pickup, death or encounter events that the game adapter
has not implemented.

## Inspect and react

`missions.get(id)` returns a snapshot. `missions.list()` lists this mod's definitions and
progress. Mission status is `idle`, `active`, `completed` or `failed`. Objectives include
`progress`, `goal` and a state such as `locked`, `active` or `completed`.

```lua
game.on("mission.changed", function(event)
    local mission = event.mission
    game.log.info(mission.title .. ": " .. mission.status)
end)
```

Each event includes the current mission, change reason and a revision. Multiple changes
to one mission coalesce. A callback cannot recursively deliver its newly queued changes
in the same tick. Delivery rotates through a shared budget, up to 32 callbacks per tick;
the time budget is checked between callbacks. Keep callbacks short. Events belong to the
defining mod and disappear when that owner retires. Undelivered changes are saved, so a
restore does not lose a pending completion notification.

The native menu APIs can display progress and expose start/retry actions, as the example
does. Creator inspection also includes each mod's mission state. C++ plugins use
`RegisterMission`, `StartMission`, `AdvanceObjective`, `Mission` and `Missions` on `Game`.

## Failure and saves

`missions.fail(id, reason)` fails an active mission. `missions.checkpoint(id)` records its
current objective progress. `missions.restart(id)` retries from that point when one exists;
passing `false` starts from the beginning and discards the old retry point.

An objective retry point changes mission progress. It does not rewind the world. Restore
associated objects and world effects through their supported APIs, or use a game checkpoint.
The game's checkpoint/exact-save sidecars separately retain the mission service state with
the rest of the mod profile. Store your script's own coordinates, choices and counters in
`game.state`; keep live handles out of saved state.

## Code reload and migration

An unchanged objective contract restores automatically after `state.loaded`. Titles,
descriptions and display order may change. Goals, prerequisites, optional flags and revision
changes require a migration. Missing old definitions also need an explicit migration or a
fresh project session.

The callback receives this mod's saved service state as `event.missions`. Edit a copy of
that snapshot to match the new definitions, then pass it to
`game.missions.restore(event.missions)`. This operation is available only during that
preflight callback. Validate both current progress and any retry point, and keep completion
and prerequisite states consistent. An invalid migration rejects the replacement before
the old runtime is retired. New definitions absent from the old save start idle.

Ordinary save callbacks may inspect missions, but cannot register, advance, restore or
otherwise change them. Native plugins follow the same rules through the shared host API.

## Limits

A mod can register 16 missions, with 128 across the runtime. A mission has at most 32
objectives. Ids contain 1 to 80 lowercase ASCII letters, digits, underscores or hyphens.
Goals and increments must be finite and no greater than one billion. Goals are positive;
increments may be zero. Definitions fit 64 KiB. Registration reserves enough room for
progress, retry points, pending events and escaped failure text within a 256 KiB saved
service quota per mod. Oversized definitions fail before replacing an existing mission.
