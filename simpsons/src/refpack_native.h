#pragma once

#include "guest_memory.h"
#include "refpack.h"

#include <bit>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include <rex/ppc/context.h>

namespace simpsons::native_refpack {

struct StreamOwner {
  uint32_t cursor = 0, source = 0, destination = 0;
  uint32_t size = 0, produced = 0;
  bool destination_known = false;
  uint64_t generation = 0;
};

// Entries originate at the actual size-header reader, not byte patterns near
// a cursor. An evicted or changed stream simply retains the original decoder.
// The game adapter serializes access, so a loader job may change threads.
class StreamOwners {
 public:
  void Begin(uint32_t cursor, uint32_t source, uint32_t size) {
    if (!cursor)
      return;
    // A fresh header at a reused source supersedes any lease still waiting
    // there, even if a cancelled reader happens to reuse both cursor values.
    for (auto& owner : owners_)
      if (owner.destination_known && owner.source == source)
        owner = {};
    for (auto& owner : owners_)
      if (owner.cursor == cursor) {
        owner = {cursor, source, 0, size, 0, false, ++generation_};
        return;
      }
    for (auto& owner : owners_)
      if (!owner.cursor) {
        owner = {cursor, source, 0, size, 0, false, ++generation_};
        return;
      }
    owners_[next_] = {cursor, source, 0, size, 0, false, ++generation_};
    next_ = (next_ + 1) % owners_.size();
  }
  StreamOwner Find(uint32_t cursor, uint32_t source, uint32_t destination) {
    for (auto& owner : owners_)
      if (owner.cursor == cursor && owner.destination_known) {
        if (owner.source == source && owner.destination == destination)
          return owner;
        owner = {};
      }
    // The asynchronous loader reads the header through a stack field, then
    // copies that cursor into its persistent reader. Adopt its unique initial
    // source once; subsequent steps must match both bound cursor positions.
    StreamOwner* initial = nullptr;
    for (auto& owner : owners_)
      if (owner.cursor && !owner.destination_known && owner.source == source) {
        if (initial)
          return {};
        initial = &owner;
      }
    if (initial) {
      initial->cursor = cursor;
      initial->destination = destination;
      initial->destination_known = true;
      return *initial;
    }
    return {};
  }
  void Forget(uint32_t cursor) {
    for (auto& owner : owners_)
      if (owner.cursor == cursor)
        owner = {};
  }
  void Clear() {
    owners_ = {};
    next_ = 0;
  }
  void Advance(const StreamOwner& previous, uint32_t source, uint32_t destination,
               uint32_t produced, bool stopped) {
    if (!previous.cursor)
      return;
    for (auto& owner : owners_) {
      if (owner.cursor != previous.cursor || owner.source != previous.source ||
          owner.generation != previous.generation)
        continue;
      if (stopped || source < previous.source || destination < produced ||
          produced > owner.size - owner.produced ||
          (owner.destination_known && uint64_t(owner.destination) + produced != destination)) {
        owner = {};
      } else {
        owner.source = source;
        owner.destination = destination;
        owner.destination_known = true;
        owner.produced += produced;
      }
      return;
    }
  }

 private:
  std::array<StreamOwner, 64> owners_{};
  size_t next_ = 0;
  uint64_t generation_ = 0;
};

// Shared with the original-routine oracle. Bounds and input/output aliasing
// are proved by the caller before this commit writes guest memory.
inline refpack::Result Commit(PPCContext& ctx, uint8_t* base, std::span<const uint8_t> packed,
                              std::span<uint8_t> output) {
  const uint64_t saved = std::byteswap(ctx.r31.u64);
  std::memcpy(GuestToHost(base, ctx.r1.u32 - 8), &saved, sizeof(saved));
  const auto decoded = refpack::Decode(packed, output);
  if (ctx.r5.u32)
    StoreGuestU32(base, ctx.r5.u32, decoded ? decoded.size : 0);
  ctx.r3.u64 = decoded ? 1 : 0;
  return decoded;
}

inline bool TryStep(PPCContext& ctx, uint8_t* base, std::span<const uint8_t> packed,
                    std::span<uint8_t> output, size_t history, uint32_t source,
                    uint32_t destination, size_t budget, std::span<uint8_t> backup,
                    refpack::StepResult& decoded) {
  if (history > output.size() || backup.size() < output.size() - history)
    return false;
  const auto changed = output.subspan(history);
  if (!changed.empty())
    std::memcpy(backup.data(), changed.data(), changed.size());
  decoded = refpack::Step<true>(packed, output, history, budget, source, destination);
  if (!decoded) {
    // Checks precede each command's writes, so produced is the exact changed
    // prefix. Restore only that prefix; the remaining mapped tail may include
    // another resource being updated by another thread.
    if (decoded.produced)
      std::memcpy(changed.data(), backup.data(), decoded.produced);
    return false;
  }
  // __savegprlr_29 spills these registers without changing r1. Its matching
  // restore reloads the low 32 bits of the saved return address.
  const uint64_t saved[] = {std::byteswap(ctx.r29.u64), std::byteswap(ctx.r30.u64),
                            std::byteswap(ctx.r31.u64)};
  std::memcpy(GuestToHost(base, ctx.r1.u32 - 32), saved, sizeof(saved));
  StoreGuestU32(base, ctx.r1.u32 - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, ctx.r3.u32, source + uint32_t(decoded.consumed));
  StoreGuestU32(base, ctx.r4.u32, destination + uint32_t(decoded.produced));
  ctx.r3.u64 = decoded.stopped ? 1 : 0;
  ctx.r12.u64 = uint32_t(ctx.lr);
  ctx.lr = ctx.r12.u64;
  return true;
}

}  // namespace simpsons::native_refpack
