// Compare the production ownership reader with the game's human-control and
// button-dispatch routines. The dispatcher records its port without polling.
#include "../../simpsons/src/player_control.h"
#include <simpsons/modkit.hpp>
#include <rex/ppc/context.h>

#include <iostream>
#include <random>

REX_EXTERN(sub_826BBC98);
namespace {
int checks = 0, dispatches = 0;
uint32_t dispatched_port = 0;
uint32_t ReferenceLoad(const uint8_t* base, uint32_t address) {
  // The only global read is the original input-dispatcher singleton. Keep
  // its synthetic object in the bounded fixture, without reserving 4 GiB.
  return address == 0x82D08DAC ? 0x1000 : simpsons::LoadGuestU32(base, address);
}
void Require(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
}  // namespace

#undef REX_LOAD_U32
#define REX_LOAD_U32(address) ReferenceLoad(base, uint32_t(address))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_player.h"

REX_FUNC(sub_826BBC98) {
  ++dispatches;
  dispatched_port = ctx.r4.u32;
  if (ctx.r3.u32 != 0x1000 || ctx.r5.u32 != 7)
    throw std::runtime_error("The original reader changed dispatcher arguments");
  ctx.r3.u64 = 1;
}

int main() {
  try {
    constexpr uint32_t actor = 0x2000;
    std::array<uint8_t, 32768> memory{};
    std::mt19937 random(0x20071030);
    const uint32_t bindings[] = {0, 1, 2, 3, 4, 5, 6, UINT32_MAX, 0x100, 0x101};
    for (const auto binding : bindings) {
      for (int i = 0; i < 32; ++i) {
        const auto human = i % 3 ? uint32_t(random()) : UINT32_MAX;
        simpsons::StoreGuestU32(memory.data(), actor + 820, human);
        simpsons::StoreGuestU32(memory.data(), actor + 2076, binding);
        // Family identity and viewport deliberately disagree with the port.
        simpsons::StoreGuestU32(memory.data(), actor + 2068, random() % 8);
        simpsons::StoreGuestU32(memory.data(), actor + 2088, random() % 3);
        simpsons::StoreGuestU32(memory.data(), actor + 2092, 2);
        const auto before = memory;
        PPCContext ctx{};
        ctx.r3.u64 = actor;
        GameHasHumanControl(ctx, memory.data());
        const bool active = ctx.r3.u32 != 0;
        ctx.r3.u64 = actor;
        ctx.r4.u64 = 7;
        dispatches = 0;
        GameReadPlayerButton(ctx, memory.data());
        Require(dispatches == 1 && dispatched_port == (binding & 0xFF),
                "Original button dispatch identifies the actual input binding");
        const int32_t expected = active && binding < 2 ? int32_t(dispatched_port) : -1;
        Require(players::ReadInputPlayer(memory.data(), actor) == expected && memory == before,
                "Ownership agrees with the original readers without writing guest memory");
        simpsons::StoreGuestU32(memory.data(), actor + 2092, 0);
        ctx.r3.u64 = actor;
        ctx.r4.u64 = 7;
        dispatches = 0;
        GameReadPlayerButton(ctx, memory.data());
        Require(dispatches == 0 && players::ReadInputPlayer(memory.data(), actor) == expected,
                "Temporarily blocked controls do not move a character to another input player");
      }
    }

    std::array<players::Player, 4> family{};
    family[3].input_player = 0;
    family[0].input_player = 1;
    players::ClearAmbiguousInputs(family);
    Require(family[3].input_player == 0 && family[0].input_player == 1 &&
                family[1].input_player == -1 && family[2].input_player == -1,
            "P1/P2 assignments do not depend on family order or AI characters");
    family[2].input_player = 0;
    players::ClearAmbiguousInputs(family);
    Require(family[2].input_player == -1 && family[3].input_player == -1 &&
                family[0].input_player == 1,
            "Ambiguous handovers reject all claimants without affecting the other player");

    // Exercise the public native helpers through their C ABI, including older
    // additive players.list replies that do not contain the new optional field.
    using namespace simpsons::modkit;
    Json actor_record = {{"id", "player:12"}, {"slot", 3}, {"character", "Lisa"},
                         {"health", 3}, {"max_health", 5}, {"input_player", 0},
                         {"position", {{"x", 1}, {"y", 2}, {"z", 3}}}};
    SimModHost host{sizeof(SimModHost), SIM_MOD_ABI_VERSION, &actor_record};
    host.call = [](void* context, const char* method, const char* arguments,
                   SimModWriter reply, void* user) -> SimModStatus {
      const auto args = Json::parse(arguments);
      const auto& record = *static_cast<const Json*>(context);
      Json value;
      if (std::string_view(method) == "players.list") value = Json::array({record});
      else if (std::string_view(method) == "players.controlled") {
        if (args.at("player") == 0) value = record;
      } else throw std::runtime_error("Unexpected native helper operation");
      const auto response = Json{{"ok", true}, {"value", value}}.dump();
      reply(user, response.data(), response.size());
      return SIM_MOD_OK;
    };
    Game game(&host);
    auto controlled = game.ControlledPlayer(0);
    Require(controlled && controlled->handle.id == "player:12" && controlled->slot == 3 &&
                controlled->input_player == 0 && controlled->position.z == 3 &&
                !game.ControlledPlayer(1),
            "Native creators get an optional character with its independent input ownership");
    actor_record["input_player"] = nullptr;
    Require(!game.Players().at(0).input_player, "AI ownership stays empty in native helpers");
    actor_record.erase("input_player");
    Require(!game.Players().at(0).input_player,
            "Existing additive players.list replies remain readable by native creators");
    std::cout << checks << " player control checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
