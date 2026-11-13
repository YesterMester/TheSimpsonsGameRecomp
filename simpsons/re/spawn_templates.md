# Spawn templates and source streams

The debug spawner and `world.catalog` use native definitions in addition to
observed live objects. The debug menu also has an experimental source browser
for archives belonging to other levels.

## Definition identity and lifetime

The world singleton is `0x82D08BEC`. Its definition hash map is at +72, with
bucket pointer +0, count +4 and colliding definition links at record +12.
`sub_826A1108` hashes all sixteen GUID bytes through `sub_82745390` and compares
the GUID returned by `sub_8269B028`.

Reviewed compiled definitions have record +16 bit 0 set, class hash +28 and a
signed, field-relative GUID offset at +24. Backward references into shared GUID
pools are normal. They are not chunk-relative pointers. Unsupported uncompiled
records remain outside the guarded host mirror.

`sub_826C6168` resolves a GUID prefix through the file manager's four lists at
+8728, +8712, +8704 and +8720. Each file has its load key at +96, native state at
+100 and GUID prefix at +108. The manager singleton is `0x82D096A4`. Only state 4
files lend resident templates. GUID prefixes alone are insufficient: different
global files share prefix zero. Exact ownership comes from the world's layer
containers, linked at world +56/+60, with load key at container +8 and definition
head at +16. Definition +0 points to its preceding link field; +4 points to its
next definition. The guarded reader checks both layers' and definitions'
backlinks. Missing, pending and retiring owners are rejected. `spawn_templates.h`
also checks mappings, arithmetic, bucket membership, cycles and traversal budgets.

A host template retains its definition address, full GUID, class hash, source
load key, world address and host world generation. Selection and creation check
these identities again. A new load at the same address cannot revive an old
handle. Retained labels are keyed by both source load and definition address,
and disappear when either owner or definition leaves the native indexes.

Native `sub_826A3350` inserts definitions separately from constructing live
entities. `sub_8269B738` associates an ordinary live entity with its definition.
The original `sub_826A2468` tears down a source layer's objects and definitions,
then removes retained layer data. A resident clone therefore uses the source's
load key for registration and the existing original destruction path.

The clone's unique direct GUID avoids attaching a borrowed source definition to
its own identity. An allocation reused at a deleted source object's address is
accepted only with a fresh native lifetime; returning an already live instance
is rejected. See [entity registration](entities.md).

## Classes and variants

`sub_82703BD0` returns the sorted factory registry at `0x82D6C08C`. Its entries
are sixteen bytes: class hash, factory, name pointer and component offset.
`sub_82704008` registers those fields; `sub_82703960` performs class lookup.
The spawner copies bounded, hash-checked names and sizes from that registry.

The friendly category table seeds reviewed placeable classes. It no longer
restricts which factory creations are observed. Additional classes need a
registered world object and readable native frame before they contribute
resident templates. Services, unsupported layouts and unproved classes are
excluded. Equal class/model names do not merge distinct definition records;
duplicate display labels include the source GUID's final word.

## Foreign archive loading

`spawn_sources.cpp` keeps a retained session of up to eight additional archives.
A host worker validates SToc v7 metadata before each serialized addition. Child metadata offsets
are relative to the folder root, including names; expansion must fit the
original 32-slot folder stack. Most zone and mission streams have no folder
metadata, so the episode's root archive supplies their StreamTOC first.

`sub_826CB3A0` requests a catalog entry and its dependencies;
`sub_826CA9A0` creates the native file owner when a load is needed.
The folder request uses `sub_826CB888`; `sub_826C5E90` proves that a child is described
by a retained catalog. The queue hook at `sub_826C76B8` records each new load
before IO can start. Pre-existing loads remain borrowed. Identities include the
unique load key, file-pool address and filename, rather than a GUID prefix.

The original SimG reader, `sub_826A18A0`, still allocates, reads and relocates
each whole body. The `sub_826A1818` hook returns false only for owned source
loads, keeping their layers in the original pending list. Individual records
are never copied out of their body: signed offsets can reference shared pools.
Once every owned file is state 4, `sub_826A3350` retains definitions without
constructing source actors. Already committed definitions are recognized by their
exact native pointer. Conflicting GUIDs reject an addition before new definitions
are registered. Catalog warming happens outside mod callbacks;
batch queries validate stream states once and still recheck exact definitions.
Spawning revalidates a selected handle separately.

Retirement uses `sub_823B1A18`, `sub_826C8358` and `sub_826C97C0`. Borrowed files
are protected while an owned request retires recursively. Additions keep a
separate set of uncommitted load keys. Rollback applies a persistent retirement
mask to those keys, protecting older committed files during recursive calls and
later IO callbacks. Only the last session lease retires all accepted files. Native routines
destroy the source layer's objects, remove definitions and release its pending
SimG bodies. Host ownership remains until retirement is observed; closing a
menu does not release it. Native calls run without holding the host state lock.
Normal world teardown requests retirement before invalidating template handles.

Exact-save format 2 retains every archive path, each file's loading root, the
debug selection, file/body identities and world/manager addresses. Format 1
single-source saves remain readable. Restore validates the metadata before applying guest
state, then rebinds ownership to the restored loads without creating actors.
Checkpoint loads clear those host identities and start their checkpoint world.
Loading/releasing sources reject exact capture. Save serialization failures
release prepared audio, and development save commands wait for audio readiness
just like the save screen.

## Checks and remaining acceptance

`tools/modkit/spawn_reference.py` extracts unchanged native GUID lookup/read/hash
routines. The isolated fixture compares selected definitions, negative relative
offsets and hash collisions against those originals. Corrupt pointers, bucket
counts, cycles, stream states, missing mappings and owner reuse are exercised
only through the guarded host reader.

A private Linux hub run enumerates 997 definitions from thirteen source owners
and 213 native classes. It exposes 276 templates, including 106 without live
source instances. A retained ScriptableProp is created at the requested world
position, survives a simulation update and is destroyed through the native
path; the world count returns from 798 to its baseline 797. The game exits
normally. This qualifies that instance lifecycle, not every class's appearance,
AI, collision or script behavior. Windows x64, Windows without AVX2 and Android
ARM64 object compiles are recorded separately from device runs.

A second check uses the visible debug menu's Props action to create a
RotatingDoor in front of the controlled player. The world count rises from
797 to 798, the actual window capture shows the categorized spawner, and the
game closes normally. The earlier owned test removes its instance explicitly;
the debug-created door remains source-owned until native world teardown.

The folder reader accepts all 490 installed archives, including forty folder
headers. The asynchronous library finds 415 files containing SimG bodies.
Synthetic checks cover root-relative names, zero-prefix children, invalid
offsets, cycles, layer/file backlinks and stream-load reuse.

A private Linux Vulkan run opens the actual native source menu and loads
`brt/brt/challenge_mode/challenge_mode_design.str` in GameHub. The original
catalog/global/selected-file chain adds three files and 343 definitions, with
the live object count unchanged at 797. Selectable templates rise from 329 to
494. A foreign ScriptableProp is created and gains its actual model identity
and label, `Brt jetengineupdraft`. Removing its source destroys the owned copy,
returns all fourteen baseline files and preserves the original 844 resource
GUID/payload bindings. Retired handles fail without calling the factory.

An exact capture of 528.7 MB then restores the loaded source and owned prop
after retirement. Native ownership rebinds, all 494 templates are reconstructed,
and releasing the restored source again returns the count to 797 and preserves
the same 844 resource bindings. Both qualification runs exit zero. The Combat
workshop also remains active under its unchanged callback budget after catalog
warming and batch validation.

This establishes the tested source/prop/resource/save lifecycle. Broader class
behavior, encounter prerequisites, repeated campaign streaming, memory pressure
and native Windows/Pixel qualification remain separate work. Factory/resource
registration is not proof that every class can run outside its source scene.


## Library identities and mod-owned sources

Compiled SimG records carry resource identities before any live instance exists.
`sub_8269B588` selects the property namespace and `sub_8269B428` walks its
packed values. Each namespace has a big-endian field count, LSB-first default
bits and an aligned value table; defaulted fields consume no value word.
`sub_8269A7A8` returns a wrapper and `sub_8269B0F8` resolves the GUID relative
to the word containing its signed displacement. Entity namespace `38523FC3`,
property 1 supplies the MetaModel; Animated namespace `AE986323`, property 1
supplies the character bank. The first primary resource is a fallback only
when its named native type is verified as MetaModel.

The shared guarded reader is in `modkit/src/compiled_objects.h`. The native
oracle extractor `tools/modkit/definition_reference.py` keeps the original
routines and their dependencies unchanged. Its fixture compares packed/default
fields and signed GUID references, and separately rejects malformed host
inputs. Byte fields are validated through their containing aligned region so
the same reader works with both archive bytes and the live guest-memory view.

The asset index links compiled definitions to MetaModels, character banks and
MetaModel DFF references. Each association retains its original GUID, class,
archive, entry and record offset. Native stream loading remains responsible
for resources and teardown. A mod generation owns a source claim; shared claims
cannot unload each other's native files. The debug menu tracks its own copies
and removes those independently when a mod still holds the session. Accepted
archives share one lifetime because their native dependencies may overlap.

A successful mod-owned spawn retains the source automatically. An explicit
request for an already committed file, including its episode catalog, shares
the canonical root that originally loaded that file without new native IO.
`SourceLeases` separates accepted selections from pending waiters. A failed or
canceled addition restores the requester's prior lease, including one acquired
by spawning from an older source while waiting. A released code generation
cannot be resurrected by rollback. The world epoch is atomic, so checking source
identity under the source mutex does not invert the spawner's lock order.
Cloning rejects a source while its native retirement is in progress.

Both retirement hooks take the native manager in r3 and the file node in r4.
Protection applies only to that manager and checks exact load key, address and
filename. Every other current file remains borrowed, including level streams
which arrived after the initial request. Reused file-pool addresses do not
establish ownership. `source_state.h` also validates saved group identities and
all mod claims before exact guest-memory replacement; checkpoint restore
discards old-world claims instead.

The dedicated Linux ownership run covers failed preparation, automatic claims
after spawn, episode-dependency sharing without duplicate IO, last-owner
retirement, exact restore of two independent owners and code reload. It returns
to the original 797 objects, fourteen files and 844 resource bindings and exits
zero. The source-state fixture passes 33 checks including the captured exact
profile. All nine changed-file object compiles pass across Windows x64,
Windows without AVX2 and Android ARM64; those are compilation checks, not native
device qualification or broader class-behavior acceptance.

Catalog warming runs before timed mod callbacks, including exact-restore
notification. Parsed definition metadata is cached by source-load key, record
address and checked GUID/class; reset and source retirement invalidate it.
Periodic label refresh is performed by WarmCatalog rather than a random query
inside a user callback. Native class names validate a bounded region once
when possible, with the original byte guard for a name crossing an unreadable
region. CurrentCatalog continues to validate loaded files and exact definition
identities before exposing handles.


## Multiple retained archives

The current Linux Vulkan qualification combines eight archives in GameHub:
Bartman challenge and story definitions, Bargain Bin challenge definitions and
five Bartman zones. The session retains 1,671 definitions across twelve owned
files, with 26 total native files including fourteen borrowed level files.
Three independently owned ScriptableProps keep their full world poses while
later archives load. Resource checks preserve every prior binding during each
addition; the final accepted session has 1,997 native resource identities.

The same run rejects a ninth root before IO, rejects a borrowed level target,
restores previous claims after missing metadata and cancels both a preparation
and a native request with two new files already queued. Earlier objects, file
identities and resource bindings survive those cases. Exact save/restore after
real retirement retains all eight roots and the three full poses; code reload
transfers them without duplication. Owner cleanup and last-lease retirement
return the object count to 797 and the file count to fourteen while preserving
all 844 baseline resource bindings. The process exits zero.

Normal fixtures cover lease generations/cancellation, saved identities and
version overflow, legacy and captured format-2 profiles, and the actual public
Library objects Lua project. Lease/save guards also pass ASan/UBSan. Eighteen
affected-file compiles pass for Windows x64, Windows without AVX2 and Android
ARM64. These remain separate from native Windows/Pixel runtime qualification
and class-specific AI, collision or scripted-scene acceptance.
