# shaderkit - taking the GPU shaders native

The game's shaders run today by having the engine translate each Xbox 360
(Xenos) shader to SPIR-V at runtime, the first time it's needed. That's the
same kind of runtime translation the CPU side got rid of with static
recompilation. shaderkit does the shader half: it translates the game's shaders
to SPIR-V ahead of time, offline, so the runtime cost goes away.

There are three tools. Together they take you from "the game's shaders, wherever
they are" to "a folder of SPIR-V, ready to bake into the engine."

## xsh_inventory.py - find and extract the game's shaders

The engine already writes every unique shader it translates into a cache file
on disk (`<title_id>.xsh` under `cache/shaders/shareable`), keyed by a hash of
the shader's microcode. That's a ready-made list of the shaders collected while
playing. Unseen scenes and declaration-patched vertex programs can add more.

    python3 tools/shaderkit/xsh_inventory.py report
    python3 tools/shaderkit/xsh_inventory.py list
    python3 tools/shaderkit/xsh_inventory.py extract --out shaders_ucode

`report` summarizes the set (how many shaders, vertex vs pixel, sizes). `list`
prints every shader. `extract` writes each shader's raw microcode to its own
file, named `<hash>.<vs|ps>.ucode` - the input the translator takes.

It finds the cache automatically. Point it at one with `--cache` if needed.

The hashes it reports are the same ones the engine logs at pipeline-creation
time ("Creating graphics pipeline state with VS ..., PS ..."), so you can line
up a shader in the cache with where it's used in a frame.

## xex_inventory - extract programs from the executable

The executable contains compiled shader containers, including programs that
the play-session cache hasn't seen. This tool decodes the XEX through the
runtime's tool mode, validates the container and program ranges, and extracts
the original microcode. It can merge one or more caches into a separate output
storage, checking the hashes, shader types and bytes of duplicate records.

    sh tools/shaderkit/build_xex_inventory.sh
    tools/shaderkit/xex_inventory gamedata/default.xex shader_inventory \
        --cache /path/to/45410809.xsh

The output has `inventory.json`, the microcode files under `ucode/`, and a
merged `cache/shaders/shareable/45410809.xsh` for the normal bake step. Input
files and caches are read only. The inventory records each container's address,
metadata size, program offset, size, hash and shader type.

The supported containers have signature `0x102A1100` (pixel) or `0x102A1101`
(vertex). Their metadata and program descriptors use big-endian fields; the
program bytes remain in guest byte order, just as the shader cache stores them.
Vertex declarations can patch the original vertex programs before submission,
so executable extraction and gameplay collection are both needed.

## xenos_translate - translate one shader to SPIR-V

This is the offline translator. It reuses the engine's own, already-correct
shader translator (the Xenia-derived code in rexglue-sdk) rather than
reimplementing it, so its output comes from the exact same code that renders
the game today - just run ahead of time instead of during play.

    tools/shaderkit/build.sh                       # build it (see below)
    xenos_translate <hash>.vs.ucode  out.spv       # translate one shader

It reads a microcode blob (from `xsh_inventory extract`), takes the shader type
from the filename, and writes a `.spv` SPIR-V module.

## Building the translator

The translator links against the engine's runtime library, so it has to be
built against the same source the runtime was built from. Build the ReXGlue SDK
from source first (the normal project build does this), then:

    tools/shaderkit/build.sh

Why not CMake `find_package`? Linking against the prebuilt SDK's shipped
headers while the runtime `.so` came from a different revision causes a struct-
layout mismatch that crashes at runtime. `build.sh` sidesteps that by compiling
against the SDK source headers and linking the source-built runtime - one tree,
no skew. See the comments in `build.sh` for the details.

## Baking the served shader set

The runtime already consumes hash-keyed, pre-translated modules. The release
bake uses the actual Vulkan translator configuration at each render scale:

    sh tools/native-renderer/build_translated_set.sh \
        shader_inventory/cache /path/to/frame.xtr translated_shaders.tar.xz

This includes both 1x and 2x and the plain / level 0 texture variants. The
runtime checks the translator source hash and GPU configuration before serving
them. Keep the existing hand-written shaders alongside the translated set.
Captured scenes report no runtime translations. Full-game coverage, including
patched vertex programs and every pipeline modification, remains to be checked.
See `simpsons/re/native_shaders.md` for the current status.
