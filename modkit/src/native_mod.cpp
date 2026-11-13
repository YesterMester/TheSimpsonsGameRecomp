#include "runtime.h"

#include <cstddef>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace simpsons::modkit {
namespace {
class NativeMod final : public Mod {
 public:
  NativeMod(Runtime& runtime, Manifest manifest) : Mod(runtime, std::move(manifest)) {
    const auto found = this->manifest.binaries.find(Platform());
    if (found == this->manifest.binaries.end())
      throw Failure("unsupported_platform", this->manifest.name + " has no binary for " + Platform());
    if (!this->manifest.native_image)
      throw Failure("not_prepared", "Native code must be verified before entering the game thread");
    path_ = this->manifest.native_image->entry;
    host_ = {sizeof(SimModHost), SIM_MOD_ABI_VERSION, this, CallHost, LogHost};
  }
  ~NativeMod() override { try { Stop(); } catch (...) {} }
  void Start() override {
#if defined(_WIN32)
    library_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library_) throw Failure("load_failed", "Cannot load native mod " + manifest.name);
    auto entry = reinterpret_cast<SimModEntry>(GetProcAddress(library_, "SimMod_Entry"));
#else
    library_ = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_) {
      const char* error = dlerror();
      throw Failure("load_failed", error ? error : "Cannot load native mod");
    }
    auto entry = reinterpret_cast<SimModEntry>(dlsym(library_, "SimMod_Entry"));
#endif
    plugin_.struct_size = sizeof(plugin_);
    plugin_.abi_version = SIM_MOD_ABI_VERSION;
    if (!entry || PluginCall([&] { return entry(&host_, &plugin_); }, "entry") != 0 || plugin_.struct_size < sizeof(plugin_) ||
        plugin_.abi_version != SIM_MOD_ABI_VERSION || !plugin_.event)
      throw Failure("abi_mismatch", "Native mod entrypoint is missing or incompatible");
    descriptor_ready_ = true;
    if (manifest.save_mode != "stateless" && (!plugin_.save_state || !plugin_.load_state))
      throw Failure("save_incompatible", "Stateful native mods need both state callbacks");
    ready_ = true;
  }
  void Event(std::string_view name, const Json& data) override {
    if (!ready_) return;
    const std::string event(name), text = data.dump();
    if (PluginCall([&] { return plugin_.event(plugin_.context, event.c_str(), text.c_str()); }, "event") != 0)
      throw Failure("native_mod_error", "Native mod event failed: " + event);
  }
  Json Save() override {
    StateBuffer output;
    if (!ready_ || !plugin_.save_state ||
        PluginCall([&] { return plugin_.save_state(plugin_.context, Write, &output); }, "save") != 0 || output.failed)
      throw Failure("save_incompatible", "Native mod could not serialize its state");
    return Json::parse(output.text);
  }
  void Load(const Json& data) override {
    const auto text = data.dump();
    if (!ready_ || !plugin_.load_state ||
        PluginCall([&] { return plugin_.load_state(plugin_.context, text.c_str()); }, "load") != 0)
      throw Failure("save_incompatible", "Native mod could not restore its state");
  }
  void Stop() override {
    if (!library_) return;
    const auto library = std::exchange(library_, nullptr);
    ready_ = false;
    const auto plugin = std::exchange(plugin_, {});
    const bool descriptor = std::exchange(descriptor_ready_, false);
    std::string error;
    if (descriptor && plugin.shutdown) {
      try { plugin.shutdown(plugin.context); }
      catch (const std::exception& failure) { error = failure.what(); }
      catch (...) { error = "A native shutdown callback threw an exception"; }
    }
    // Destroy the original exception while its module is still loaded.
    // Only host-owned text may survive FreeLibrary/dlclose.
#if defined(_WIN32)
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    if (!error.empty()) throw Failure("native_mod_error", "Native shutdown: " + error);
  }

 private:
  template <typename Function>
  static int32_t PluginCall(Function function, std::string_view operation) {
    try { return function(); }
    catch (const std::exception& error) {
      throw Failure("native_mod_error", "Native " + std::string(operation) + ": " + error.what());
    }
    catch (...) { throw Failure("native_mod_error", "Native " + std::string(operation) + " callback threw an exception"); }
  }
  struct StateBuffer { std::string text; bool failed = false; };
  static void Write(void* user, const char* data, size_t size) {
    auto& output = *static_cast<StateBuffer*>(user);
    if ((!data && size) || size > 1024 * 1024 - output.text.size()) {
      output.failed = true;
      return;
    }
    try { if (size) output.text.append(data, size); }
    catch (...) { output.failed = true; }
  }
  static SimModStatus CallHost(void* user, const char* method, const char* arguments,
                               SimModWriter writer, void* reply_user) {
    if (!writer) return SIM_MOD_INVALID_ARGUMENT;
    auto& mod = *static_cast<NativeMod*>(user);
    Json result;
    SimModStatus status = SIM_MOD_OK;
    try {
      if (!mod.ready_) throw Failure("not_ready", "Wait for the initialize event before calling the game");
      if (!method || !arguments || std::strlen(arguments) > 1024 * 1024)
        throw Failure("invalid_argument", "Invalid native mod request");
      result = {{"ok", true}, {"value", mod.Call(method, Json::parse(arguments))}};
    } catch (const Failure& error) {
      result = {{"ok", false}, {"code", error.code}, {"error", error.what()}};
      status = error.code == "wrong_thread" ? SIM_MOD_WRONG_THREAD : SIM_MOD_ERROR;
    } catch (const std::exception& error) {
      result = {{"ok", false}, {"code", "error"}, {"error", error.what()}};
      status = SIM_MOD_ERROR;
    }
    const auto text = result.dump();
    writer(reply_user, text.data(), text.size());
    return status;
  }
  static void LogHost(void* user, const char* message) {
    if (message) static_cast<NativeMod*>(user)->Message(message);
  }
  SimModHost host_{};
  SimModPlugin plugin_{};
  fs::path path_;
  bool ready_ = false, descriptor_ready_ = false;
#if defined(_WIN32)
  HMODULE library_ = nullptr;
#else
  void* library_ = nullptr;
#endif
};
}  // namespace

std::unique_ptr<Mod> MakeNativeMod(Runtime& runtime, Manifest manifest) {
  return std::make_unique<NativeMod>(runtime, std::move(manifest));
}
}  // namespace simpsons::modkit
