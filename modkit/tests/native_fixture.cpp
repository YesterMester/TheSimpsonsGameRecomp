#include <simpsons/modkit.hpp>

#include <cstring>
#include <memory>

namespace {
struct Fixture {
  simpsons::modkit::Game game;
  int frames = 0;
  explicit Fixture(const SimModHost* host) : game(host) {}
};
}

extern "C" SIM_MOD_EXPORT int32_t SimMod_Entry(const SimModHost* host, SimModPlugin* plugin) {
  try {
    if (!plugin || plugin->struct_size < sizeof(*plugin)) return 1;
    auto* fixture = new Fixture(host);
    *plugin = {sizeof(SimModPlugin), SIM_MOD_ABI_VERSION, fixture,
      [](void* context, const char* event, const char*) -> int32_t {
        try {
          auto& self = *static_cast<Fixture*>(context);
          if (std::strcmp(event, "level.ready") == 0) {
            for (const auto& player : self.game.Players()) self.game.RefillHealth(player.handle);
          }
          if (std::strcmp(event, "frame") == 0) ++self.frames;
          return 0;
        } catch (...) { return 1; }
      },
      [](void* context, SimModWriter writer, void* user) -> int32_t {
        try {
          auto text = simpsons::modkit::Json{{"frames", static_cast<Fixture*>(context)->frames}}.dump();
          writer(user, text.data(), text.size());
          return 0;
        } catch (...) { return 1; }
      },
      [](void* context, const char* text) -> int32_t {
        try {
          static_cast<Fixture*>(context)->frames = simpsons::modkit::Json::parse(text).at("frames").get<int>();
          return 0;
        } catch (...) { return 1; }
      },
      [](void* context) { delete static_cast<Fixture*>(context); }
    };
    return 0;
  } catch (...) { return 1; }
}
