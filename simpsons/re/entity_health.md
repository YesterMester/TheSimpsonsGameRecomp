# Native health adapter

The public combat operations are documented in `modkit/COMBAT.md`. Guest
addresses and offsets stay in the adapter, not creator projects.

## Component identity

`sub_8269AD58` walks the component pointer array at entity +52, entries 1 through
the unsigned halfword count at bank +12; bank is entity +48. Entry zero is the
absent-component sentinel. `sub_8269ADC0` instead indexes the bank's numeric
type map at `(type + 7) * 2`. That numeric type is not a hash or a component's
field +4. The adapter walks the bounded bank rather than inventing a type ID.

`sub_82956E70` constructs the recognized health component with vtable
`0x8218AB98`, owner at +8/+40 and 216 bytes. Live family banks contained this
component at different indices (41, 38 and 37), agreeing with their existing
entity +344 convenience pointer. Generic props need not have this component.
Unsupported tables, excessive counts, unreadable data, duplicate health
components and mismatched owners reject before mutation.

## Native operations

`sub_82956FF0` returns current health (+16); `sub_82956FF8` returns the normal
healing limit (+20). `sub_82957000` returns the separate reference value (+24).
`sub_82957018` is the native death predicate and `sub_82956FC0` the enabled
predicate. The original setter `sub_82955FC0` also performs property
notification, so raw hit-point writes are insufficient.

`sub_82957478` is the health message handler (component vtable +4). Damage uses
the message ID stored at `0x82E07CC4`; healing uses `0x82E07CD4`. Its damage
branch copies the input to component +124 through `sub_82937118`, resets the
native recovery timer for absorbed hits, calls `sub_82956900`, then preserves
update registration through `sub_82955D00`. Healing runs `sub_829573C8` then
`sub_82956530`. The normal damage eligibility function `sub_82957208` remains
hooked by the existing family damage-protection service; result 2 ignores a hit.

The temporary 72-byte damage record uses original constructor `sub_82937650`
and destructor `sub_82881400`. Amount is +44 and type is +52. Original type 13
accepts a source-free scripted request while retaining the remaining masks and
class filters. This is a descriptive public use, not a recovered enum name.
The host provides no fabricated source references.

Original hit producers `sub_82938660`, `sub_82938C70` and `sub_82957F78` write
the simulation timestamp from `0x82D572BC` into damage +64. `sub_82938660` also
copies its impact vector to +20 and +32. The adapter supplies the target's
resolved world position and that original clock. Leaving timestamp zero caused
the live family hit to be ignored: `sub_828836F8` compares new and previous
damage timestamps against the character's cooldown before accepting a hit.
The reference check includes this unchanged filter and its original member
callback thunk `sub_82882C48`; same-frame hits remain subject to cooldown.

`sub_829563B8` runs native death callbacks and authored death actions. Its hook
captures only a recognized component satisfying the original death predicate,
queues the owned lifetime before class callbacks can delete it, and always
executes the original routine. The host drains at most 16 death/removal actions
within the existing 2 ms retirement budget. Ownership records deduplicate the
first death per allocation and retain delivery state through reload/restore.
Saving while native notices remain queued reports `busy` instead of freezing
inconsistent encounter progress. No script callback runs inside the hook.

The production adapter checks the lifetime after native callbacks before
reading the target again. Its original-reference fixture includes constructor,
copy/assignment, destructor, damage/heal/death and register-save routines.
World actions, component-property services, class effects and matrix backends
are explicit fixture boundaries and need separate scene qualification.

Delivery follows original synchronous hit producers such as `sub_82883888`:
`sub_8268FAA8(message-global, entity +56, damage-info, 0)` constructs the borrowed
message and calls `sub_8268F998`. That dispatcher respects subscriber +4 bit 0,
enters the entity's virtual handler and preserves message-type dispatch flags
and deferred subscriber retirement. Entering only the health receiver would
omit other listening components and the subscriber activation check. The
adapter uses this original dispatcher for both damage and healing, without the
force flag. The native fixture also executes both unchanged dispatch routines;
its synthetic entity receiver is an explicit scene boundary.
