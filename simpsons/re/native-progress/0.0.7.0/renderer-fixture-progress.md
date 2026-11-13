# Recorded native GPU measurements

Historical generated geometry fixtures from native-direct-ring-20261101/backends-final: one 200-frame mode per renderer on Steam Deck hardware, Windows APIs through Proton. These older reports contain paired draw counters only; other systems remain unknown. They do not qualify the current build, campaign, native Windows drivers or Android hardware.

These are recorded-work measurements, not development completion percentages.
Saved reports do not qualify the current build or unrecorded campaign states.

```text
Draw command dispatch     ████████████████████   100%
Geometry source selection ????????????????????   unknown
Texture upload commits    ????????????????????   unknown
Host resolve requests     ????????????????????   unknown
Shader command dispatch   ????????????????????   unknown
GPU synchronization       ????????????????????   unknown
Buffer submissions        ????????????????????   unknown
Presentation commands     ????????????????????   unknown
State and constants       ????????????????????   unknown
Queries                   ????????????????????   unknown
Other commands            ????????????????????   unknown
Host presentation         ????????????????????   unknown
```

Unknown does not mean 0%. No overall percentage is calculated.
Exercised systems: 1/12.

Reported compatibility dependency events: 0; compatibility allocation events: 0.

Paired percentage = native completions / executed attempts, excluding intentional skips.
Mixed, retained and failed attempts stay in the denominator. Unclassified or
unfinished attempts prevent a percentage. Resource and command units differ.
Old reports use native_draw_call / (native_draw_call + legacy_draw_packet),
including native predicated no-ops. No ratio proves complete API conversion.

| Renderer | Counted native draws | Counted legacy draws | Native share |
|---|---:|---:|---:|
| vulkan | 564 | 0 | 100% |
| d3d11 | 564 | 0 | 100% |
| d3d12 | 564 | 0 | 100% |

| System | Native | Retained | Mixed | Failed | Skipped | Unclassified | Executed |
|---|---:|---:|---:|---:|---:|---:|---:|

| System | Recorded native operations | Recorded dependency events |
|---|---:|---:|
| Geometry source selection | 1680 | 0 |
| Texture upload commits | 520 | 0 |
| Host resolve requests | 0 | 0 |

Dependency events can overlap and differ from native operation counts.
Zero operations means unexercised, even when dependencies are also zero.

## Input evidence

- `simpsons/re/native-progress/0.0.7.0/gpu-native.json`
  SHA-256: `0ea5d74952aab1fb4e691cf6998539e0e3494206fccc964d4cdc80bea218ed24`
- `simpsons/re/native-progress/0.0.7.0/gpu-dx11.json`
  SHA-256: `9d4ed1a893586c2052d2117186dffcd71f94940d6b3ead447e1e2ab79749d78c`
- `simpsons/re/native-progress/0.0.7.0/gpu-dx12.json`
  SHA-256: `301ec714759f42acec0d4e9510e5045894f2fd8f94c3f6e5089b20f87ed4b6c8`
