# Native shaders - taking the GPU shader translation offline

The Vulkan runtime consumes precompiled shaders on the draw path now. It looks
up the microcode hash, stage and modification in `aot_shader_path`, loads the
SPIR-V module and its binding sidecar, and uses it for the driver pipeline.
Stored pipelines preload both the native vertex stream and fallback versions.
Unseen modifications still translate at runtime.

This removes translation from the tested scenes. It does not establish that
every level, effect and cutscene has been covered, or that the GPU's remaining
memory and render target emulation has been removed.

## The shader corpus

`tools/shaderkit/xsh_inventory.py` reads the engine's collected shader storage.
The cache covers the scenes played so far. The original offline translation
test converted all 153 programs in that collection; it was a partial playthrough,
not the full game. Later collection added more programs and modifications.

The 0.0.6.4 set contains 3,297 translated modules at each of 1x and 2x, plus
32 hand-written modules for the expensive passes: 6,626 modules altogether.
It adds 5,908 modules to the previous set and keeps every earlier module path
and binding sidecar. Eight hand-written fullscreen search modules are corrected;
the other 710 earlier modules are unchanged. Every module passes SPIR-V validation. The live
title, gameplay and pause runs use the set without runtime translations in
those scenes; that does not establish full-game coverage.

`tools/shaderkit/xex_inventory.cpp` also reads shader containers from the
player's decoded executable. It validates the metadata and program ranges
before adding a program, and can merge one or more collected caches without
changing them. The checked executable has 283 supported containers; 58 match
collected programs exactly. Merging them with the collected caches gives 379
unique programs (214 vertex, 165 pixel).

Vertex declarations can patch the executable's vertex programs before the
game submits them. Those patched programs have different hashes. Collection
and first-use fallback are still needed for variants absent from the bake.

### The `.xsh` format

Little-endian, from `VulkanPipelineCache::ShaderStoredHeader`:

    file header: u32 magic 'XESH' (0x48534558), u32 byte-swapped version (0x20201219)
    repeated:    u64 XXH3 hash of the microcode
                 u32 dword count in bits 0..30, stage in bit 31 (0 vertex, 1 pixel)
                 dword_count * 4 bytes of microcode

The inventory checks each record's hash and size. Executable extraction uses
the program descriptor inside each container, excluding the literal prefix;
it does not scan arbitrary aligned words as though they were shaders.

## Bake and consume

`tools/native-renderer/build_translated_set.sh` uses the current Vulkan runtime
and its actual device configuration. It exports the modifications referenced
by stored pipelines, both vertex buffer paths, and the plain / level 0 texture
variants. With `aot_export_storage`, it also bakes common modifications of
stored programs that have no recorded pipeline. Interpolator masks come from
shader analysis; dynamically indexed register counts and other draw state
still need recorded pipelines or first-use translation.

The set records the translator version, source and device configuration, render
target path and resolution scale. The runtime serves a translation only when
those match. A set made on the Steam Deck can fall back on another Vulkan
device. A ready module still needs a driver pipeline; stored pipelines are
created before play to avoid first-draw compilation stalls.

The four hand-written pixel shaders use the translation's operation semantics,
binding sidecars and float controls. They retain the Xenos multiplication,
predicate, texture signedness and gamma rules. Changes are checked by replaying
the same frame through both versions and comparing every output pixel.

### Fullscreen search shader check

`6116B6CC7219148B` searches texture samples using a loop-addressed constant table.
The hand-written version advanced that address twice per iteration. It now
advances once, matching the original program, including the batched loop tail.

Build `tools/native-renderer/build_shader_search_check.sh`, then run its
`shader_search_check` with four paths: the generated test vertex module, the
original translated pixel module, the previous hand-written module and the
corrected hand-written module. The check needs Vulkan 1.3 and compares the
actual 32-bit float output pixels rather than a second implementation of the
search. Its 1,000 states vary loop counts, positive and negative address steps,
constants, texture contents and input coordinates.

All four modifications at both scales pass on software Vulkan and the Deck:
16,000 states altogether. The previous modules fail 15 or 29 states per run;
the correction matches the original translation in every state. Twenty-four
software and sixteen hardware full-frame replay comparisons also match.

## Build compatibility

Build the tools against the same source headers, compile definitions and runtime
library. Mixing shipped SDK headers with a newer source-built library changes
class layouts and can crash the translator. The trace tool builder reads the
SDK's `compile_commands.json` to match profiling and graphics definitions.

The shaderkit's standalone translator remains useful for inspection and
single-program tests. Release sets use the runtime bake so the specialization
keys and device configuration match what the renderer actually requests.

## Work remaining

- Capture the remaining campaign, effects and cutscenes, including patched
  vertex programs and modifications absent from the current set.
- Check uncommon primitive types, dynamically indexed register counts and
  depth / stencil modes against the fallback.
- Continue replacing the remaining GPU memory and render target paths.
- Qualify equivalent offline shader loading for D3D12; these sets are Vulkan
  SPIR-V, and do not replace the Windows DXBC translator.
