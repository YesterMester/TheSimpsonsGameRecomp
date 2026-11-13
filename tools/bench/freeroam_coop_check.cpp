// Package layouts captured from the original gameflow builder. The change
// must affect Springfield's mode only and always call the original AddEntry.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "../../simpsons/src/guest_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

REXCVAR_DECLARE(bool, freeroam_coop);
REX_EXTERN(sub_828A6900);

namespace {
uint32_t calls = 0;
void Require(bool value) {
  if (!value) {
    std::fputs("Springfield co-op package check failed\n", stderr);
    std::abort();
  }
}
}  // namespace

REX_EXTERN(__imp__sub_828A6900);
REX_FUNC(__imp__sub_828A6900) {
  (void)base;
  Require(ctx.r3.u32 == 0x100 && ctx.r4.u32 == 0x200 && ctx.lr == 0x82A0AE04);
  ++calls;
  ctx.r3.u64 = 0x12345678;
}

int main() {
  rex::InitLogging();
  using simpsons::LoadGuestU32;
  using simpsons::StoreGuestU32;
  struct Case {
    const char* name;
    uint32_t episode_kind, entry_kind, limit;
    bool enabled;
    uint32_t expected;
  };
  const Case cases[] = {
      {"SPR_HUB", 5, 3, 1, true, 2},        {"SPR_HUB", 5, 3, 1, false, 1},
      {"SPR_HUB", 5, 3, 2, true, 2},        {"LAND_OF_CHOCOLATE", 5, 3, 1, true, 1},
      {"BARTMAN_BEGINS", 5, 3, 1, true, 1}, {"BARTMAN_BEGINS", 5, 3, 2, true, 2},
      {"GAME_HUB", 5, 3, 2, true, 2},       {"SPR_HUB", 6, 3, 1, true, 1},
      {"SPR_HUB", 5, 2, 1, true, 1},        {"SPR_HUB_EXTRA", 5, 3, 1, true, 1},
  };
  for (const auto& test : cases) {
    std::vector<uint8_t> memory(0x4000, 0xCA);
    auto* base = memory.data();
    StoreGuestU32(base, 0x104, 0x1000);
    StoreGuestU32(base, 0x204, 0x2000);
    StoreGuestU32(base, 0x1008, test.episode_kind);
    StoreGuestU32(base, 0x100C, 0x3000);
    StoreGuestU32(base, 0x1010, uint32_t(std::strlen(test.name)) << 16);
    StoreGuestU32(base, 0x2008, test.entry_kind);
    StoreGuestU32(base, 0x2038, test.limit);
    std::strcpy(reinterpret_cast<char*>(base + 0x3000), test.name);
    auto expected = memory;
    StoreGuestU32(expected.data(), 0x2038, test.expected);
    REXCVAR_SET(freeroam_coop, test.enabled);
    PPCContext ctx{};
    ctx.r3.u64 = 0x100;
    ctx.r4.u64 = 0x200;
    ctx.lr = 0x82A0AE04;
    const auto old_calls = calls;
    sub_828A6900(ctx, base);
    Require(calls == old_calls + 1 && ctx.r3.u32 == 0x12345678);
    Require(memory == expected && LoadGuestU32(base, 0x2038) == test.expected);
  }
  std::puts("10 Springfield co-op package cases passed");
  rex::ShutdownLogging();
}
