#pragma once

#include <algorithm>
#include <array>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// Primary shader reference after the original shader/declaration fixups and ring
// growth. Retain the version/fence write and the original caller's publication.
inline bool WritePrimaryShaderReference(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r30.u32, resource = ctx.r31.u32, cursor = ctx.r11.u32;
  const uint64_t variant = ctx.r19.u64;
  const uint64_t selector = (variant + 112) * 8, version = variant * 416 + 64;
  MemoryRange dev, stack, header, selected, output, code, version_field;
  if (!base || variant > UINT32_MAX || selector > UINT32_MAX - 4 || version > UINT32_MAX - 4 ||
      (device & 3) || (resource & 3) || (cursor & 3) || (ctx.r1.u32 & 15) ||
      ctx.r17.u32 != 0xC0012700 || uint64_t(resource) + version + 4 > 0x100000000ull ||
      !BackingRange(device, 22256, dev) || !BackingRange(ctx.r1.u32, 256, stack) ||
      !BackingRange(resource, std::max(uint64_t(36), selector + 4), header) ||
      !BackingRange(resource + uint32_t(version), 4, version_field) || Overlaps(dev, stack) ||
      cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 12 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, 12, output) || Overlaps(output, dev) || Overlaps(output, stack) ||
      Overlaps(output, header) || Overlaps(output, version_field) || Overlaps(version_field, dev) ||
      Overlaps(version_field, stack))
    return false;
  const uint32_t relative = LoadGuestU32(base, resource + uint32_t(selector));
  if (uint64_t(resource) + relative + 880 > 0x100000000ull ||
      !BackingRange(resource + relative, 880, selected) || Overlaps(output, selected))
    return false;
  const uint64_t source =
      uint64_t(LoadGuestU32(base, resource + 32)) + LoadGuestU32(base, resource + relative + 872);
  const uint32_t bytes = LoadGuestU32(base, resource + relative + 876);
  if (source > UINT32_MAX || (bytes & 3))
    return false;
  const uint32_t physical =
      (uint32_t(source) & 0x1FFFFFFFu) + (((uint32_t(source) >> 20) + 512) & 0x1000u);
  const rex::graphics::native_gpu_shader_reference::Call call{physical, bytes / 4};
  if (!rex::graphics::native_gpu_shader_reference::Validate(call) ||
      !BackingRange(0xC0000000u + (physical & ~3u), bytes, code) || Overlaps(output, code) ||
      Overlaps(version_field, code))
    return false;
  const uint32_t fence = LoadGuestU32(base, device + 10908);
  const std::array<uint32_t, 3> words = {rex::graphics::native_gpu_shader_reference::kHeader,
                                         call.address_type, call.words};
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  StoreGuestU32(base, resource + uint32_t(version), fence);
  ctx.r6.u64 = ctx.r31.u64 + variant * 416;
  ctx.r7.u64 = uint32_t(source) >> 20;
  ctx.r8.u64 = uint32_t(source) & 0x1FFFFFFFu;
  ctx.r9.u64 = selector;
  ctx.r10.u64 = fence;
  ctx.r11.u32 = cursor + 12;
  return true;
}

}  // namespace simpsons::native_gpu_calls
