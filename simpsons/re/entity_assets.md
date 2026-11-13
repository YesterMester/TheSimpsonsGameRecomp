# Loaded model resources

The creator catalog groups loaded entities by class and supported native model
identity, rather than merging unrelated models under a shared display label.
`world.catalog` exposes `class` and `model_resource`; the latter is a native
16-byte GUID written as 32 hexadecimal digits. It is not a guest address or a
handle accepted by `world.spawn`. Asset-library locations expose the same
`resource_id`, and `assets.search` can filter it.

## Model and resource layout

`sub_826A8760` creates an MMdl model with vtable `0x820B7030` and retains its
relocated prototype at model +16. The reviewed prototype has `MMdl` magic,
version 10 and a GUID at +16. Entity +136 is the model pointer. Other model
classes and versions need separate adapters; they return no native identity.

`sub_82711460` resolves a nonzero GUID through the resource registry at
`0x82D6CADC`. `sub_82710EB0` selects a hash bucket, follows resource entry +44,
and compares all four GUID words at +28. Entry +8 is the actual payload pointer.
`sub_82745390` hashes sixteen big-endian GUID bytes using `hash * 65599 + byte`.
The host mirror checks mappings, bucket counts, arithmetic and a finite chain
budget. A matching GUID must also retain this model's actual prototype before
its name can be used.

## Names without another disk scan

`sub_82711B28` reads a named resource header. Before its first read, stream +12
is a cursor into the +20 memory buffer, pointing at the header-length word.
The next word contains the padded name length. Name bytes follow it, then the
sixteen-byte GUID. The original function advances the cursor and loads or
reuses the resource through its original class reader.

The observer copies bounded metadata before calling that reader exactly once.
After it returns, only a matching, registered MMdl v10 payload can contribute
a name. The cache owns its strings, verifies the current payload on queries,
marks conflicting aliases ambiguous and retains at most 4,096 names. Guest
memory reset clears it. It does not change the original reader's registers,
exceptions, file cursor or resource ownership, and performs no file reads.

The asynchronous asset library separately reads the original named-header GUID
and exposes archive/source metadata. Its native-ID lookup rejects ambiguous
source matches. Catalog queries only peek at an existing library snapshot;
opening/searching the library is the explicit operation that starts its worker.

## Evidence and limits

The original GUID lookup and hash routines are extracted unchanged for the
resource fixture. Sparse protected pages catch unexpected accesses; the
production header observer additionally checks exact original-call inputs,
outputs, exception propagation, copied strings, missing loads, replacement
allocations and conflicting names. The named reader's I/O/class-loader endpoint
is an explicit fixture boundary, qualified separately in the real game.

In the private GAME_HUB scene, seven NPCMelee instances contain six model
identities: Sherri twice, Frink, and the four 8-bit family variants. All six
loaded GUIDs match their original archive's named header and embedded MMdl
payload. The old `Brawler` row collapsed Frink and those four family models.
They now remain separate, with readable metadata names when available.

Matching model IDs do not establish correct rendering, natural combat AI,
script-assigned appearances, every distinct behavior record or dependency-aware
cross-level loading. The inspected Frink copy uses its source's model/rig
identity. Controlled close views from both sides show matching appearances,
and both retain the same 26 native geometry/material bindings across two LODs
before and after a simulation update. The previous mismatch diagnosis was not
supported by that comparison. Other classes and script-assigned appearances
remain acceptance work in the full modkit plan.
