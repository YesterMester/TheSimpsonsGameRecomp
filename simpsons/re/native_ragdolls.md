# Character tools and native ragdolls

The bundled `euphoria.ragdolls` package is currently **Experimental Knockdowns**,
disabled in the default profile. Its original implementation rotated and moved
whole characters. It did not create skeletal rigid bodies or constraints, solve
terrain collision, or implement an Euphoria controller. Keep it disabled for
normal campaign play. Its ID is retained for profile compatibility. Driving its
player knockdowns with the native rig was tried and not shipped (see
[Native knockdowns: tried, not shipped](#native-knockdowns-tried-not-shipped)).

## Repairs

- Knockdown controls use `physics.knockdowns(options)`, an owner-aware resource
  request. Unload, callback failure and release restore the prior request or
  manual baseline. Menu page names cannot change global cvars.
- Motion uses the guest simulation clock. Repeated draws, pauses and clock
  resets advance nothing; high render rates have no artificial minimum step.
  Disabling restores surviving actors on the game thread. Effects are bounded
  to 64 active actors and expire even if a defeated actor never despawns.
- Damage reactions are queued after the original eligibility check accepts a
  hit. A verified health component and lifetime identify the target; props are
  not interpreted using family-member health offsets.
- NPC/animation selection uses authored GUID, class and model relationships.
  An unsupported asset cannot substitute the first NPC or a random clip.
- The old AI toggle called a nonexistent address (`0x8288FE80`) and an interior
  branch (`0x8288F84C`) requiring its caller's stack/registers. Those calls and
  arbitrary writes to NPC input fields are removed. General AI suspension
  reports unsupported until a controller adapter is reviewed.
- NPC costume previews balance their own hide request and validate actor
  lifetimes. Native force-show cancels the request; cleanup cannot consume a
  later authored hide. The preview keeps the real family HUD and abilities.
  It is not NPC possession or a signature-attack implementation.
- Spooner replacement checks pose application and old-actor removal before
  committing. Failed replacements retire their new copy. Existing map actors
  are parked reversibly; their scripts and AI remain active. Failed restoration
  retains the undo record for retry. Native force-show cancels its hide lease.

## Animation catalogs

The animation browser now follows the native bucket/chain table instead of
interpreting it as a binary tree. The bank's +48 field points to the table;
the table stores its bucket array at +0 and bucket count at +4. Each node
contains a key, authored name, clip pointer and next-node pointer at +0/+4/+8/+12.
The original lookup routines are `0x8278AD00` and `0x8278B478`.

Traversal bounds the buckets and nodes, rejects cycles, duplicate keys, wrong
buckets and unreadable clip storage, and returns no partial catalog after a
structural error. Authored labels must match the original case-insensitive
65599 hash; missing or invalid labels use the clip's hexadecimal key. Results
have deterministic key order, and playback accepts only a listed native key.

```sh
python3 tools/bench/animation_catalog_check.py --output /tmp/animation-catalog-check
```

The fixture passes 70,794 assertions and 16,833 calls to the unchanged native
lookup routines. It includes colliding buckets, randomized banks, the three
authored family get-up clips, malformed names and tables, and traversal limits.
Windows x64, NoAVX2 and Android ARM64 compile checks pass for the fixed browser.
The November 12 private Linux/Vulkan run lists 324 Bart clips and 194 Homer
clips, with every returned pointer matching the original bank lookup. All
three authored recovery clips play on both characters. The same run observes
60 native bone writes during the passive player test, automatic restoration,
successful walking afterward and clean exit without logged errors.
Clip enumeration and direct playback do not supply the controller and collision
handover needed for automatic ragdoll get-up.

## Real Havok joint definitions

`native_ragdoll_joint.cpp` introduces a checked internal adapter for the game's
actual `hkRagdollConstraintData`. It constructs fresh, unpublished definitions
using `sub_82B20270`; it does not synthesize a solver or replace native physics.

Reviewed executable contracts:

| Contract | Address / layout | Evidence |
|---|---|---|
| Joint constructor | `0x82B20270` | Initializes reference count, native vtable, local frames, atoms and limits. |
| Native vtable | `0x821A07D4` | Written by the constructor; matches the executable's ragdoll type registration. |
| Storage | 320 bytes, aligned to 16 | Last ball/socket atom starts at +312; vector frames require aligned loads. |
| Body-local joint frames | +32 and +96 | Independent native transforms, each with three basis columns and a translation. |
| Joint validity | `0x82B1FF18` | Validates frames and twist/cone/plane limits using `0x82B5FF80`. |
| Boolean return | Hidden output pointer in r3; joint in r4 | The native validator writes a byte through r3. Treating r3 as `this` corrupts the vtable. |

The adapter rejects scaled, reflected, degenerate or non-finite frames before
mutation. Failed construction/validation restores unpublished storage. Guest
call scopes preserve registers and the actual host floating-point mode on
success and exceptions.

Run the original-code check without opening the game:

```sh
python3 tools/bench/ragdoll_joint_check.py --output /tmp/simpsons-ragdoll-joint-check
```

The Linux check passes 16,067 assertions, including 3,000 independent
frame/pivot pairs, 1,000 custom limit profiles, native rejection of inverted
twist limits, invalid input and interrupted construction. Custom profiles alter
only the eight verified twist/cone/plane fields; constructor axes, motors,
padding and the cone-minimum sentinel remain intact. Invalid limits are rejected
before allocation or mutation. The generated constructor, matrix validator, joint
validator and register helpers execute unchanged; their hashes are recorded in
the output. These are **joint-definition checks**, not world simulation or
playable character qualification.

## Native collision bodies and joint instances

`native_ragdoll_body.cpp` builds solid capsules with the game's actual shape,
rigid-body and constraint-instance constructors. These are internal building
blocks for a rig; normal play does not create them yet.

| Contract | Address / layout | Evidence |
|---|---|---|
| Capsule constructor | `0x82AFB470` | 48-byte shape, aligned endpoint vectors at +16/+32; radius passed in f1. |
| Rigid-body defaults | `0x82AB0978` | Native Cinfo initialization, including motion, deactivation, friction, restitution and damping. |
| Body configuration | 192 bytes, aligned to 16 | Position +16, quaternion +32, velocities +48/+64, inertia +80, center of mass +128, mass +144. |
| Rigid-body constructor | `0x82AB0288` | 512-byte native allocation, category 43; vtable `0x8219AE44`. |
| Body readback | `0x82AAFE30` | Copies the actual motion's mass, inertia, pose and velocities back to Cinfo. |
| Constraint instance | `0x82AB3940` | 44-byte native allocation, category 42; retains both bodies and constraint data. |
| Native reference release | `0x82AAC0A0` | Calls the virtual destructor at the last owned reference; no host heap substitutes. |

Capsules use their local Y axis and a centered, uniform-density mass model.
Inertia includes both the cylinder and the displaced solid hemispheres. World
pose and velocities are independent of that local inertia tensor. Inputs that
overflow native float32 bounds or flush mass/inertia to zero are rejected.

Fresh-definition failure restores both storage regions. Heap creation uses the
game thread's native Havok allocator, preserves the default deactivator, and
transfers the temporary shape/data references to the body or joint instance.
The release helpers reject objects belonging to a world and bodies still owned
by a joint. Callers must release the joint before releasing its bodies.

The definition check runs without opening the game:

```sh
python3 tools/bench/ragdoll_body_check.py --output /tmp/simpsons-ragdoll-body-check
```

It passes 3,094 assertions using the unchanged native capsule/Cinfo routines,
independent numerical integration of capsule volume, native defaults, input
validation and interrupted construction. Windows x64, Windows without AVX2 and
Android ARM64 also pass source compile checks for the new adapter.

A private Linux/Vulkan game check passes 64 linked-body cycles: two bodies and
one actual `hkConstraintInstance` per cycle. It reads mass, inertia, pose and
velocities through the original native copy routine, verifies shape/deactivator
ownership and native joint validity, rejects premature body release, then
releases the joint before its bodies. The primary body allocator reuses one
address, and the game continues playing and closes with exit 0.
An earlier immediate post-load close aborted during shutdown; its cause remains
undetermined and is retained in the private evidence. These results do not
qualify world insertion, terrain collision or rendered skeletal ragdolls.

## Native family skeleton snapshots

`native_ragdoll_pose.cpp` reads the actual family rig and animated pose. It
resolves the skinned clump using the native resource traversal, obtains its
hierarchy, and matches native bone IDs to animation tracks and render-palette
indices. It copies values into track order with validated parent indices;
guest pointers do not escape the query. The snapshot includes actor lifetime,
world generation, model GUID and the actor's resolved world transform.

The reader requires a complete mapping, finite matrices, unique indices and
topologically ordered parents. It rejects stale actors, unsupported resources
and changes during the query. The native lookup and description helpers use
different player indices (+8 and +10); mixed-description transitions are
rejected rather than combining their data.

The private Linux/Vulkan check reads all 104 animated Bart tracks and 92 Homer
tracks. Every matrix matches the live native palette exactly; both palettes
remain byte-identical after reading. Each model has three additional nodes
outside its animation tracks, which are preserved. Invalid actor identities
are rejected. The game continues running and exits with code 0; the reader
also passes Windows x64, Windows without AVX2 and Android ARM64 compile checks.

This is a **current-pose reader for family actors**, not a bind-pose importer,
NPC rig adapter or physics-to-skeleton writer. Animated matrices may include
scale and must not be passed directly to Havok as rigid rotations.

## World ownership and collision filters

`native_ragdoll_world.cpp` owns a complete native rig behind a host handle.
It constructs the bodies and constraints before adding them to the game's
existing Havok world. It verifies actual world membership after each insertion,
rejects locked worlds, and retains incomplete rigs until safe cleanup succeeds.
An actor may own one rig; the internal limits are eight rigs, 32 bodies and
31 constraints per rig. Definitions must form a connected tree.

Joints are removed and released before their endpoint bodies. The collision
filter is freed last. Lost actor identities or source generations trigger
cleanup before the next physics step. World identities include a host
generation, so a new world at a reused address does not inherit an old rig.
The real world destructor invalidates the identity before native teardown and
releases remaining owned references afterward.

| Native operation | Address | Contract |
|---|---|---|
| Add body | `0x82AA2BB8` | World, body, activation mode; verify body membership. |
| Add constraint | `0x82AA75F0` | World, constraint; verify its owner. |
| Remove body / constraint | `0x82AA2DC0` / `0x82AA77B0` | Hidden `hkBool` output pointer in r3, world in r4, object in r5. |
| Full world destructor | `0x82AA43F0` | Called by deleting destructor `0x82AABF80`. |
| World memory report | `0x82AA2250` | Vtable slot 1; this is not the destructor. |
| Actor collision group | `0x8269BAF8` | Uses the native actor's allocated 12-bit group. |
| Game filter data / encoder | `0x827A4D28` / `0x827A4A90` | A 20-byte native allocation encoded as a tagged token. |
| Game pair predicate | `0x827A4D50` | Native layer/type masks, actor groups and exclusions. |

The game's filter subclasses Havok's group filter but uses its own tagged
format. Stock Havok bit packing is incompatible and can be interpreted as a
pointer. All owned limbs use their source actor's group: internal contacts and
contacts with the original actor's colliders are suppressed. Other groups,
scenery and native exclusions remain effective. Bodies do not reference-count
the filter's raw data, so the rig owner retains it through world removal and
body destruction.

The original-code body fixture now also checks the game's collision predicate
across all 4,095 nonzero groups, compact native actor tokens, all 32 layers and
foreign exclusion masks. It passes 65,098 assertions. The private Linux/Vulkan
world test observes native gravity and scenery contact for two independently
owned two-body rigs. Invalid joint/body definitions roll back; stopping one
actor leaves the other active; 12 repeated construction/removal cycles and
idempotent cleanup pass. The game continues and exits with code 0, without
logged errors.

Exact save capture and restore refuse while any owned rig remains, including
pending cleanup. A snapshot must not rewind native allocations underneath host
ownership records. Successful restores invalidate observed world identities.
This does not serialize an active ragdoll or provide character recovery.

These are internal physics foundations. The live check does not yet qualify
world teardown during a level transition, skeletal driving, anatomical limits,
native Windows drivers or an Android phone.

## Anatomical family rigs and checked palette writing

`native_ragdoll_rig.cpp` builds a connected 13-body, 12-joint biped from the
native named skeleton: pelvis, chest, head, upper/lower arms, upper/lower legs
and feet. The case-sensitive native bone hashes identify the anatomy; track
numbers are not hardcoded. Required ancestry, unique IDs/palette indices and
finite matrices are validated before constructing a definition. Capsule
proportions and mass fractions are authored defaults, not recovered collision
assets. Each joint has an explicit passive twist/cone/plane profile, with tighter
lateral limits on elbows and knees. These symmetric limits are relative to the
captured entry pose; they are not bind-pose calibrated anatomical hinges.

Every animation track keeps its entire affine frame relative to its nearest
physical ancestor. Facial/helper bones, nonuniform scale and deliberately
collapsed helper bones survive reconstruction. The three model-only nodes
remain outside the mapped tracks. Rigid, right-handed body/joint frames are
separate from these affine skinning frames. Checked affine composition and
relative conversion also support a moving/scaled render root.

Run the deterministic definition/math fixture without opening the game:

```sh
python3 tools/bench/ragdoll_rig_check.py --output /tmp/simpsons-ragdoll-rig-check
```

It passes 23,481 checks, including an independent homogeneous matrix oracle,
rotated/sheared/scaled parents, world-space motion, quaternion reconstruction,
coincident joint frames, total mass, passive limit profiles and malformed anatomy. These are not live
controller or campaign checks.

The private Linux/Vulkan check creates both Bart and Homer rigs in the actual
world, reconstructs all 104/92 native tracks from the original body's quaternion
readback within 0.000004 per matrix component, and leaves the original palettes
byte-identical. Both rigs simulate simultaneously. Maximum measured joint pivot
separation is under 0.009 world units in that check. Eight repeated lifecycle
cycles pass, ownership is released, play continues and shutdown exits 0 without
logged errors.

`WriteFamilySkeleton` revalidates the current actor, world, model, native track
mapping and render root before writing. It validates every candidate bone
before changing any spatial component. Native padding and model-only nodes are
preserved. A separate private live check writes both actual simulated poses,
reads every component back exactly and restores each entire palette byte for
byte. Eight stale/malformed candidates per actor are rejected without changes.
The math, rig and writer also pass source checks for Windows x64, Windows without
AVX2 and Android ARM64; that is not native-driver or phone runtime qualification.

`native_ragdoll_driver.cpp` now overrides the palette after the native animation
player (`82789028`) generates it. The original animation routine still runs;
only the checked, owned family rig supplies the final mapped matrices. Guest
registers and the host floating-point state are preserved. Foreign-thread calls
disable the driver without touching native physics or bone memory. A retired
rig similarly disables its driver before traversing its old actor.

Private Linux/Vulkan checks pass for continuous Bart/Homer driving, zero failed
writes or foreign callbacks, malformed/duplicate bindings, and safe retirement.
The native collision suspension/re-add routines (`82695208`/`82695260`) remove
and restore the exact wrapper phantoms to the original world. Both actor roots
remain stationary in that check. Repeated construction/removal still exits
cleanly without logged errors. This does not establish every controller's
behavior or all-scenes animation threading.

### Opt-in family physics test

`native_ragdoll_preview.cpp` adds a bounded, manual test under **Debug Menu →
Ragdolls & knockdowns → Test player 1/2 ragdoll**. A successful action closes
the menu. On the touch gamepad, hold **RB** and tap **D-pad Left** to open the
debug menu; enable Cheats to use its test actions. Disable the old root-motion
prototype first. A living family character must be assigned to that logical
player; NPC costume previews are rejected.

The test creates the native biped, leases its native collision suspension and
drives its actual skeleton for two seconds of unpaused simulation. Character
movement and actions are suppressed only for the affected input port; pause,
back and camera look remain available. Paused menus receive their full input,
and pausing stops the test timer. It then restores animation, normal collision
membership and controls at the actor's current game-owned root. A teleport or
moving-platform transform is not replaced with an obsolete starting position.
**Restore native ragdolls** cancels it manually. No root is inferred from the
fallen pelvis, which could put an upright character below a floor. This is a
reset for a passive physics test, not an authored get-up animation.

The production adapter passes a private Linux/Vulkan run with visible jointed
Bart, rejected duplicate/invalid slots, attempted movement while active, timed
restoration, successful walking afterward, manual cancellation and eight repeated
cycles. Pausing longer than the test duration preserves the active rig and
simulation clock; menu navigation and resume still work. A native teleport
survives cancellation. Exiting through the native pause menu with two owned rigs
releases physics ownership and input suppression; after the private quick-launch
reload, another test and walking succeed. The game exits 0 without logged errors.
This covers one level transition, not the campaign. The lower-level two-actor
check separately covers Homer.

Animation readback uses the body's verified cached matrix and velocity vectors,
avoiding a native shape/inertia query for every limb. All 13 bodies match the
original native Cinfo getter across 80 live samples; continuous Homer driving
records zero failures or foreign callbacks. Windows/NoAVX2/ARM64 source checks
pass; a compile check is not phone or native Windows runtime qualification.
NPC controls, impact triggers, bind-pose anatomy, animated recovery and broader
campaign unloads remain separate qualification work.

### Native knockdowns: tried, not shipped

For 0.0.7.0 the Experimental Knockdowns mod was given native player knockdowns:
after the damage-eligibility hook (`sub_82957208`) accepted a hit on a
controlled Homer or Bart, the test's path above started the rig with a small
initial velocity (1.4 units/s backwards, 0.9 up) for 1.6 s, then restored the
player. The attempt (sources, diffs, logs and stacks) is kept privately outside
the repository; nothing of it is in the release.

It froze the game. With the AI partner on the falling body (once in normal play
as the partner walked up, and every time when the test teleported the partner
onto it), the game thread stopped answering about 0.9 s into the knockdown. One
stack sample shows it in game code, not in the host:
`sub_82691540` (level update) → `sub_82690168` → `sub_8292E7D0` →
`sub_829FB7A8` → `sub_829FB4E0` → `sub_829FB058` → `sub_82A00280` (a predicate on
a family entity's character id at +3460). `sub_829FB4E0` collects bodies near
the character through a world query (`sub_82AA4D60`) and resolves them in a
loop. Teleporting the partner onto the player with no rig, twelve times as Homer
and Bart, did not freeze. It still froze with the player's own collision left in
the world, and with the rig simulated but not driving the skeleton, so the cause
is the rig's bodies being in the world next to another character, not the
collision suspension or the written pose. A hook on the pair predicate
`sub_827A4D50` (and its virtual wrappers `sub_827A5020`/`sub_827A51E0`) was never
called during those runs, so the world filters these pairs somewhere else, and
the rig filter's layer 0 with every mask bit set is not yet known to matter.
Next: find which filter function the world uses and how the character code
treats a body that belongs to no game object. The debug-menu test above uses the
same rig and filter.

Other findings from the attempt:
- The game's own reaction to a 5-point scripted hit (type 13) moves the root
  1.10–1.25 m. With the rig it moves 1.15–1.30 m, with no further drift or
  change in height. With the stick held after that knockback the root moved
  less than 0.001 m: the input filter holds.
- The authored get-up clips cannot be played directly. Playing
  `damage_knockdown_front_ground_getup` through `spawner::PlayEntityAnimation`
  right after the restore shows for about one frame (30 fps window video), also
  after a 0.2 s settle with the actor's collision restored while the rig still
  drove the skeleton. The player controller replaces the clip; a get-up has to
  go through the controller's own knockdown state.

## Live qualification

The November 9 private Linux/Vulkan build passes 19 checks in Bartman Begins:
development quick launch, the actual mod's toggle, player/template queries,
Spooner movement/cloning/deletion, rejected editor commands, exact NPC preview,
stale-template rejection, repeated replacement and restoration without orphaned
copies. Final window captures confirm the NPC model and debug menu. Both the
smoke run and the separate visual run exit cleanly without logged errors.

The creator bridge exposes `npc_preview` with `start`, `restore` and `status`
operations. Start takes an opaque `template` handle from `world.catalog`;
status explicitly reports that NPC possession and abilities are unavailable.
`debug_menu` supports `open`, `close` and `status` for focused automation.
These are authenticated creator commands, not public Lua gameplay operations.

Windows x64, Windows without AVX2 and Android ARM64 pass source compile checks.
This does not qualify native Windows drivers, a phone APK, general NPC behavior,
campaign progression or playable skeletal ragdolls. The installed build and saves
were not used for these tests.

## Work remaining for real character ragdolls

1. Extend the verified family pose reader to qualified NPC rigs, obtain bind
   poses and retain rig identities/owners correctly across streaming. Resolve
   mixed-description transitions before treating them as supported snapshots.
2. Extend the qualified 13-body family definitions and passive limits with bind-pose anatomical axes
   and model-specific collision proportions. Qualify further rigs and existing
   controller collision handover; body/joint construction, native world
   publication, group filters and repeated ownership cleanup already work.
3. Extend the qualified continuous family bone driver with incoming controller
   velocities and impact triggers. The passive test starts from the current
   animated pose; it does not yet implement a gameplay hit-reaction controller.
4. Blend into authored get-up animations through the controller's own knockdown
   state (direct clip playback is replaced at once; see Native knockdowns above),
   restore player/NPC controllers and
   collision, and clean up before world/source unload. Preserve mission damage,
   deaths, two-player ownership, pause, checkpoints and native despawn behavior.
5. Qualify impacts, pushing, stairs/slopes, repeated recovery, world changes and
   body limits on Linux, native Windows and Android. Active balancing needs its
   own controller after the passive jointed ragdoll works.

The passive family test is opt-in. It and the older root-motion prototype must
not be described as complete gameplay ragdolls, full NPC possession, or an
Euphoria implementation.
