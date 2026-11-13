#pragma once

#include <algorithm>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace shader_control_detail {
using namespace native_gpu_memory;

struct Plan {
  uint32_t device = 0, initial = 0, first = 0, final = 0, header = 0, value = 0;
  uint8_t flags = 0;
  bool emit = false, write_flags = false;
  MemoryRange shader{};
};

// 8245E330: derive the depth/surface compatibility bits and preserve the ordered
// event-flag or WAIT_UNTIL write used when the pending state changes.
inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  const uint32_t device = ctx.r3.u32;
  MemoryRange dev, stack, commands;
  auto& shader = plan.shader;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 112 ||
      !BackingRange(device, 22256, dev) || !BackingRange(ctx.r1.u32 - 112, 112, stack) ||
      Overlaps(dev, stack))
    return false;
  bool shader_bit = false;
  const uint32_t resource = LoadGuestU32(base, device + 12684);
  if (resource) {
    if ((resource & 3) || !BackingRange(resource, 68, shader) || Overlaps(shader, dev) ||
        Overlaps(shader, stack))
      return false;
    const uint32_t relative = LoadGuestU32(base, resource + 64);
    if ((relative & 3) || uint64_t(resource) + relative + 52 > 0x100000000ull ||
        !BackingRange(resource, std::max(uint64_t(68), uint64_t(relative) + 52), shader) ||
        Overlaps(shader, dev) || Overlaps(shader, stack))
      return false;
    shader_bit = (LoadGuestU32(base, resource + relative + 48) >> 27) & 1;
  }
  const uint32_t surface = (LoadGuestU32(base, device + 11844) >> 20) & 4095;
  const uint32_t control = LoadGuestU32(base, device + 10548);
  const uint32_t prior = LoadGuestU32(base, device + 10560);
  const uint32_t mode = (control >> 4) & 7;
  const uint8_t shader_flags = *GuestToHost(base, device + 10942);
  plan.device = device;
  plan.first = (prior & ~1u) | (((shader_flags >> 5) & (control >> 1)) & 1);
  bool compatible = surface & 1;
  if (surface & 2) {
    const bool allowed =
        mode == 2 || (plan.first & 16 ? mode == 4 || mode == 6 : mode == 1 || mode == 3);
    compatible = !shader_bit && (plan.first & 1) && (control & 2) && allowed;
    if (control & 1) {
      compatible &= !(control & 0x000E3800u);
      if (control & 0x80)
        compatible &= !(control & 0xE3800000u);
    }
  }
  plan.final = (plan.first & ~2u) | (uint32_t(compatible) << 1);
  plan.flags = *GuestToHost(base, device + 10941);
  plan.emit = plan.write_flags = false;
  if (compatible) {
    if (plan.flags & 4) {
      plan.initial = LoadGuestU32(base, device + 48);
      if (!plan.initial || (plan.initial & 3) || plan.initial > LoadGuestU32(base, device + 56) ||
          uint64_t(plan.initial) + 8 > LoadGuestU32(base, device + 52) ||
          !BackingRange(plan.initial + 4, 8, commands) || Overlaps(commands, dev) ||
          Overlaps(commands, stack) || Overlaps(commands, shader))
        return false;
      if (LoadGuestU32(base, device + 13488)) {
        plan.header = rex::graphics::native_gpu_ports::kWaitHeader;
        plan.value = 0x20000;
      } else {
        // EVENT_WRITE(15) has no memory payload. Its existing consumer writes
        // VGT_EVENT_INITIATOR & 63; this scalar native range retains that effect.
        if (!rex::graphics::native_gpu_state::EncodeHeader(0x21F9, 1, plan.header))
          return false;
        plan.value = 15;
      }
      plan.flags &= ~4;
      plan.emit = plan.write_flags = true;
    }
  } else if ((control & 2) && (control & 4) &&
             (mode == 7 || mode == 5 ||
              (plan.final & 16 ? mode == 1 || mode == 3 : mode == 4 || mode == 6))) {
    plan.flags |= 4;
    plan.write_flags = true;
  }
  return true;
}

inline void Apply(const Plan& plan, uint8_t* base) {
  StoreGuestU32(base, plan.device + 10560, plan.first);
  StoreGuestU32(base, plan.device + 10560, plan.final);
  if (plan.write_flags)
    *GuestToHost(base, plan.device + 10941) = plan.flags;
  if (plan.emit) {
    StoreGuestU32(base, plan.initial + 4, plan.header);
    StoreGuestU32(base, plan.initial + 8, plan.value);
    StoreGuestU32(base, plan.device + 48, plan.initial + 8);
  }
}
}  // namespace shader_control_detail

inline bool PrepareShaderControl(PPCContext& ctx, uint8_t* base) {
  shader_control_detail::Plan plan;
  if (!shader_control_detail::BuildPlan(ctx, base, plan))
    return false;
  shader_control_detail::Apply(plan, base);
  ctx.r3.u64 = ctx.r4.u64 | 0x100;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
