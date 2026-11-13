# MMdl native model definitions

`sub_82719770` relocates a version 10 MMdl payload, adding its base to stored
relative pointers. `sub_82719B50` performs the inverse operation. The host
reader/writer and reference fixture use these original routines to validate
the pointer layout without launching the game.

The big-endian header is 88 bytes. It starts with `MMdl`, version, payload size
and flags; four GUID words follow at +16. Table pointers at +44 through +64
select references, states, variables, properties, conditions and parts. Their
six unsigned 16-bit counts are at +76 through +86. Record sizes are 32, 32,
12, 24, 12 and 20 bytes respectively.

The forward routine also relocates +68 and +72. The inverse does not undo
+72. The host reader rejects a nonzero +72, and unknown +68 data keeps
out-of-line value edits read-only. All 2,616 installed model definitions in
the private inventory pass the supported layout without this exception.

## Records and binding

Reference records contain a native resource type at +8, a GUID at +12 and a
runtime binding at +28. `sub_826DECA0` sets header flag 4, resolves these GUIDs
through the original resource registry and stores the bindings. Wire data must
have zero runtime bindings and no bound flag.

Property records contain the value at +0, name and owner/class strings at +4
and +8, a name hash at +16 and the native type at +20. The original relocator
fixes the value only when `type & 0x1B8` is nonzero. Reviewed types are unsigned
32-bit integers (2), floats (4), strings (8), resource references (16), matrices
(32) and three-component vectors (128). A matrix holds sixteen floats; a vector
holds three. `sub_82719360` and `sub_827193D0` copy those respective layouts.
Other kinds remain preserved, and unreviewed pointees prevent overlapping edits.

Condition records store the value at +0 and a variable pointer at +4. The
relocator fixes only that variable pointer. The variable's unsigned 16-bit type
at +8 determines scalar inspection; condition pointer-like values must not be
treated as ordinary property pointers. Comparison codes remain unnamed until
their evaluation routines are reviewed.

Part records contain type and name pointers at +4/+8, a property-pointer list
at +12 and its unsigned 16-bit count at +16. States have type/name pointers at
+4/+8, property/part/condition lists at +12/+20/+24 and corresponding counts
at +16/+28/+30. Each list entry is also relocated. Aliased relocation slots
would be adjusted twice by the original routine and are rejected.

`sub_826E3F88` constructs a ModelPart using its resource-valued Model property.
It requires native type `0x9E3791CF` and passes the bound DFF clump through the
original render-part setup. The live variant policy therefore permits selecting
another already declared DFF reference, while preserving GUIDs, dependency
records, part classes and allocation sizes.

## Loader integration and bounds

The named RenderWare wrapper is little-endian chunk 0x716 with a big-endian
resource header. The header's name padding, GUID, class metadata, indirection
and allocation records locate the MMdl body. The body GUID and declared size
must agree with that wrapper. Fixed-size edits preserve every other wrapper
byte, including alignment padding.

Source documents are captured with the mod's package fingerprint. The adapter
checks committed readable/writable heap regions, the selected model identity,
its exact source hash and all edits before copying the replacement body.
The production hook calls `sub_82719770`'s original body exactly once outside
the optional-edit exception handler. It preserves original return registers
and native exceptions. The original loader retains ownership and lifetime.

The reference fixture compares complete native relocated bodies, exact inverse
round trips, typed edits and guard bytes. Its host-memory seam does not qualify
actual game resource binding, GPU output or device behavior. New geometry,
rigs, animation, collision and cross-level dependency authoring remain separate
acceptance work.

The private Linux Vulkan GameHub comparison selects Frink's existing reference
2 for its first ModelPart. The baseline uses reference 0. Both retain all three
native reference bindings. In the variant, both render states use the selected
DFF geometry; source and owned copy retain matching 26 geometry/material
bindings before and after a simulation update. Front/back captures render the
edited model, removal restores the world count from 798 to 797, and shutdown
returns zero. This qualifies that existing-resource variant rather than new
geometry, arbitrary component edits or other devices.
