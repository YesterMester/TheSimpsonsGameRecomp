# Automated checkpoint rendering checks

Checkpoint starts provide a faster way to collect rendering states than playing
the campaign from the beginning. The gameflow's `NewMap` helper already calls
`MapPkg:SetCheckpoint` when given a checkpoint GUID. The test tools use that
loader and let the game create its own save files.

The initial gameflow inventory has 36 map entries: 20 standard entries across
18 episodes and 16 timed entries. Eighteen entries specify a checkpoint GUID.
This is a catalog of map entry points, not every checkpoint in the game.

## Preparing a start

Read the inventory from an extracted copy of the game:

```sh
python3 tools/bench/checkpoint_sweep.py --game-data /path/to/gamedata inventory
```

Prepare a later Medal of Homer section:

```sh
python3 tools/bench/checkpoint_sweep.py --game-data /path/to/gamedata \
    prepare MEDAL_OF_HOMER:MODE_STANDARD:3 \
    --output /path/on/same/disk/tests/moh-3
```

The output must be new, outside the repository and game data, and on the same
filesystem as the game files. Assets are hard-linked to save space. The test's
gameflow is replaced atomically, so editing it does not write through a hard
link. The original map's character overrides and extension flag are kept.
Other entries in the selected episode are omitted to start directly at the
chosen map; other episodes and the gameflow installation footer are preserved.
The six startup logos are disabled only in the test copy. `--keep-logos` keeps
them enabled. In-level scripts are still the originals.

Each output has its own `userdata` directory. Real saves, settings, cache and
installed binaries are not edited. A checkpoint found in a live run can be
selected with `--checkpoint '{GUID}'`.

## Running and discovering checkpoints

Use a matching Linux game executable and runtime build:

```sh
python3 tools/bench/checkpoint_run.py /path/on/same/disk/tests/moh-3 \
    --exe /path/to/simpsons \
    --lib-dir /absolute/path/to/runtime-libraries \
    --aot-shaders /path/to/native_shaders
```

The runner uses 30 FPS game and menu timing, an isolated window and injected
controller input. It waits for gameplay, reads the loaded checkpoint registry,
checks the current episode and explicit checkpoint target, moves the camera,
and captures a GPU frame. Success requires a clean shutdown and a complete
strict native report with zero compatibility dependencies and allocations.
An incorrect spawn, timeout, missing capture or incomplete report fails the run.
Shader and pipeline inventories are collected for later replay and shader baking.

`--startup-wait` controls the delay after the first rendered startup frames.
`--cache` shares a dedicated test cache between serial runs. `--scale 2` checks
the higher render scale; `--no-trace` omits frame capture for a diagnostic run.
Captures can take tens of megabytes each, so keep room for the sweep's output.

The campaign runner prepares and runs the standard entries serially:

```sh
python3 tools/bench/checkpoint_campaign.py \
    --game-data /path/to/gamedata \
    --exe /path/to/simpsons \
    --lib-dir /absolute/path/to/runtime-libraries \
    --aot-shaders /path/to/native_shaders \
    --output /path/on/same/disk/tests/campaign \
    --discover
```

`--episodes MEDAL_OF_HOMER LAND_OF_CHOCOLATE` limits the sweep. `--discover`
adds loaded non-default checkpoint IDs that have not already been scheduled.
Known timed-mode IDs are recorded as excluded; this runner does not change their
mode and silently treat them as standard missions. The sweep stops on its first
failure. Its report remains incomplete until every queued case succeeds.
Restart the same command with `--resume` to keep qualified results and retry
the failed case in a new directory. Resume checks the gameflow, executable,
runtime and sweep settings, and keeps the previous report and case files.
`--reuse-sweep /path/to/previous/campaign` can reuse successful cases when
starting a different selection of episodes. It checks the same build and source
fingerprints and validates each case's episode, checkpoint and native report;
failed cases are not reused and their artifacts stay in the earlier sweep.

## Save bank

Keep a successful run's `userdata` directory as a test profile. To check its
actual save-load path, copy it into another prepared run:

```sh
python3 tools/bench/checkpoint_sweep.py --game-data /path/to/gamedata \
    prepare MEDAL_OF_HOMER:MODE_STANDARD:3 \
    --profile /path/to/previous-test/userdata \
    --output /path/on/same/disk/tests/moh-3-continue
```

`--profile` copies saves rather than hard-linking them, excludes cache/logs,
and leaves the original gameflow intact. Run that output with `checkpoint_run.py`
to exercise Continue. An explicit `--checkpoint` can require the observed live
GUID. The qualified Medal of Homer profile resumes the third section with the
original gameflow and the expected active checkpoint; its binary save was not
patched or given a replacement checksum. Other profiles need the same check.

## Registry and limits

For the supported executable, the Checkpoint factory at `823BD350` creates a
180-byte object through `829539B8`. Objects register in the 16-slot array at
`82E07DB0`; its count is at `82E07DF0`. Their vtable is `8218AA60`, their GUID
starts at object offset 28, and bit 0 at offset 88 marks the default point.
The current checkpoint GUID is at `82E07D94`.

GUID dwords use guest byte order, with the GUID's little-endian field layout.
The first word is replaced by the loaded stream identity. The fallback lookup
at `82953748` compares the remaining three words, which lets a discovered ID
identify the same point after another load. The reader validates the image
marker, object types, count and current episode package before using the data.
The manager pointer is stored at `82D08BA8`; its current episode data is at
manager offset 160, with package kind 5 and its name pointer at offset 12.
It only reads memory and runs as the game's parent process for Linux inspection.

These checks establish that a target loaded and its sampled rendering stayed
native. Idling at every checkpoint does not exercise every boss phase, effect,
co-op view, cutscene or later resource sequence. Further streaming may also load
more checkpoint objects. Those states still need targeted actions and captured
frame checks. Linux captures can be replayed through the Windows renderer tools;
native Windows and Pixel driver qualification remain separate work.

All 20 original standard entries across 18 episodes/hubs have qualified starts.
The first additional Springfield ID was loaded but did not become active: the
hub kept its normal start even when position restoration was disabled in a
private diagnostic copy. That ID remains an unsuccessful test, not covered
gameplay. Further hub checkpoint selection needs separate qualification; a
mission-only discovery sweep can use `--episodes` without repeating the original
successful cases by adding `--reuse-sweep`.
