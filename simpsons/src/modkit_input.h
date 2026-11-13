#pragma once

#include "input_state.h"

struct PPCContext;

namespace modkit_input {
// Surround the existing game reader, including its early-return capture paths.
// This observes the final game-facing output without adding a second hook.
class Observation {
 public:
  Observation(PPCContext& context, uint8_t* base);
  ~Observation();
  Observation(const Observation&) = delete;
  Observation& operator=(const Observation&) = delete;

 private:
  PPCContext& context_;
  uint8_t* base_;
  uint64_t epoch_;
  uint32_t player_, output_;
  bool enabled_;
  int exceptions_;
};
void Enable(bool enabled);
void Reset();
void BeginFrame();
simpsons::modkit::Json State(uint32_t player);
}  // namespace modkit_input
