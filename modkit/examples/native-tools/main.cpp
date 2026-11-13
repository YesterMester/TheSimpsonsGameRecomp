#include <simpsons/modkit.hpp>

#include <cstring>

namespace {
struct Mod {
  // Keep instance data here. The host may prepare another instance while this
  // one is still running, so mutable globals would mix their state together.
  simpsons::modkit::Game game;
  explicit Mod(const SimModHost* host) : game(host) {}
  void Event(const char* event, const char* data) {
    if (std::strcmp(event, "initialize") == 0) {
      // Register pages during initialization. Gameplay changes belong in a
      // level or action callback; initialization also runs during save preflight.
      game.Call("ui.register_page", {{"page", {
          {"id", "tools"}, {"title", "Native health tools"},
          {"rows", simpsons::modkit::Json::array({{{"id", "heal"}, {"label", "Refill health"}}})}
      }}});
    } else if (std::strcmp(event, "ui.action") == 0) {
      const auto action = simpsons::modkit::Json::parse(data);
      if (action.at("page") == "tools" && action.at("row") == "heal")
        // These handles belong to this level. Query again after loads or respawns.
        for (const auto& player : game.Players()) game.RefillHealth(player.handle);
    }
  }
};
}

extern "C" SIM_MOD_EXPORT int32_t SimMod_Entry(const SimModHost* host, SimModPlugin* plugin) {
  // Do not call the game until the initialize event. Always catch exceptions
  // at C ABI callbacks; an error return disables this mod and removes its pages.
  try {
    if (!plugin || plugin->struct_size < sizeof(*plugin)) return 1;
    auto* mod = new Mod(host);
    *plugin = {sizeof(*plugin), SIM_MOD_ABI_VERSION, mod,
      [](void* context, const char* event, const char* data) -> int32_t {
        auto& mod = *static_cast<Mod*>(context);
        try { mod.Event(event, data); return 0; }
        catch (const std::exception& error) { mod.game.Log(error.what()); return 1; }
        catch (...) { return 1; }
      }, nullptr, nullptr,
      [](void* context) { delete static_cast<Mod*>(context); }};
    return 0;
  } catch (...) { return 1; }
}
