#include "runtime.h"
#include "lua_cxx.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

namespace simpsons::modkit {
namespace {
constexpr int kMaximumDepth = 32;

Json ReadValue(lua_State* state, int index, int depth = 0) {
  if (depth > kMaximumDepth) throw Failure("invalid_state", "Lua values are nested too deeply or contain a cycle");
  index = lua_absindex(state, index);
  switch (lua_type(state, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
    case LUA_TNUMBER:
      if (lua_isinteger(state, index)) return lua_tointeger(state, index);
      if (!std::isfinite(lua_tonumber(state, index))) throw Failure("invalid_value", "Numbers must be finite");
      return lua_tonumber(state, index);
    case LUA_TSTRING: {
      size_t size = 0;
      const char* text = lua_tolstring(state, index, &size);
      return std::string(text, size);
    }
    case LUA_TTABLE: {
      const size_t length = lua_rawlen(state, index);
      Json value = length ? Json::array() : Json::object();
      size_t entries = 0;
      lua_pushnil(state);
      while (lua_next(state, index)) {
        if (++entries > 65536) throw Failure("resource_limit", "A Lua table is too large");
        if (length) {
          if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
              static_cast<uint64_t>(lua_tointeger(state, -2)) > length)
            throw Failure("invalid_value", "Use either an array or string-keyed object, not a mixed table");
        } else {
          if (lua_type(state, -2) != LUA_TSTRING)
            throw Failure("invalid_value", "Object keys must be strings");
          value[lua_tostring(state, -2)] = ReadValue(state, -1, depth + 1);
        }
        lua_pop(state, 1);
      }
      if (length) {
        if (entries != length) throw Failure("invalid_value", "Arrays cannot have holes");
        for (size_t i = 1; i <= length; ++i) {
          lua_rawgeti(state, index, i);
          value.push_back(ReadValue(state, -1, depth + 1));
          lua_pop(state, 1);
        }
      }
      return value;
    }
    default: throw Failure("invalid_value", "Game values and saved state cannot contain functions, threads or userdata");
  }
}

void PushValue(lua_State* state, const Json& value, int depth = 0) {
  if (depth > kMaximumDepth) throw Failure("invalid_value", "Game value is nested too deeply");
  if (value.is_null()) lua_pushnil(state);
  else if (value.is_boolean()) lua_pushboolean(state, value.get<bool>());
  else if (value.is_number_integer()) lua_pushinteger(state, value.get<lua_Integer>());
  else if (value.is_number()) lua_pushnumber(state, value.get<lua_Number>());
  else if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    lua_pushlstring(state, text.data(), text.size());
  } else if (value.is_array()) {
    lua_createtable(state, static_cast<int>(value.size()), 0);
    lua_Integer i = 1;
    for (const auto& item : value) {
      PushValue(state, item, depth + 1);
      lua_rawseti(state, -2, i++);
    }
  } else {
    lua_createtable(state, 0, static_cast<int>(value.size()));
    for (auto it = value.begin(); it != value.end(); ++it) {
      PushValue(state, it.value(), depth + 1);
      lua_setfield(state, -2, it.key().c_str());
    }
  }
}

class LuaMod final : public Mod {
 public:
  LuaMod(Runtime& runtime, Manifest manifest) : Mod(runtime, std::move(manifest)) {
    PrepareCode(this->manifest, {});
  }
  ~LuaMod() override { Stop(); }

  void Start() override {
    if (state_) return;
    state_ = lua_newstate(Allocate, this);
    if (!state_) throw Failure("memory_limit", "Cannot create this mod's Lua state");
    *static_cast<LuaMod**>(lua_getextraspace(state_)) = this;
    loading_ = true;
    try {
      Protected(Initialize, this, 2000000);
      Protected(LoadEntry, this, 2000000);
      loading_ = false;
    } catch (...) {
      loading_ = false;
      throw;
    }
  }

  void Event(std::string_view event, const Json& data) override {
    const auto found = handlers_.find(std::string(event));
    if (found == handlers_.end()) return;
    // Registering handlers from an event is disallowed, so iteration is stable.
    EventCall call{this, &found->second, &data};
    Protected(Dispatch, &call);
  }

  Json Save() override {
    if (!state_) throw Failure("not_ready", "This Lua instance has no state to save");
    Json value;
    StateCall call{this, &value};
    Protected(ReadState, &call);
    return value;
  }
  void Load(const Json& data) override {
    if (!state_) throw Failure("not_ready", "This Lua instance has no state to restore");
    StateCall call{this, const_cast<Json*>(&data)};
    Protected(WriteState, &call);
  }
  void Stop() override {
    if (state_) {
      // __gc registration is excluded below: Lua disables instruction hooks
      // inside finalizers. Keep ordinary close handlers under the budget too.
      remaining_ = 200000;
      deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
      lua_sethook(state_, Count, LUA_MASKCOUNT | LUA_MASKCALL, 1000);
      lua_close(state_);
      state_ = nullptr;
    }
    handlers_.clear();
    modules_.clear();
  }

 private:
  struct EventCall { LuaMod* self; const std::vector<int>* handlers; const Json* data; };
  struct StateCall { LuaMod* self; Json* value; };
  static LuaMod& Self(lua_State* state) { return **static_cast<LuaMod**>(lua_getextraspace(state)); }
  static void* Allocate(void* user, void* pointer, size_t old_size, size_t size) {
    auto& mod = *static_cast<LuaMod*>(user);
    if (!pointer) old_size = 0;
    if (!size) {
      std::free(pointer);
      mod.memory_ -= std::min(mod.memory_, old_size);
      return nullptr;
    }
    if (size > old_size && size - old_size > kMemoryLimit - std::min(mod.memory_, kMemoryLimit))
      return nullptr;
    void* result = std::realloc(pointer, size);
    if (result) mod.memory_ = mod.memory_ - old_size + size;
    return result;
  }
  static void Count(lua_State* state, lua_Debug*) {
    auto& mod = Self(state);
    mod.remaining_ -= 1000;
    if (mod.remaining_ <= 0 || std::chrono::steady_clock::now() > mod.deadline_)
      luaL_error(state, "Script exceeded its callback budget");
  }
  void Protected(lua_CFunction function, void* data, int instructions = 200000) {
    const int top = lua_gettop(state_);
    if (!lua_checkstack(state_, 128)) throw Failure("memory_limit", "Lua stack limit reached");
    remaining_ = instructions;
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(loading_ ? 500 : 50);
    lua_pushcfunction(state_, function);
    lua_pushlightuserdata(state_, data);
    lua_sethook(state_, Count, LUA_MASKCOUNT, 1000);
    const int status = lua_pcall(state_, 1, 0, 0);
    lua_sethook(state_, nullptr, 0, 0);
    if (status != LUA_OK) {
      const char* message = lua_tostring(state_, -1);
      const std::string error = message ? message : "Lua operation failed";
      lua_settop(state_, top);
      throw Failure("script_error", error);
    }
    lua_settop(state_, top);
  }
  template <typename Function>
  static int Binding(lua_State* state, Function function) {
    try { return function(); }
    catch (const std::exception& error) {
      lua_pushstring(state, error.what());
    }
    return lua_error(state);
  }
  static int Initialize(lua_State* state) {
    return Binding(state, [&] {
      const struct { const char* name; lua_CFunction open; } libraries[] = {
          {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
          {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
          {LUA_UTF8LIBNAME, luaopen_utf8}};
      for (const auto& library : libraries) {
        luaL_requiref(state, library.name, library.open, 1);
        lua_pop(state, 1);
      }
      // Protected calls inside a runaway script could repeatedly catch the
      // budget interrupt. Keep protected entrypoints under host ownership.
      for (const char* name : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
      }
      lua_pushcfunction(state, Print);
      lua_setglobal(state, "print");
      lua_pushcfunction(state, Require);
      lua_setglobal(state, "require");
      lua_pushcfunction(state, SetMetatable);
      lua_setglobal(state, "setmetatable");
      lua_newtable(state);
      const auto catalog = Runtime::Catalog();
      for (const auto& op : catalog.at("operations")) {
        const auto name = op.at("name").get<std::string>();
        const size_t dot = name.find('.');
        const std::string group = name.substr(0, dot), method = name.substr(dot + 1);
        lua_getfield(state, -1, group.c_str());
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          lua_newtable(state);
          lua_pushvalue(state, -1);
          lua_setfield(state, -3, group.c_str());
        }
        lua_pushlstring(state, name.data(), name.size());
        lua_pushcclosure(state, CallGame, 1);
        lua_setfield(state, -2, method.c_str());
        lua_pop(state, 1);
      }
      lua_pushcfunction(state, On);
      lua_setfield(state, -2, "on");
      lua_newtable(state);
      lua_setfield(state, -2, "state");
      lua_setglobal(state, "game");
      return 0;
    });
  }
  static int LoadEntry(lua_State* state) {
    return Binding(state, [&] {
      auto& mod = Self(state);
      const auto& source = mod.manifest.scripts->at(mod.manifest.entry);
      if (luaL_loadbufferx(state, source.data(), source.size(), mod.manifest.entry.c_str(), "t") != LUA_OK)
        return lua_error(state);
      lua_call(state, 0, 0);
      return 0;
    });
  }
  static int SetMetatable(lua_State* state) {
    return Binding(state, [&] {
      luaL_checktype(state, 1, LUA_TTABLE);
      const int type = lua_type(state, 2);
      if (type != LUA_TNIL && type != LUA_TTABLE)
        return luaL_argerror(state, 2, "nil or table expected");
      if (luaL_getmetafield(state, 1, "__metatable") != LUA_TNIL)
        return luaL_error(state, "cannot change a protected metatable");
      if (type == LUA_TTABLE) {
        lua_pushliteral(state, "__gc");
        lua_rawget(state, 2);
        const bool finalizer = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (finalizer)
          return luaL_error(state, "__gc finalizers are unavailable in bounded mod scripts");
      }
      lua_settop(state, 2);
      lua_setmetatable(state, 1);
      return 1;
    });
  }
  static int CallGame(lua_State* state) {
    return Binding(state, [&] {
      const std::string method = lua_tostring(state, lua_upvalueindex(1));
      static const auto catalog = Runtime::Catalog();
      const auto found = std::find_if(catalog.at("operations").begin(), catalog.at("operations").end(),
                                     [&](const auto& op) { return op.at("name") == method; });
      const auto& params = found->at("parameters");
      if (lua_gettop(state) > static_cast<int>(params.size()))
        throw Failure("invalid_argument", "Too many arguments for " + method);
      Json args = Json::object();
      for (size_t i = 0; i < params.size(); ++i)
        if (!lua_isnoneornil(state, static_cast<int>(i + 1)))
          args[params.at(i).at("name").get<std::string>()] = ReadValue(state, static_cast<int>(i + 1));
      PushValue(state, Self(state).Call(method, args));
      return 1;
    });
  }
  static int On(lua_State* state) {
    return Binding(state, [&] {
      auto& mod = Self(state);
      if (!mod.loading_) throw Failure("registration_closed", "Register callbacks when the mod is loaded");
      const std::string event = luaL_checkstring(state, 1);
      static const auto events = Runtime::Catalog().at("events");
      if (!events.contains(event))
        throw Failure("unsupported", "Unknown mod event: " + event);
      luaL_checktype(state, 2, LUA_TFUNCTION);
      auto& handlers = mod.handlers_[event];
      if (handlers.size() >= 64) throw Failure("resource_limit", "Too many event handlers");
      lua_pushvalue(state, 2);
      handlers.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
      return 0;
    });
  }
  static int Print(lua_State* state) {
    return Binding(state, [&] {
      std::string message;
      for (int i = 1; i <= lua_gettop(state); ++i) {
        if (i > 1) message += '\t';
        const char* text = lua_tostring(state, i);
        message += text ? text : lua_typename(state, lua_type(state, i));
      }
      Self(state).Message(message);
      return 0;
    });
  }
  static int Require(lua_State* state) {
    return Binding(state, [&] {
      auto& mod = Self(state);
      const std::string name = luaL_checkstring(state, 1);
      if (const auto found = mod.modules_.find(name); found != mod.modules_.end()) {
        if (found->second == LUA_NOREF) throw Failure("module_cycle", "Circular require: " + name);
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
        return 1;
      }
      if (!mod.loading_) throw Failure("registration_closed", "Require modules during initialization");
      std::string path = name;
      for (char& c : path) {
        if (c == '.') c = '/';
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_'))
          throw Failure("invalid_path", "Use a module name such as helpers.combat");
      }
      if (mod.modules_.size() >= 128) throw Failure("resource_limit", "Too many Lua modules");
      path += ".lua";
      RelativePath(path);
      const auto found = mod.manifest.scripts->find(path);
      if (found == mod.manifest.scripts->end())
        throw Failure("missing_module", "Lua module was not in this mod's prepared source: " + name);
      const auto& source = found->second;
      mod.modules_[name] = LUA_NOREF;
      if (luaL_loadbufferx(state, source.data(), source.size(), path.c_str(), "t") != LUA_OK)
        return lua_error(state);
      lua_call(state, 0, 1);
      lua_pushvalue(state, -1);
      mod.modules_[name] = luaL_ref(state, LUA_REGISTRYINDEX);
      return 1;
    });
  }
  static int Dispatch(lua_State* state) {
    return Binding(state, [&] {
      auto& call = *static_cast<EventCall*>(lua_touserdata(state, 1));
      for (const int handler : *call.handlers) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, handler);
        PushValue(state, *call.data);
        lua_call(state, 1, 0);
      }
      return 0;
    });
  }
  static int ReadState(lua_State* state) {
    return Binding(state, [&] {
      auto& call = *static_cast<StateCall*>(lua_touserdata(state, 1));
      lua_getglobal(state, "game");
      lua_getfield(state, -1, "state");
      *call.value = ReadValue(state, -1);
      return 0;
    });
  }
  static int WriteState(lua_State* state) {
    return Binding(state, [&] {
      auto& call = *static_cast<StateCall*>(lua_touserdata(state, 1));
      lua_getglobal(state, "game");
      PushValue(state, *call.value);
      lua_setfield(state, -2, "state");
      return 0;
    });
  }

  lua_State* state_ = nullptr;
  static constexpr size_t kMemoryLimit = 32 * 1024 * 1024;
  size_t memory_ = 0;
  int remaining_ = 0;
  bool loading_ = false;
  std::chrono::steady_clock::time_point deadline_;
  std::map<std::string, std::vector<int>> handlers_;
  std::map<std::string, int> modules_;
};
}  // namespace

std::unique_ptr<Mod> MakeLuaMod(Runtime& runtime, Manifest manifest) {
  return std::make_unique<LuaMod>(runtime, std::move(manifest));
}
}  // namespace simpsons::modkit
