# Entity registration and removal

The factory (`sub_82703CA8`) creates a class instance and calls its initialization
virtual. Normal loaders such as `sub_826A3418` and `sub_826A35C8` separately call
`sub_826A31F0(world, guid, record, entity, layer)` afterwards. Factory success alone
does not establish world membership.

Some class initializers register before returning (observed for `ScriptableProp`).
The clone path checks actual world links, unregisters that new instance through
the original helper and then performs unique direct registration. A factory that
returns its existing source instance is rejected without destroying the source.

`sub_826A31F0` copies the 16-byte GUID to entity +28, sets flag `0x20000000`, increments
world +164, records the source layer key at entity +24 and inserts entity +4/+8 into
that layer's entity list. Its record flag bit 2 selects a direct entity GUID index
at world +88 (`sub_826A2AE8`). Other records use a separate streaming GUID node at
world +72. A cloned live entity can use the direct branch without creating or
retaining a fake streaming record. The world singleton is at `0x82D08BEC`.

The unregister flag is not proof of membership: constructors may set it before
the loader inserts the object. Actual membership has a readable back-link at
entity +4 whose target points back to this entity. A constructed clone rolled
back before registration must suppress that default unregister flag.

`sub_826A11E0(world +88, guid)` looks up a direct entity by all four GUID words,
following entity +44 for hash collisions. The clone's unique GUID must be in place
before indexing; changing a linked entity's GUID leaves its index inconsistent.

`sub_8269F990(entity)` is the original single-object destruction dispatcher. For
flag `0x08000000` it calls `sub_8268F538(entity +56)`, which sends the destruction
message through subscriber virtual +4 and calls subscriber deleting virtual +0
with flag 1. Other objects call their own deleting virtual +0 with flag 1. World
cleanup routines `sub_826A20E0` and `sub_826A2468` use the same dispatch. Calling a
base destructor directly would skip class-specific physics and component cleanup.

The base destructor `sub_8269BC10` releases its factory bank and, when required,
calls `sub_826A16F8(world, entity)`. That unregister routine removes the direct GUID
entry, decrements world +164 and unlinks entity +12/+16 and +4/+8. The handwritten
base-destructor hook invalidates host lifetime identities before the original runs.
No mod callbacks or guest calls run from that bookkeeping hook.

`tools/modkit/entity_reference.py` extracts these unchanged generated routines for
the isolated original-routine check, retaining source hashes and rejecting new
unreviewed dependencies. Fixtures exercise both destruction paths, world-list
unlinking, source-layer registration, GUID indexing and hash collisions. These
fixtures do not qualify every class, placement or cross-level dependency graph.
Public mod contracts and current boundaries are in [the ownership guide](../../modkit/ENTITIES.md).

## Frame and position dispatch

Entity virtual +28 is the position setter. `sub_82696D10(entity, position)` writes
the three translation words at frame +64, calls `sub_823F2460(frame)` to queue a
RenderWare update, then dispatches entity virtual +92. The base notification
`sub_82697160` marks entity +202 bit `0x0800` and notifies a non-null model at +136
through `sub_826A8708(model, 1)`. Calling only the base setter skips overrides:
the character setter `sub_8288D2E0` also calls controller +784, virtual +60, with
the position and flag 0. The ScriptableProp and all four family vtables observed
in a private Linux GAME_HUB run agree on setter slot +28 and notification +92.

The frame at entity +140 stores its modelling matrix at +16 and cached world
matrix at +80. Parent is frame +4; hierarchy root is frame +160.
`sub_823F2540(frame)` returns the modelling matrix for a root or the resolved
world matrix for a child. It inspects the root's dirty flag and updates parent
matrices through `sub_823F24D8` before composing the child. A root's cached world
matrix can be stale even when its modelling matrix is current; copying either
translation directly is not a substitute for the getter.

`entity_transform.cpp` bounds and validates the hierarchy, uses that getter for
world reads, and dispatches the class setter for placement. A parented world
destination is converted through the inverse affine basis in double precision.
The adapter preserves local basis/scale, rejects nonfinite coordinates, cycles,
inconsistent roots and singular parents, and has no memory-scanning fallback.

`tools/modkit/transform_reference.py` extracts the unchanged original setters,
model notification and frame getter/update/dirty routines for the production
adapter fixture. Matrix-backend and controller/model boundaries are explicit
fixture seams; the fixture does not establish every class's collision or
script behavior. Live runs and platform qualification are recorded separately.

## Full matrix dispatch

Entity virtual +40 is the full local-matrix setter.
`sub_82697690(entity, matrix)` computes the three cached scale values at entity
+144/+148/+152 through the original vector-length routine, replaces the frame's
modelling matrix through `sub_823F2748`, marks the frame dirty and dispatches
entity virtual +92. The observed ScriptableProp vtable uses that method.
The character override `sub_8288D428` calls it and then dispatches controller
+784, virtual +56, with the matrix and flag zero. All four previously captured
family-character vtables use this override at +40.

`sub_823F2748(frame, matrix, combine)` applies the RenderWare matrix operation
and marks the frame dirty. Entity cached scale and class notifications belong
to the entity setter above, rather than to that frame wrapper.

`entity_transform::Set` validates the entire input basis before a guest call,
converts world directions and position separately through the parent's affine
inverse, and dispatches that class method. A general affine argument matrix is
written only to guarded guest stack scratch, with no orthonormal/identity flag
claim. Native methods own changes to the entity, its cached scale, model and
controller. Position-only authoring continues through virtual +28.

The production adapter fixture runs unchanged original full setters, their
controller dispatch, vector-length arithmetic and frame replacement wrapper.
RenderWare's matrix backend, the TLS square-root lookup and controller/model
boundaries are explicit fixture seams. It checks root and parented full poses,
reflection, nonuniform scale, shear, native notifications, cached scale and
invalid bases/frames/stacks, alongside the existing position checks. Its 10,055
checks pass normally and under AddressSanitizer/UndefinedBehaviorSanitizer.
Native rendering, the lookup's scale rounding and class behavior require the
separate game qualification; the fixture does not establish those by itself.

The Linux game qualification exercises the unmodified Owned objects Lua menu
actions on a foreign ScriptableProp, then applies two independent full poses
through the public API. Invalid matrices and another mod's object are rejected.
Exact restore and code reload preserve both poses, while retired handles stay
expired. Reading the saved native objects by their exact GUIDs also confirms
their cached scales: `(1.5, 0.75, 2)` and `(1.25, 1.5, 1.75)`, matching their
local basis lengths. Last-owner cleanup returns to 797 objects, fourteen files
and the original 844 resource bindings; the game exits zero. All twelve changed
file object compiles pass across Windows x64, Windows without AVX2 and Android
ARM64. Broader class behavior and native Windows/Pixel runs remain separate
acceptance work.
