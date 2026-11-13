# Recorded native GPU measurements

Recorded November 4, 2026: E52 private Steam Deck/RADV Vulkan startup and title screen, 1,079 frames at 1x with experimental native_gpu_calls enabled. Recorded command/resource traffic; campaign completion and native Windows/Android hardware are outside this sample.

These are recorded-work measurements, not development completion percentages.
Saved reports do not qualify the current build or unrecorded campaign states.

```text
Draw command dispatch     ████████████████████   100%
Geometry source selection ████████████████████   100%
Texture upload commits    ████████████████████   100%
Host resolve requests     ████████████████████   100%
Shader command dispatch   ███████████████████░   99.89%
GPU synchronization       ███████████████████░   99.75%
Buffer submissions        ███████████████████░   <100%
Presentation commands     ████████████████████   100%
State and constants       ███████████████████░   99.55%
Queries                   ····················   not exercised
Other commands            ░░░░░░░░░░░░░░░░░░░░   0.00%
Host presentation         ███████████████████░   99.35%
```

Unknown does not mean 0%. No overall percentage is calculated.
Exercised systems: 11/12.

Reported compatibility dependency events: 0; compatibility allocation events: 0.

Paired percentage = native completions / executed attempts, excluding intentional skips.
Mixed, retained and failed attempts stay in the denominator. Unclassified or
unfinished attempts prevent a percentage. Resource and command units differ.
Old reports use native_draw_call / (native_draw_call + legacy_draw_packet),
including native predicated no-ops. No ratio proves complete API conversion.

| Renderer | Counted native draws | Counted legacy draws | Native share |
|---|---:|---:|---:|
| vulkan | 3367526 | 0 | 100% |

| System | Native | Retained | Mixed | Failed | Skipped | Unclassified | Executed |
|---|---:|---:|---:|---:|---:|---:|---:|
| Draw command dispatch | 3367526 | 0 | 0 | 0 | 0 | 0 | 3367526 |
| Geometry source selection | 3365345 | 0 | 0 | 0 | 0 | 0 | 3365345 |
| Texture upload commits | 51 | 0 | 0 | 0 | 0 | 0 | 51 |
| Host resolve requests | 2157 | 0 | 0 | 0 | 0 | 0 | 2157 |
| Shader command dispatch | 10116449 | 10737 | 0 | 0 | 0 | 0 | 10127186 |
| GPU synchronization | 3444820 | 8778 | 0 | 0 | 0 | 0 | 3453598 |
| Buffer submissions | 53687 | 4 | 0 | 0 | 0 | 0 | 53691 |
| Presentation commands | 1079 | 0 | 0 | 0 | 0 | 0 | 1079 |
| State and constants | 20356284 | 92440 | 0 | 0 | 0 | 0 | 20448724 |
| Queries | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| Other commands | 0 | 1082 | 0 | 0 | 13581185 | 0 | 1082 |
| Host presentation | 1072 | 0 | 0 | 7 | 0 | 0 | 1079 |

| System | Recorded native operations | Recorded dependency events |
|---|---:|---:|
| Geometry source selection | 3365345 | 0 |
| Texture upload commits | 51 | 0 |
| Host resolve requests | 2157 | 0 |

Dependency events can overlap and differ from native operation counts.
Zero operations means unexercised, even when dependencies are also zero.

## Input evidence

- `simpsons/re/native-progress/0.0.7.0/vulkan-title-e52.json`
  SHA-256: `58a7763f6c0b5d115cfb43a02216fa9d31b9111d664ed8e4f2116234b95d10ad`
