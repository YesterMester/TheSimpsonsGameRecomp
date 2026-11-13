# Health and combat authoring

Use the original health system through named operations. These APIs execute on
the game thread and share the usual mod ownership, callback and save guards.

| Target | Read | Scripted hit | Healing |
| --- | --- | --- | --- |
| Loaded family member | `game.players.health(id)` | `game.players.damage(id, amount)` | `game.players.heal(id, amount)` |
| This mod's owned object | `game.entities.health(id)` | `game.entities.damage(id, amount)` | `game.entities.heal(id, amount)` |

Read returns `{current, maximum, dead, enabled}`, or nil when the object has no
recognized health component. The healing limit comes from that component. The
legacy `players.list().max_health` field is the game's separate low-health
reference, which can differ. A generic entity getter returning 1 does not prove
that a prop has health.

Damage and healing accept finite amounts from 0 to 1,000,000. Their reply is
`{before, after, removed}`. Zero is a no-op. Native protection, damage masks,
character cooldowns and class filters can leave health unchanged. `enabled`
describes the component flag; it does not promise that every hit will count.
`dead` uses the game's death predicate, including its immortal-at-zero policy.
Healing respects the native limit and does not resurrect dead actors.

Scripted damage has no attacker or faction attribution. The host creates the
game's original damage record with its current simulation timestamp and the
target's resolved world impact position. It then runs the original entity dispatcher,
including subscriber activation and all listening components, followed by native
eligibility, hit, recovery and death handling. A target needs a supported world
transform for damage. The host never subtracts hit points directly, fabricates
an attacker or clears cooldown/protection flags. `players.refill_health` and
the debug menu's refill now use native healing too.

Native callbacks may remove an object. `removed` identifies a lifetime ending
during the request; `after` is nil if that happened or the health component is
no longer available. Do not assume disappearance establishes a kill, or keep
using a handle without checking its lifetime. Owned operations reject another
mod, another runtime generation and objects queued for removal. Stock objects
cannot be damaged through an owned handle.

```lua
local actor = game.players.controlled(0)
if actor then
    local health = game.players.health(actor.id)
    if health and not health.dead then game.players.heal(actor.id, 10) end
end
for _, object in ipairs(game.world.owned()) do
    local health = game.entities.health(object.id)
    if health and not health.dead then
        local result = game.system.try_call("entities.damage", {entity = object.id, amount = 10})
        if not result.ok then game.log.info(result.code .. ": " .. result.error) end
        break
    end
end
```

C++ plugins have typed `Health` and `HealthChange` records through
`PlayerHealth`, `EntityHealth`, `DamagePlayer`, `DamageEntity`, `HealPlayer` and
`HealEntity`. An absent component becomes `std::nullopt`. The Lua completion
and generated API reference describe the same replies.

## Native deaths and encounter progress

`entity.died` comes from the original health death handler, once per owned
object lifetime. It contains `entity`, which may already be stale. Native hooks
queue the notice; the host delivers it to the owning runtime after the current
callback returns, before removal notices. It does not reenter Lua from a hit.
World teardown uses `level.unloading` instead.

`entity.removed` also covers streaming, explicit removal and ordinary cleanup.
It is not a defeat event. Calling `world.destroy` never awards a native death.
An encounter should advance an objective on `entity.died`, then treat a target
leaving without that event as a separate authored condition.

Queued and delivered death states are recorded in host ownership. Code reload
transfers those states; an already reported death does not count twice. The
game adapter finishes queued combat notifications before applying a reload.
A save requested while notices remain queued reports `busy`; try again after
the next frame. This prevents freezing mission progress before its native death
notification while omitting a target already queued for cleanup.

## Combat workshop

Choose **Combat workshop** in the desktop Modkit application, or run:

```sh
python3 tools/modkit/modkit.py new "My encounter" --template combat-workshop --id yourname.encounter --author "Your name"
```

Creation leaves the project disabled. The commented source adds an original
Options page with a loaded NPC template picker, two owned targets, a defeat objective, native test
hits/healing, hide/reveal actions, a simulation deadline, failure and checkpoint retry. Either
logical player can start it beside their own character. Retry creates only
the remaining targets; code reload rebinds existing objects without duplicates.
Natural deaths and the test-hit action use the same native death event.
The [visibility operations](ENTITIES.md#visibility-and-encounter-setup) preserve
native hide requests across reload; hiding a target does not pause its AI or
make it harmless.

Use **Previous/Next loaded target template** to choose an NPCMelee variant.
Native model GUIDs distinguish templates with equal display labels. Names come
from the original resource loader where available; opening the asset library
can supply additional source metadata asynchronously. **Refresh loaded template
names** updates the picker afterwards. Browsing another model does not
change a running encounter's checkpoint. Retry finds the original model using
a fresh template handle, and fails cleanly if it is no longer loaded instead of
silently substituting another NPC. A missing supported health component also
rolls back the partial encounter.
Author arena positions and qualify enemy AI, collision and script dependencies
for the level you distribute. The generic factory does not establish those
properties for every enemy. This is an encounter authoring workshop; campaign
mission HUD integration, ability/targeting APIs, broader enemy classes and the
visual encounter editor remain in the [full plan](PLAN.md).

The private GAME_HUB staging scene exposed different model resources collapsed
under the old `Brawler` label: Frink and four 8-bit family variants. Native GUIDs
now match their original archive metadata and remain separate catalog entries.
Controlled close views now show the source Frink and its copy alike, with the
same 26 native geometry/material bindings across two LODs, also unchanged after
a simulation update. The earlier appearance-mismatch diagnosis came from
misleading rear views and family occlusion. Original scripts can still configure
more than a factory record supplies. This qualifies the inspected Frink copy;
broader classes, scripted appearances and natural combat AI remain in the gate.

The isolated checks execute the actual workshop source for co-op initiation,
native-event routing, ordinary removal, checkpoint retry, reload, completion,
timeout and failed spawning. The native health fixture executes unchanged game
routines, including the character cooldown filter and intrusive references.
Private Linux scene runs qualify actual resources separately. Windows/ARM64
compilation and Windows fixtures through Proton do not establish native Windows
driver behavior or Pixel device qualification.
