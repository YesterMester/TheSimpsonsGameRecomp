#pragma once

#include "runtime.h"

#include <array>
#include <span>

namespace simpsons::modkit {

// Observes completed game polls. Reading a mod's snapshot must never poll a
// device: mouse deltas and assignment activity belong to the original reader.
class InputSnapshots {
 public:
  uint64_t Epoch() const { return epoch_.load(std::memory_order_relaxed); }
  void Reset();
  void Observe(uint32_t player, uint64_t epoch, bool available, std::span<const uint8_t, 16> state);
  void BeginFrame();
  Json Read(uint32_t player) const;

 private:
  struct Pad {
    uint32_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  };
  struct State {
    Pad pad;
    uint32_t pressed = 0, released = 0;
    bool sampled = false, available = false, updated = false;
  };
  mutable std::mutex mutex_;
  std::atomic<uint64_t> epoch_{1};
  uint64_t frame_ = 0;
  std::array<State, 2> pending_{}, frame_state_{};
};

}  // namespace simpsons::modkit
