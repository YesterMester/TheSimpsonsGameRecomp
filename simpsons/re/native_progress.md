# Measuring native rendering progress

The project does not yet have a defensible whole-game completion percentage.
An implementation count, a test pass rate and the fraction of recorded work
using native paths measure different things. Keep their denominators separate.

`tools/bench/native_progress.py` reads GPU diagnostic reports without starting
the game. It writes a block graph and optional JSON with input hashes. Identify
the fixture or scene, build and device in the scope. Saved reports describe
their original run and do not qualify a newer build.

```sh
python3 tools/bench/native_progress.py path/to/native-coverage.json \
    --scope 'Identify the scene, exact build and GPU here' \
    --output /tmp/native-progress.json --markdown /tmp/native-progress.md
```

The [0.0.7.0 snapshot](native-progress/0.0.7.0/README.md) includes the latest
recorded Vulkan title counters, separate historical renderer fixtures and
byte-identical numeric input reports. Use `--relative-to .` from the repository
root to keep published evidence paths portable. Percentages remain scoped to
their original runs after the script is rerun.

Use `native_gpu_diagnostics = true` and an absolute `native_gpu_report_path` to
record a run. Use `native_gpu_strict = false` to measure all retained routes:
strict mode stops at the first compatibility dependency. Strict validation and
independent image checks remain separate correctness requirements.

## Paired route counters

New reports include `route_coverage.version = 1`. Each of its twelve systems
records `attempts`, `native`, `retained`, `mixed`, `failed`, `skipped`,
`unclassified` and `in_flight`. The outcomes plus in-flight operations must sum
exactly to attempts. A scope records each outcome once, including early errors.
Compatibility dependencies in nested resource operations propagate to the
parent resource scope. Diagnostic readback is excluded.

| Report system | One attempted unit | Meaning of a native completion |
|---|---|---|
| `draw_dispatch` | A draw command at the common decoder | A native draw record dispatched successfully; not a count of completed GPU draws |
| `geometry` | Vertex-input source selection in the backend | Resource-local native streams selected without a reported compatibility dependency during selection |
| `textures` | A prepared texture upload committed through the shared cache | CPU/native-GPU source selected and upload completed without a compatibility dependency |
| `resolves` | A backend `IssueCopy` request | Host resolve request completed without a compatibility dependency; includes accepted clear-only or empty requests |
| `shader_commands` | A program/reference/invalidation command | A native command record dispatched successfully |
| `synchronization` | A GPU wait, coherency or completion-event command | A native synchronization record dispatched successfully |
| `submission` | An indirect command-buffer dispatch | A native retained-buffer record dispatched successfully |
| `presentation` | A serialized swap command | A typed native swap request dispatched successfully; the original XE swap format remains available when conversion is disabled or its contract is not proved |
| `state_commands` | A state/constant update command | A native update record dispatched successfully |
| `queries` | An occlusion/query command | No native query command producer is currently classified |
| `other_commands` | Remaining commands, including type-2 no-ops | Unconverted markers and other commands remain retained; type-2 no-ops are skipped |
| `host_presentation` | A backend `IssueSwap` request | Presenter refresh succeeded without a reported compatibility dependency; this does not prove the display has scanned out the image |

The percentage is native completions divided by executed attempts
(`attempts - skipped`). Mixed, retained and failed attempts stay in the
denominator. Unclassified or unfinished attempts prevent a percentage. Zero
executed attempts is **not exercised**, not 0% or 100%. A completed report with
in-flight scopes, malformed counters or inconsistent totals is rejected.

Geometry selection does not count every input fetch or qualify later draw work.
Texture commits do not count cache hits or failures in earlier batched load
preparation. Shader command conversion does not remove uncached shader
translation or every original shader setup helper. GPU synchronization and
buffer-dispatch counters do not establish conversion of every CPU wait helper,
allocator or publication callback. Host presentation and serialized swap
conversion are separate rows because a host presenter can already be native
while the original command-producing path remains.

Resolve requests need their existing `operations.resolve` counter and independent
pixel checks to establish that actual packing ran. An empty or clear-only request
cannot qualify the copy formats. Resource dependency events can overlap and
have a different granularity from operations; their old counters remain useful
diagnostics but must not be divided to invent a native percentage.

## Older evidence and aggregation

Old reports retain their original draw-counter ratio:
`native_draw_call / (native_draw_call + legacy_draw_packet)`. This includes some
native predicated no-ops and is distinct from the new common-decoder denominator.
The tool never combines the old and new denominators. Mixing report generations
leaves the affected percentage unknown. Missing route counters are not filled
from operation or dependency totals.

Inputs are explicit. Do not combine negative controls, retries and repeated
exports of one run into a representative sample. Duplicate paths, malformed
counters and unsuccessful/incomplete reports are rejected. Existing input and
output evidence cannot be overwritten. Reports from different builds, drivers,
settings or types of workload should have separate snapshots. No automatic
whole-game score or average of these different units is calculated.

## Implementation and campaign coverage

For an implementation percentage, finish a versioned inventory of required API
contracts and supported branches. Record fully converted, partially converted
and unconverted contracts separately. The historical function table is not a
complete API boundary. The current 18/18 draw-emitter milestone means each
identified emitter has at least one bounded native path, not that every branch
has been replaced.

For game coverage, use a separate manifest of required campaign, effect, co-op
and platform states. Check exact builds on native Windows drivers and the Pixel
as well as the Deck. A measured traffic ratio cannot substitute for either
inventory, and the presence of counters is not evidence that a system was
exercised. Keep operation share separate from frame time as well.

See [native_gpu_calls.md](native_gpu_calls.md) for conversion contracts and
remaining implementation work. The counter contract fixture is
`tools/bench/native_gpu_route_check.cpp`; its reports deliberately contain no
rendered frames and cannot qualify GPU rendering.
