# Native GPU measurements for 0.0.7.0 preparation

Snapshot prepared November 4, 2026 with `tools/bench/native_progress.py`.
The input JSON files are byte-identical copies of recorded diagnostic output.
They contain counters, not game assets. [Provenance](provenance.json) records
their hashes and the application/runtime identities for the E52 title sample.

## Latest paired Vulkan sample

[Full graph, counters and hashes](vulkan-title-progress.md) ·
[Machine-readable summary](vulkan-title-progress.json) ·
[Original counters](vulkan-title-e52.json)

This private Steam Deck/RADV run covers startup and the title screen at 1x,
with experimental `native_gpu_calls` enabled: 1,079 frames, 3,367,526 native draw
dispatches, zero recorded compatibility resource dependencies and zero
compatibility allocations. Eleven of twelve tracked systems were exercised.

All recorded presentation commands use the native producer. Shader commands
are 99.89% native, synchronization 99.75% and state/constants 99.55%. Buffer
submissions include four retained attempts and remain below 100%. Queries were
not exercised. Other executed commands remain retained; intentional no-ops are
excluded from the denominator. Seven startup host-refresh attempts had no
refreshed output and stay in the host-presentation denominator.

These ratios describe this recorded work. They do not measure how much of the
game's API implementation or campaign is finished. Android and native Windows
drivers are outside this sample. Re-running the script validates and summarizes
the saved counters; it does not replay the game or qualify a newer binary.

## Historical renderer fixtures

[Graph and renderer table](renderer-fixture-progress.md) ·
[Machine-readable summary](renderer-fixture-progress.json)

One 200-frame generated geometry fixture is retained per backend: Vulkan,
Direct3D 11 and Direct3D 12. Each records 564 native draw dispatches and zero
legacy draw packets. Windows APIs ran through Proton on the Deck. These older
reports do not contain the newer paired system counters, so their other
percentages remain unknown. Alternate modes with identical counter payloads
are omitted from the published table.

The fixture percentages and the newer Vulkan title percentages have different
denominators. They are kept in separate summaries. Neither summary supplies an
overall completion percentage or current-build qualification.

## Reproduce the summaries

From the repository root, choose new output paths; the script refuses to
overwrite existing evidence:

```sh
python3 tools/bench/native_progress.py \
    simpsons/re/native-progress/0.0.7.0/vulkan-title-e52.json \
    --scope 'E52 Steam Deck Vulkan startup/title, 1079 frames, 1x; recorded traffic only' \
    --relative-to . --output /tmp/0070-vulkan-progress.json \
    --markdown /tmp/0070-vulkan-progress.md

python3 tools/bench/native_progress.py \
    simpsons/re/native-progress/0.0.7.0/gpu-native.json \
    simpsons/re/native-progress/0.0.7.0/gpu-dx11.json \
    simpsons/re/native-progress/0.0.7.0/gpu-dx12.json \
    --scope 'Historical generated geometry fixtures; Windows APIs through Proton; draw counters only' \
    --relative-to . --output /tmp/0070-fixture-progress.json \
    --markdown /tmp/0070-fixture-progress.md
```

The [measurement contract](../../native_progress.md) defines each counted unit.
The [API conversion notes](../../native_gpu_calls.md) and
[renderer plan](../../native_renderer_plan.md) describe remaining implementation
and campaign/device qualification.
