# Historical D3D function inventory

The entries below were extracted from guest image data at `0x821F4274`.
The original interpretation as a complete, 73-handler D3D dispatch table is
unverified and must not be used to measure native rendering coverage.

The metadata pairing is suspect: `0x40007003` encodes the 112-instruction
length of the preceding function `sub_82461500`, and `0x40004603` matches
the preceding `sub_824616C0`'s 70 instructions. This is consistent with
mispaired unwind data. The inventory also includes debug printing
(`sub_82465C18`), shutdown (`sub_82466450`) and an `ExGetXConfigSetting`
caller (`sub_824667C0`), rather than only command writers.

Keep the addresses as research leads. Verify each function's body, callers,
buffer lifetime and patching contract before replacing it. See
[native_gpu_calls.md](native_gpu_calls.md) for the bounded conversion pilot.

The historical `pm4 ops` column was a conservative immediate-value scan,
not a decoded command stream. Matching an opcode byte does not prove a draw:
`sub_82464D98`'s `0x2200` is a register-base argument. The `meta` words and
unverified notes are retained below for comparison, not API contracts.

The current draw telemetry hooks 18 verified header emitters, including
functions outside this inventory, and excludes `sub_82464D98`. Some emitters
reserve space that another function commits later; a header match alone is
not sufficient to move execution into a host draw call.

| # | meta | handler | len | pm4 ops | notes |
|---|------|---------|-----|---------|-------|
| 0 | `0x4000BD05` | `sub_82461500` | 257 |  | state write (header composed via shared ring writer) |
| 1 | `0x40007003` | `sub_824616C0` | 159 |  | state write (header composed via shared ring writer) |
| 2 | `0x40004603` | `sub_824617D8` | 87 | EVENT_WRITE_SHD | flush/sync event |
| 3 | `0x40002305` | `sub_82461868` | 48 |  | state write (header composed via shared ring writer) |
| 4 | `0x40001304` | `sub_824618B8` | 39 |  | state write (header composed via shared ring writer) |
| 5 | `0x40000F04` | `sub_824618F8` | 131 | EVENT_WRITE_SHD | flush/sync event |
| 6 | `0x40003705` | `sub_824619D8` | 381 |  | state write (header composed via shared ring writer) |
| 7 | `0x4000A903` | `sub_82461C88` | 65 |  | state write (header composed via shared ring writer) |
| 8 | `0x40001B04` | `sub_82461CF8` | 49 |  | state write (header composed via shared ring writer) |
| 9 | `0x40001404` | `sub_82461D48` | 59 |  | state write (header composed via shared ring writer) |
| 10 | `0x40001804` | `sub_82461F28` | 132 |  | state write (header composed via shared ring writer) |
| 11 | `0x40003905` | `sub_82462090` | 144 |  | state write (header composed via shared ring writer) |
| 12 | `0x40004003` | `sub_82462190` | 62 |  | state write (header composed via shared ring writer) |
| 13 | `0x40001803` | `sub_82462288` | 61 |  | state write (header composed via shared ring writer) |
| 14 | `0x40001A05` | `sub_824623A0` | 136 |  | state write (header composed via shared ring writer) |
| 15 | `0x40003B03` | `sub_82462490` | 181 | EVENT_WRITE_SHD | flush/sync event |
| 16 | `0x40004A03` | `sub_824625B8` | 156 | EVENT_WRITE_SHD | flush/sync event |
| 17 | `0x40004005` | `sub_824626B8` | 178 | DRAW_INDX_2,SET_BIN_MASK,WAIT_REG_MEM | **DRAW** (auto-index variant) |
| 18 | `0x40005602` | `sub_82462810` | 227 | DRAW_INDX,SET_BIN_MASK,WAIT_REG_MEM | **DRAW** (DMA index buffer) -- telemetry hook target |
| 19 | `0x40006D02` | `sub_824629C8` | 236 | EVENT_WRITE_SHD | flush/sync event |
| 20 | `0x40006803` | `sub_82462B68` | 58 |  | state write (header composed via shared ring writer) |
| 21 | `0x40001803` | `sub_82462BC8` | 84 |  | state write (header composed via shared ring writer) |
| 22 | `0x40002103` | `sub_82462D50` | 224 |  | state write (header composed via shared ring writer) |
| 23 | `0x40006802` | `sub_82462EF0` | 172 |  | state write (header composed via shared ring writer) |
| 24 | `0x40005102` | `sub_82463038` | 459 |  | state write (header composed via shared ring writer) |
| 25 | `0x4000CD03` | `sub_82463370` | 170 |  | state write (header composed via shared ring writer) |
| 26 | `0x40004903` | `sub_82463498` | 471 |  | state write (header composed via shared ring writer) |
| 27 | `0x4000C503` | `sub_824637B0` | 143 |  | state write (header composed via shared ring writer) |
| 28 | `0x40003B03` | `sub_824638A0` | 78 |  | state write (header composed via shared ring writer) |
| 29 | `0x40002204` | `sub_82463978` | 206 |  | state write (header composed via shared ring writer) |
| 30 | `0x40005C05` | `sub_82463AE8` | 243 |  | state write (header composed via shared ring writer) |
| 31 | `0x40006603` | `sub_82463C80` | 193 |  | state write (header composed via shared ring writer) |
| 32 | `0x40005403` | `sub_82463DD0` | 91 |  | state write (header composed via shared ring writer) |
| 33 | `0x40002303` | `sub_82463E60` | 375 | DRAW_INDX_2,EVENT_WRITE_SHD,SET_BIN_MASK,WAIT_REG_MEM | **DRAW** (inline-index, with shader-done event + ring wait; prime suspect for tiny UI-quad storms) |
| 34 | `0x4000A503` | `sub_824640F8` | 125 | EVENT_WRITE | event write |
| 35 | `0x40003705` | `sub_824641D8` | 93 | EVENT_WRITE_ZPD | occlusion/z-pass query (begin or end) |
| 36 | `0x40002301` | `sub_82464268` | 271 | EVENT_WRITE_ZPD | occlusion/z-pass query (begin or end) |
| 37 | `0x40006B03` | `sub_824644A0` | 136 |  | state write (header composed via shared ring writer) |
| 38 | `0x40003B03` | `sub_82464590` | 178 | DRAW_INDX_2,SET_BIN_MASK,WAIT_REG_MEM | **DRAW** (auto-index variant) |
| 39 | `0x40005602` | `sub_824646E8` | 227 | DRAW_INDX,SET_BIN_MASK,WAIT_REG_MEM | **DRAW** (DMA index buffer) -- telemetry hook target |
| 40 | `0x40006D02` | `sub_824648A0` | 156 | EVENT_WRITE_SHD | flush/sync event |
| 41 | `0x40004005` | `sub_824649A0` | 181 | EVENT_WRITE_SHD | flush/sync event |
| 42 | `0x40004A03` | `sub_82464AC8` | 247 |  | state write (header composed via shared ring writer) |
| 43 | `0x40006C03` | `sub_82464C78` | 117 |  | state write (header composed via shared ring writer) |
| 44 | `0x40003104` | `sub_82464D40` | 55 |  | state write (header composed via shared ring writer) |
| 45 | `0x40001603` | `sub_82464D98` | 565 | COND_EXEC,DRAW_INDX,EVENT_WRITE,EVENT_WRITE_EXT,REG_RMW,VIZ_QUERY | Historical false positive: `0x2200` is a register base, not a draw header |
| 46 | `0x4000FB03` | `sub_82465188` | 444 | EVENT_WRITE,EVENT_WRITE_ZPD | query/event resolve |
| 47 | `0x4000BD03` | `sub_82465480` | 362 | EVENT_WRITE_EXT,EVENT_WRITE_ZPD | screen-extent + z-pass events (resolve-adjacent) |
| 48 | `0x40009503` | `sub_824656D8` | 76 |  | state write (header composed via shared ring writer) |
| 49 | `0x40002005` | `sub_82465758` | 96 |  | state write (header composed via shared ring writer) |
| 50 | `0x40002803` | `sub_824657F8` | 118 |  | state write (header composed via shared ring writer) |
| 51 | `0x40003303` | `sub_82465948` | 287 | REG_RMW | register read-modify-write |
| 52 | `0x40007403` | `sub_82465C18` | 57 |  | debug printing |
| 53 | `0x4000180A` | `sub_82465CB0` | 76 |  | state write (header composed via shared ring writer) |
| 54 | `0x4000200A` | `sub_82465D30` | 355 |  | state write (header composed via shared ring writer) |
| 55 | `0x40009A03` | `sub_82465FC0` | 72 |  | state write (header composed via shared ring writer) |
| 56 | `0x40001D03` | `sub_82466038` | 120 |  | state write (header composed via shared ring writer) |
| 57 | `0x40003303` | `sub_82466108` | 360 |  | state write (header composed via shared ring writer) |
| 58 | `0x40009F03` | `sub_82466388` | 94 |  | state write (header composed via shared ring writer) |
| 59 | `0x40002804` | `sub_82466450` | 29 |  | shutdown |
| 60 | `0x40000A03` | `sub_824667C0` | 38 |  | calls ExGetXConfigSetting |
| 61 | `0x40000F03` | `sub_82466800` | 346 |  | state write (header composed via shared ring writer) |
| 62 | `0x40009A03` | `sub_82466A68` | 331 |  | state write (header composed via shared ring writer) |
| 63 | `0x40008F03` | `sub_82466CA8` | 99 |  | state write (header composed via shared ring writer) |
| 64 | `0x40002703` | `sub_82466D48` | 55 |  | state write (header composed via shared ring writer) |
| 65 | `0x40001604` | `sub_82466DA0` | 98 |  | state write (header composed via shared ring writer) |
| 66 | `0x40002A04` | `sub_82466E48` | 70 |  | state write (header composed via shared ring writer) |
| 67 | `0x40001E04` | `sub_82466EC0` | 63 |  | state write (header composed via shared ring writer) |
| 68 | `0x40001B04` | `sub_82466F30` | 314 |  | state write (header composed via shared ring writer) |
| 69 | `0x40008705` | `sub_82467150` | 60 |  | state write (header composed via shared ring writer) |
| 70 | `0x40001804` | `sub_824671B0` | 267 |  | state write (header composed via shared ring writer) |
| 71 | `0x40006F03` | `sub_82467450` | 213 |  | state write (header composed via shared ring writer) |
| 72 | `0x40005B04` | `sub_824675C8` | 790 | EVENT_WRITE | large handler w/ event write (state flush / present-adjacent; largest in table) |
