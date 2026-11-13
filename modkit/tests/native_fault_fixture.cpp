#include <simpsons/modkit.hpp>

#include <cstring>
#include <stdexcept>

namespace {
// Keep the exception's destructor in the plugin: the host must destroy it
// before unloading this module.
class ModuleError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
  ~ModuleError() override = default;
};

struct Fixture {
  SimModHost host;
  simpsons::modkit::Game game;
  explicit Fixture(const SimModHost* source) : host(*source), game(&host) {}
};
}  // namespace

extern "C" SIM_MOD_EXPORT int32_t SimMod_Entry(const SimModHost* host, SimModPlugin* plugin) {
  if (!host || !plugin || plugin->struct_size < sizeof(*plugin))
    return 1;
  *plugin = {sizeof(SimModPlugin),
             SIM_MOD_ABI_VERSION,
             new Fixture(host),
             [](void* context, const char* event, const char*) -> int32_t {
               auto& fixture = *static_cast<Fixture*>(context);
               if (std::strcmp(event, "level.ready") == 0)
                 fixture.game.Call("players.god_mode", {{"enabled", true}});
               if (std::strcmp(event, "frame") == 0)
                 throw ModuleError("fixture event failure");
               return 0;
             },
             nullptr,
             nullptr,
             [](void* context) {
               auto* fixture = static_cast<Fixture*>(context);
               fixture->host.log(fixture->host.context, "fault-fixture-shutdown-marker");
               delete fixture;
               throw ModuleError("fixture shutdown failure");
             }};
  return 0;
}
