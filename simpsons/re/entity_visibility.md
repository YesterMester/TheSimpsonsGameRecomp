# Native entity visibility

`entity_render.cpp` recognizes the shared original visibility layout by its
hide, show, force-show and scene-attachment virtual entries. Unsupported class
overrides need their own reviewed adapter. Public mods use `entities.render_state`
and `entities.set_visible`, never these guest addresses or native counters.

## Requests and native scene methods

`sub_82697188` increments the byte at entity +206. On the first hide, if the
model at +136 exists, it invokes the entity's virtual +260. `sub_826971C0`
decrements a nonzero count and invokes virtual +256 when the result is zero.
`sub_82697200` clears the counter and dispatches virtual +100 (the show method).
It is an explicit force-show and must not be used to release one owner's request.

The shared scene entries are `sub_826993F0` at virtual +256 and `sub_826994E0`
at +260. Attach calls the model's virtual +40, obtains class bounds through
virtual +236, inserts the +156 spatial node and updates the +160 culling object.
Detach removes the node, calls the model's virtual +44 and updates that culling
object. `sub_826AF408` sets spatial-node byte +26 bit 4 after successful insertion;
`sub_826AF4B0` clears it on removal. This is scene membership, not a camera test.

## Ownership

Each mod-owned lifetime records at most one `render_hidden` lease. A repeated
hide is a no-op; reveal releases just that lease. The adapter rejects increment
at 255 and skips native show at zero, avoiding byte wrap and duplicate scene
insertion. Other authored requests remain counted. A strong C force-show hook
retires the host lease before the original routine resets the native counter.
It performs host bookkeeping only and never reenters Lua.

Reload and exact ownership restore transfer the lease as metadata, without
running hide again. Old three-field descriptors remain valid. Validation accepts
only the optional native death and true visibility metadata; resolution receives
the immutable GUID/class/vtable identity. A fresh checkpoint recreates objects
and their authored visibility. Broader exact-save game qualification remains
part of the full modkit plan.

The game adapter filters out lifetimes already destroyed by the game before
saving. That filtered path must call `Entry::Save()`, the same serializer used
by `OwnedEntities::Save()`. Copying `entry.description` alone loses both native
death-notification state and visibility leases. The live hidden-reload check
caught this integration error; the shared serializer retains that metadata.

These operations do not alter the NPC update list, collision, health, audio,
target selection or state machines. An NPCMelee class label alone does not
establish a particular character model, faction or complete encounter behavior.

## Qualification boundaries

`tools/modkit/render_reference.py` extracts the original hide/show, model attach/
detach and spatial-node flag routines unchanged, with dependency checks and
source hashes. `modkit_entity_render_check` runs the production adapter against
them, with explicit model/container fixture boundaries. Scene tests separately
need to observe rendering, reload and cleanup. Compiler checks establish build
compatibility; they do not establish native Windows or Pixel device behavior.

Private Linux staging checks observe the models disappear and reappear, retain
one hide request across code reload, preserve another owner's prop, complete the
native health/death workshop and return world counts from 797 to 800 to 797,
then exit cleanly. Photo mode holds simulation still for this visual/reload
check. The loaded NPCMelee copies' intended costume/rig and natural combat AI
are not qualified by it.
