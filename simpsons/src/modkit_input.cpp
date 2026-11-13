#include "modkit_input.h"
#include "guest_memory.h"

#include <exception>
#include <rex/ppc/context.h>

namespace {
simpsons::modkit::InputSnapshots g_states;
std::atomic<bool> g_enabled{false};
}  // namespace

namespace modkit_input {
void Enable(bool enabled) {
  g_enabled.store(false, std::memory_order_relaxed);
  g_states.Reset();
  g_enabled.store(enabled, std::memory_order_relaxed);
}
void Reset() {
  g_states.Reset();
}
void BeginFrame() {
  g_states.BeginFrame();
}
simpsons::modkit::Json State(uint32_t player) {
  return g_states.Read(player);
}

// The game's XInputGetState wrapper moves r4 (output) to r5 and clears r4
// (flags) before calling XamInputGetState. Observe its completed output; never
// ask the driver to poll again or change any guest registers or memory.
// freecam.cpp owns that hook. The scope also observes its early returns after
// native menus and the camera have filtered or synthesized game-facing input.
Observation::Observation(PPCContext& context, uint8_t* base)
    : context_(context),
      base_(base),
      epoch_(g_states.Epoch()),
      player_(context.r3.u32),
      output_(context.r4.u32),
      enabled_(g_enabled.load(std::memory_order_relaxed)),
      exceptions_(enabled_ ? std::uncaught_exceptions() : 0) {}

Observation::~Observation() {
  if (!enabled_ || !g_enabled.load(std::memory_order_relaxed) ||
      std::uncaught_exceptions() != exceptions_)
    return;
  // The original XAM entry maps an any-user query to logical player zero.
  if ((player_ & 0xFF) == 0xFF)
    player_ = 0;
  if (player_ > 1)
    return;
  const bool available = context_.r3.u32 == 0;
  if (available && !output_)
    return;  // A successful null query did not return a device sample.
  std::array<uint8_t, 16> bytes{};
  if (available)
    std::memcpy(bytes.data(), simpsons::GuestToHost(base_, output_), bytes.size());
  g_states.Observe(player_, epoch_, available, bytes);
}
}  // namespace modkit_input
