// Compare the production observer around the original wrapper, with a
// deterministic kernel reader and final-input capture. Never poll twice.
#include "../../simpsons/src/modkit_input.h"
#include <rex/ppc/context.h>

#include <cstring>
#include <iostream>

REX_EXTERN(__imp__XamInputGetState);
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_input.h"

namespace {
int checks = 0, polls = 0;
uint32_t result = 0;
std::array<uint8_t, 16> sample{};
std::function<void()> during_poll;
bool capture = false;
bool synthesize = false;
void Require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
void Filter(PPCContext& ctx, uint8_t* base) {
  if (synthesize && ctx.r3.u32 != 0 && ctx.r5.u32) {
    std::memcpy(base + ctx.r5.u32, sample.data(), sample.size());
    ctx.r3.u64 = 0;
  }
  if (capture && ctx.r3.u32 == 0 && ctx.r5.u32)
    std::memset(base + ctx.r5.u32 + 4, 0, 12);
}
void ObservedRead(PPCContext& ctx, uint8_t* base) {
  modkit_input::Observation observation(ctx, base);
  GameInputRead(ctx, base);
  Filter(ctx, base);
  // The existing camera/native-menu hook can neutralize the returned pad.
  // The observer's scope sees the filtered output, including an early return.
  if (capture) {
    return;
  }
}
void Poll(uint32_t player, uint32_t output = 128) {
  std::array<uint8_t, 4096> original{}, hooked{};
  original.fill(0xCD);
  hooked = original;
  PPCContext expected{}, actual{};
  std::memset(&expected, 0x5A, sizeof(expected));
  expected.r1.u64 = 0x1000;
  expected.r3.u64 = player;
  expected.r4.u64 = output;
  std::memcpy(&actual, &expected, sizeof(actual));
  polls = 0;
  GameInputRead(expected, original.data());
  Filter(expected, original.data());
  Require(polls == 1, "The original wrapper polls exactly once");
  polls = 0;
  ObservedRead(actual, hooked.data());
  Require(polls == 1 && original == hooked && std::memcmp(&actual, &expected, sizeof(actual)) == 0,
          "Observation preserves every guest register and byte and calls the original once");
}
}  // namespace

REX_FUNC(__imp__XamInputGetState) {
  ++polls;
  Require(ctx.r4.u32 == 0, "The real wrapper passes the original zero flags");
  if (!result && ctx.r5.u32)
    std::memcpy(base + ctx.r5.u32, sample.data(), sample.size());
  if (during_poll)
    during_poll();
  ctx.r3.u64 = result;
}

int main() {
  try {
    modkit_input::Enable(false);
    sample[4] = 0x10;
    Poll(0);
    modkit_input::BeginFrame();
    Require(!modkit_input::State(0).at("sampled"), "Disabled observation leaves the game alone");
    modkit_input::Enable(true);
    sample = {};
    Poll(0);
    Poll(1);
    modkit_input::BeginFrame();
    sample[4] = 0x10;
    Poll(0);
    sample[4] = 0x40;
    Poll(1);
    modkit_input::BeginFrame();
    polls = 0;
    const auto first = modkit_input::State(0), second = modkit_input::State(1);
    Require(first["pressed"]["a"] && !first["buttons"]["x"] && second["pressed"]["x"] &&
                !second["buttons"]["a"] && polls == 0 && first == modkit_input::State(0),
            "Two-player snapshot reads never consume input or call a kernel reader");
    sample = {};
    Poll(0, 0);
    modkit_input::BeginFrame();
    Require(modkit_input::State(0)["buttons"]["a"] && !modkit_input::State(0).at("updated"),
            "A successful null query does not fabricate a neutral sample");
    result = 1167;
    Poll(1);
    modkit_input::BeginFrame();
    Require(!modkit_input::State(1).at("available") && modkit_input::State(1)["released"]["x"],
            "A failed read ignores its untouched output and releases that player");
    synthesize = true;
    sample[4] = 0x40;
    Poll(1);
    modkit_input::BeginFrame();
    synthesize = false;
    Require(modkit_input::State(1).at("available") && modkit_input::State(1)["buttons"]["x"],
            "A game-facing menu input synthesized after a failed poll is observed");
    result = 0;
    sample = {};
    Poll(0xFF);
    modkit_input::BeginFrame();
    Require(modkit_input::State(0)["released"]["a"],
            "Any-user queries follow the original player-zero mapping");
    sample[4] = 0x10;
    Poll(0);
    modkit_input::BeginFrame();
    capture = true;
    sample[4] = 0x80;
    Poll(0);
    modkit_input::BeginFrame();
    capture = false;
    Require(modkit_input::State(0)["released"]["a"] && !modkit_input::State(0)["pressed"]["y"] &&
                !modkit_input::State(0)["buttons"]["y"],
            "Final camera/menu filtering and early returns are reflected in the snapshot");
    sample[4] = 0x80;
    Poll(2);
    modkit_input::BeginFrame();
    Require(!modkit_input::State(0)["buttons"]["y"] && !modkit_input::State(1)["buttons"]["y"],
            "Unsupported player ports cannot alias a supported player");
    during_poll = [] { modkit_input::Reset(); };
    Poll(0);
    modkit_input::BeginFrame();
    during_poll = {};
    Require(!modkit_input::State(0).at("sampled"),
            "A reset during the original poll rejects its late result");
    Poll(0);
    modkit_input::BeginFrame();
    Require(modkit_input::State(0)["buttons"]["y"] && !modkit_input::State(0)["pressed"]["y"],
            "The next observation starts a new timeline without a phantom press");
    modkit_input::Reset();
    PPCContext interrupted{};
    interrupted.r4.u64 = 128;
    std::array<uint8_t, 4096> memory{};
    during_poll = [] { throw std::runtime_error("Interrupted kernel reader"); };
    bool interrupted_poll = false;
    try {
      ObservedRead(interrupted, memory.data());
    } catch (const std::runtime_error&) {
      interrupted_poll = true;
    }
    during_poll = {};
    modkit_input::BeginFrame();
    Require(interrupted_poll && !modkit_input::State(0).at("sampled"),
            "Unwinding a failed kernel call cannot publish incomplete input");
    modkit_input::Enable(false);
    std::cout << checks << " original-game input hook checks passed; source " << kGameInputSource
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
