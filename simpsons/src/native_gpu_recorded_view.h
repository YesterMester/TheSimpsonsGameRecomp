#pragma once

#include <array>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_coherency.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"
#include "native_gpu_recorded.h"

namespace simpsons::native_gpu_calls {

// An aggregate native call still contains every operand at its original offset.
// The CPU editors need the individual logical operations inside it. Keep their
// original iterator, with a short relative continuation in the unused high bits
// of its predicate field. Bit 0 keeps its original meaning. This state follows
// the iterator when it is copied, including when its buffer pointers relocate;
// no host map, temporary command buffer or hidden buffer lifetime is involved.
class RecordedIteratorView {
 public:
  RecordedIteratorView(uint8_t* base, uint32_t iterator, uint32_t tag, uint32_t metadata,
                       uint32_t stack)
      : base_(base), iterator_(iterator), tag_(tag), metadata_(metadata), stack_(stack) {
    native_gpu_memory::MemoryRange state;
    if (!base || (iterator & 3) || !native_gpu_memory::BackingRange(iterator, 28, state))
      return;
    const uint32_t flags = LoadGuestU32(base, iterator + 24);
    if (flags <= 1) {
      eligible_ = true;
      return;
    }
    if ((flags & ~63u) != kContinuation)
      return;
    const uint32_t offset = (flags >> 1) & 31;
    const uint32_t next = LoadGuestU32(base, iterator);
    if (!offset || next < offset * 4)
      return;
    const uint32_t start = next - offset * 4;
    if (Prepare(start)) {
      if (offset < layout_.words) {
        eligible_ = true;
        active_ = true;
      } else if (offset == layout_.words) {
        eligible_ = true;
        StoreGuestU32(base_, iterator_ + 24, flags & 1);
      }
    }
  }

  ~RecordedIteratorView() {
    if (!active_)
      return;
    const uint32_t next = LoadGuestU32(base_, iterator_);
    const uint32_t flags = LoadGuestU32(base_, iterator_ + 24) & 1;
    if (next > first_ && next <= first_ + layout_.words * 4 && !((next - first_) & 3))
      StoreGuestU32(base_, iterator_ + 24, kContinuation | (((next - first_) / 4) << 1) | flags);
  }

  RecordedIteratorView(const RecordedIteratorView&) = delete;
  RecordedIteratorView& operator=(const RecordedIteratorView&) = delete;

  // The range editor resets its iterator to the last draw it found. That
  // header may be inside an aggregate, so carry its relative origin through
  // the reset instead of treating a reserved word as a new packet header.
  static uint32_t RestartFlags(uint8_t* base, uint32_t iterator, uint32_t address) {
    using namespace native_gpu_memory;
    MemoryRange state, commands;
    if (!base || ((iterator | address) & 3) || !BackingRange(iterator, 28, state))
      return 0;
    const uint32_t flags = LoadGuestU32(base, iterator + 24);
    const uint32_t offset = (flags >> 1) & 31, next = LoadGuestU32(base, iterator);
    if ((flags & ~63u) != kContinuation || !offset || next < offset * 4 ||
        address != LoadGuestU32(base, iterator + 8))
      return 0;
    const uint32_t first = next - offset * 4;
    if (!first || address <= first || address >= next ||
        !BackingRange(first, offset * 4, commands) || Overlaps(commands, state))
      return 0;
    const uint32_t header = LoadGuestU32(base, first);
    const uint32_t words = ((header >> 16) & 0x3FFF) + 2;
    const uint32_t opcode = (header >> 8) & 127;
    const uint32_t draw_offset = opcode == 0x7D ? 22 : opcode == 0x7A ? 13 : opcode == 0x79 ? 2 : 0;
    if (!IsCompoundHeader(header) || offset > words ||
        uint64_t(first) + words * 4 > LoadGuestU32(base, iterator + 4) || !draw_offset ||
        address - first != draw_offset * 4)
      return 0;
    return kContinuation | (((address - first) / 4) << 1);
  }

  uint32_t Header(uint32_t raw, uint32_t address) {
    if (!active_ && eligible_ && IsCompoundHeader(raw) &&
        address == LoadGuestU32(base_, iterator_ + 8) && Prepare(address))
      active_ = true;
    if (active_ && address >= first_) {
      const uint32_t offset = (address - first_) / 4;
      if (!((address - first_) & 3))
        for (uint32_t i = 0; i < layout_.count; ++i)
          if (layout_.parts[i].offset == offset)
            return layout_.parts[i].header;
    }
    return RecordedLogicalHeader(raw);
  }

 private:
  struct Part {
    uint32_t offset, header;
  };
  struct Layout {
    uint32_t words, count;
    std::array<Part, 6> parts;
  };
  static constexpr uint32_t kContinuation = 0x4E560000;

  static bool IsCompoundHeader(uint32_t header) {
    return header == 0xC0167D00 || header == 0xC0197D00 || header == 0xC00E7A00 ||
           header == 0xC0107A00 || header == 0xC0037900 || header == 0xC0057900 ||
           header == 0xC00B7100 || header == 0xC0096D00;
  }

  bool Prepare(uint32_t address) {
    using namespace native_gpu_memory;
    using namespace rex::graphics;
    MemoryRange state, tag, metadata, stack, commands;
    if (!base_ || !address || ((iterator_ | address | tag_ | metadata_) & 3) || stack_ < 32 ||
        !BackingRange(iterator_, 28, state) || !BackingRange(tag_, 4, tag) ||
        !BackingRange(metadata_, 16, metadata) || !BackingRange(stack_ - 32, 32, stack) ||
        Overlaps(state, tag) || Overlaps(state, metadata) || Overlaps(state, stack) ||
        Overlaps(tag, metadata) || Overlaps(tag, stack) || Overlaps(metadata, stack) ||
        !BackingRange(address, 4, commands))
      return false;
    const uint32_t header = LoadGuestU32(base_, address);
    if (!IsCompoundHeader(header))
      return false;
    const uint32_t words = ((header >> 16) & 0x3FFF) + 2;
    const uint32_t end = LoadGuestU32(base_, iterator_ + 4);
    if ((end & 3) || uint64_t(address) + words * 4 > end ||
        !BackingRange(address, words * 4, commands) || Overlaps(commands, state) ||
        Overlaps(commands, tag) || Overlaps(commands, metadata) || Overlaps(commands, stack))
      return false;
    std::array<uint32_t, 27> data{};
    for (uint32_t i = 1; i < words; ++i)
      data[i] = LoadGuestU32(base_, address + i * 4);
    const std::span<const uint32_t> payload{data.data() + 1, words - 1};
    const uint32_t opcode = (header >> 8) & 127;
    if (opcode == rex::graphics::native_gpu_calls::kOpcode) {
      rex::graphics::native_gpu_calls::DrawCall call{};
      if (!rex::graphics::native_gpu_calls::DecodeDraw(payload, call))
        return false;
      layout_ = {words,
                 6,
                 {{{0, 0x548BA},
                   {7, 0xA31},
                   {9, 0x10A2F},
                   {12, 0xC0043C00},
                   {18, 0x25000},
                   {22, words == 27 ? 0xC0032200u : 0xC0003600u}}}};
    } else if (opcode == rex::graphics::native_gpu_calls::kVerticesDrawOpcode) {
      rex::graphics::native_gpu_calls::VerticesDrawCall call{};
      if (!rex::graphics::native_gpu_calls::DecodeVertices(payload, call))
        return false;
      layout_ = {words,
                 4,
                 {{{0, 0x2102},
                   {2, 0x548BA},
                   {9, 0x25000},
                   {13, words == 18 ? 0xC0032201u : 0xC0012201u}}}};
    } else if (opcode == rex::graphics::native_gpu_calls::kRangeDrawOpcode) {
      rex::graphics::native_gpu_calls::RangeDrawCall call{};
      if (!rex::graphics::native_gpu_calls::DecodeRange(payload, call))
        return false;
      layout_ = {words, 2, {{{0, 0x2102}, {2, words == 7 ? 0xC0032201u : 0xC0012201u}}}};
    } else if (opcode == native_gpu_coherency::kOpcode) {
      native_gpu_coherency::Call call{};
      if (!native_gpu_coherency::Decode(payload, call))
        return false;
      layout_ = {words, 4, {{{0, 0x2007}, {2, 0xA31}, {4, 0x10A2F}, {7, 0xC0043C00}}}};
    } else {
      native_gpu_sync::Coherency call{};
      if (!native_gpu_sync::Decode(payload, call))
        return false;
      layout_ = {words, 3, {{{0, 0xA31}, {2, 0x10A2F}, {5, 0xC0043C00}}}};
    }
    first_ = address;
    return true;
  }

  uint8_t* base_;
  uint32_t iterator_, tag_, metadata_, stack_, first_ = 0;
  Layout layout_{};
  bool active_ = false;
  bool eligible_ = false;
};

}  // namespace simpsons::native_gpu_calls
