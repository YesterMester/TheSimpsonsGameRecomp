#include <simpsons/modkit.hpp>

#include <cstring>

#ifndef FIXTURE_VERSION
#define FIXTURE_VERSION 1
#endif

namespace {
struct Fixture {
  SimModHost host;
  simpsons::modkit::Game game;
  int frames = 0;
  explicit Fixture(const SimModHost* source) : host(*source), game(&host) {}
};
}  // namespace
extern "C" SIM_MOD_EXPORT int32_t SimMod_Entry(const SimModHost* host, SimModPlugin* plugin) {
  try {
    if (!plugin || plugin->struct_size < sizeof(*plugin))
      return 1;
    *plugin = {sizeof(SimModPlugin),
               SIM_MOD_ABI_VERSION,
               new Fixture(host),
               [](void* context, const char* event, const char*) -> int32_t {
                 if (std::strcmp(event, "frame") == 0)
                   static_cast<Fixture*>(context)->frames += FIXTURE_VERSION;
                 return 0;
               },
               [](void* context, SimModWriter writer, void* user) -> int32_t {
                 try {
#ifdef FIXTURE_UNSAFE_SAVE
                   static_cast<Fixture*>(context)->game.Call("players.god_mode",
                                                             {{"enabled", true}});
#endif
                   const auto state = simpsons::modkit::Json{
                       {"frames", static_cast<Fixture*>(context)->frames},
                       {"version",
                        FIXTURE_VERSION}}.dump();
                   writer(user, state.data(), state.size());
                   return 0;
                 } catch (...) {
                   return 1;
                 }
               },
               [](void* context, const char* text) -> int32_t {
                 try {
                   static_cast<Fixture*>(context)->frames =
                       simpsons::modkit::Json::parse(text).at("frames").get<int>();
                   return 0;
                 } catch (...) {
                   return 1;
                 }
               },
               [](void* context) { delete static_cast<Fixture*>(context); }};
    return 0;
  } catch (...) {
    return 1;
  }
}
