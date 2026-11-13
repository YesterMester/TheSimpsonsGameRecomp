/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <unordered_map>

#include <CLI/CLI.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <toml++/toml.hpp>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::cvar {

namespace {

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Recursive: FlagRegistrar chain methods re-enter; change callbacks invoked
// from SetFlagByName must not mutate the registry.
std::recursive_mutex& GetRegistryMutex() {
  static std::recursive_mutex m;
  return m;
}

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
  static std::vector<FlagEntry> registry;
  return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
  static std::unordered_map<std::string, size_t> index;
  return index;
}

// Config values of flags not registered when the config was loaded, such as
// the overlay keybinds the app registers once its window exists. They are
// applied when the flag is registered, so a saved value isn't lost at the
// next start. Guarded by the registry mutex.
std::unordered_map<std::string, std::string>& GetDeferredConfigValues() {
  static std::unordered_map<std::string, std::string> values;
  return values;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
  std::string result = "REX_";
  for (char c : name) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

// Recursively apply TOML values
void ApplyTomlTable(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key);
    } else {
      std::string value_str;
      if (value.is_boolean()) {
        value_str = value.as_boolean()->get() ? "true" : "false";
      } else if (value.is_integer()) {
        value_str = std::to_string(value.as_integer()->get());
      } else if (value.is_floating_point()) {
        value_str = std::to_string(value.as_floating_point()->get());
      } else if (value.is_string()) {
        value_str = value.as_string()->get();
      } else {
        REXLOG_WARN("Config: unsupported type for key '{}'", full_key);
        continue;
      }

      if (SetFlagByName(full_key, value_str)) {
        REXLOG_DEBUG("Config: {} = {}", full_key, value_str);
      } else {
        std::lock_guard lock(GetRegistryMutex());
        if (GetRegistryIndex().count(full_key)) {
          REXLOG_WARN("Config: invalid value for '{}'", full_key);
        } else {
          // Possibly registered later; applied then.
          GetDeferredConfigValues()[full_key] = value_str;
          REXLOG_DEBUG("Config: '{}' isn't registered (yet)", full_key);
        }
      }
    }
  }
}

// todo(tomc): move restart manager to Runtime
std::vector<std::string>& GetPendingRestartStorage() {
  static std::vector<std::string> pending;
  return pending;
}

// Callback storage for change notifications
std::unordered_map<std::string, std::vector<ChangeCallback>>& GetCallbackStorage() {
  static std::unordered_map<std::string, std::vector<ChangeCallback>> callbacks;
  return callbacks;
}

void MarkPendingRestart(std::string_view name) {
  auto& pending = GetPendingRestartStorage();
  std::string name_str(name);
  if (std::find(pending.begin(), pending.end(), name_str) == pending.end()) {
    pending.push_back(name_str);
  }
}

bool ValidateConstraints(const FlagEntry& entry, std::string_view value) {
  const auto& c = entry.constraints;

  // Range validation for numeric types
  if (c.HasRangeConstraint()) {
    double numeric_val = 0;
    if (entry.type == FlagType::String || entry.type == FlagType::Boolean) {
      // These types don't have numeric range constraints
    } else if (entry.type == FlagType::Double) {
      if (!ParseDouble(value, numeric_val))
        return false;
    } else {
      // Integer types
      int64_t int_val = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
      if (ec != std::errc())
        return false;
      numeric_val = static_cast<double>(int_val);
    }

    if (c.min.has_value() && numeric_val < *c.min) {
      REXLOG_WARN("Flag '{}': value {} below min ({})", entry.name, value, *c.min);
      return false;
    }
    if (c.max.has_value() && numeric_val > *c.max) {
      REXLOG_WARN("Flag '{}': value {} exceeds max ({})", entry.name, value, *c.max);
      return false;
    }
  }

  // Allowed values validation
  if (c.HasAllowedValues()) {
    bool found = false;
    for (const auto& allowed : c.allowed_values) {
      if (allowed == value) {
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Flag '{}': '{}' not in allowed values", entry.name, value);
      return false;
    }
  }

  // Custom validator
  if (c.custom_validator && !c.custom_validator(value)) {
    REXLOG_WARN("Flag '{}': custom validation failed for '{}'", entry.name, value);
    return false;
  }

  return true;
}

}  // namespace

//=============================================================================
// Registry
//=============================================================================

std::vector<FlagEntry>& GetRegistry() {
  return GetRegistryStorage();
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
  std::lock_guard lock(GetRegistryMutex());
  (void)GetCallbackStorage();
  (void)GetPendingRestartStorage();
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  auto it = index.find(entry.name);
  if (it != index.end()) {
    REXLOG_ERROR("cvar: duplicate registration of '{}'; second registration ignored", entry.name);
    return std::nullopt;
  }
  size_t pos = storage.size();
  index[entry.name] = pos;
  storage.push_back(std::move(entry));
  auto& deferred = GetDeferredConfigValues();
  if (auto it_value = deferred.find(storage[pos].name); it_value != deferred.end()) {
    const FlagEntry& registered = storage[pos];
    if (ValidateConstraints(registered, it_value->second) && registered.setter(it_value->second)) {
      REXLOG_DEBUG("Config: {} = {} (registered after the config was loaded)", registered.name,
                   it_value->second);
    } else {
      REXLOG_WARN("Config: invalid value for '{}'", registered.name);
    }
    deferred.erase(it_value);
  }
  return pos;
}

void UnregisterFlag(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  std::string key(name);
  auto idx_it = index.find(key);
  if (idx_it == index.end()) {
    return;
  }
  size_t pos = idx_it->second;
  index.erase(idx_it);
  storage.erase(storage.begin() + pos);
  for (auto& [n, i] : index) {
    if (i > pos) {
      --i;
    }
  }
  GetCallbackStorage().erase(key);
  auto& pending = GetPendingRestartStorage();
  pending.erase(std::remove(pending.begin(), pending.end(), key), pending.end());
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
  if (owned_name_.empty()) {
    return;
  }
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto it = index.find(owned_name_);
  if (it == index.end()) {
    return;
  }
  fn(GetRegistryStorage()[it->second]);
}

bool SetFlagByName(std::string_view name, std::string_view value) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }

  const auto& entry = GetRegistryStorage()[it->second];

  // Check lifecycle
  if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
    REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
    return false;
  }

  // Validate constraints
  if (!ValidateConstraints(entry, value)) {
    return false;
  }

  bool success = entry.setter(value);

  // Track pending restart flags
  if (success && entry.lifecycle == Lifecycle::kRequiresRestart) {
    MarkPendingRestart(name);
  }

  // Invoke registered callbacks
  if (success) {
    auto& callbacks = GetCallbackStorage();
    auto it = callbacks.find(std::string(name));
    if (it != callbacks.end()) {
      for (const auto& callback : it->second) {
        callback(name, value);
      }
    }
  }

  return success;
}

std::string GetFlagByName(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return "";
  }

  return GetRegistryStorage()[it->second].getter();
}

std::vector<std::string> ListFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  result.reserve(GetRegistryStorage().size());
  for (const auto& entry : GetRegistryStorage()) {
    result.push_back(entry.name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.lifecycle == lc) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  // Pointer is invalidated by any subsequent registry call.
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return nullptr;
  }
  return &GetRegistryStorage()[it->second];
}

template <>
bool Query<bool>(std::string_view name) {
  std::string v = GetFlagByName(name);
  return v == "true" || v == "1" || v == "yes";
}

template <>
int32_t Query<int32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
int64_t Query<int64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint32_t Query<uint32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint64_t Query<uint64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
double Query<double>(std::string_view name) {
  std::string v = GetFlagByName(name);
  double out = 0.0;
  ParseDouble(v, out);
  return out;
}

template <>
std::string Query<std::string>(std::string_view name) {
  return GetFlagByName(name);
}

std::vector<std::string> GetPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  entry.setter(entry.default_value);
}

void ResetAllToDefaults() {
  std::lock_guard lock(GetRegistryMutex());
  for (const auto& entry : GetRegistryStorage()) {
    entry.setter(entry.default_value);
  }
}

bool HasNonDefaultValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      result.push_back(entry.name);
    }
  }
  return result;
}

std::string SerializeToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      if (entry.type == FlagType::String) {
        result += entry.name + " = \"" + entry.getter() + "\"\n";
      } else {
        result += entry.name + " = " + entry.getter() + "\n";
      }
    }
  }
  return result;
}

std::string SerializeToTOML(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category && entry.getter() != entry.default_value) {
      if (entry.type == FlagType::String) {
        result += entry.name + " = \"" + entry.getter() + "\"\n";
      } else {
        result += entry.name + " = " + entry.getter() + "\n";
      }
    }
  }
  return result;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage().erase(std::string(name));
}

//=============================================================================
// Initialization
//=============================================================================

std::vector<std::string> Init(int argc, char** argv) {
  CLI::App app{"", ""};
  app.allow_extras();

  for (auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Boolean) {
      app.add_flag_function(
          "--" + entry.name + ",!--no-" + entry.name,
          [&entry](int64_t count) { entry.setter(count > 0 ? "true" : "false"); },
          entry.description);
    } else {
      app.add_option_function<std::string>(
          "--" + entry.name, [&entry](const std::string& val) { entry.setter(val); },
          entry.description);
    }
  }

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
    //             dont call rex logging funcs here for now.
    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
  }

  return app.remaining();
}

void LoadConfig(const std::filesystem::path& config_path) {
  if (!std::filesystem::exists(config_path)) {
    REXLOG_DEBUG("Config file not found: {}", config_path.string());
    return;
  }

  try {
    auto config = toml::parse_file(config_path.string());
    ApplyTomlTable(config, "");
    REXLOG_INFO("Loaded config from {}", config_path.string());
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse config {}: {}", config_path.string(), err.what());
  }
}

void ApplyEnvironment() {
  int count = 0;
  for (const auto& entry : GetRegistryStorage()) {
    std::string env_name = FlagNameToEnvVar(entry.name);
    const char* env_value = std::getenv(env_name.c_str());
    if (env_value != nullptr) {
      if (entry.setter(env_value)) {
        REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, env_value, env_name);
        ++count;
      } else {
        REXLOG_WARN("Env: failed to parse {} = {}", env_name, env_value);
      }
    }
  }

  if (count > 0) {
    REXLOG_INFO("Applied {} environment variable override(s)", count);
  }
}

void FinalizeInit() {
  std::lock_guard lock(g_mutex);
  g_finalized = true;
  REXLOG_DEBUG("cvar: initialization finalized");
}

bool IsFinalized() {
  return g_finalized;
}

void SaveConfig(const std::filesystem::path& config_path) {
  std::string content = SerializeToTOML();
  if (content.empty()) {
    REXLOG_DEBUG("SaveConfig: no modified flags to save");
    return;
  }

  try {
    std::ofstream file(config_path);
    if (!file) {
      REXLOG_ERROR("SaveConfig: failed to open {}", config_path.string());
      return;
    }
    file << "# Auto-generated cvar configuration\n";
    file << content;
    REXLOG_INFO("Saved config to {}", config_path.string());
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
  }
}

bool SaveConfigPatch(const std::filesystem::path& config_path,
                     const std::map<std::string, std::string>& values) {
  if (values.empty())
    return true;
  std::filesystem::path temporary;
  try {
    toml::table config;
    std::string original;
    if (std::filesystem::exists(config_path)) {
      std::ifstream file(config_path, std::ios::binary);
      original.assign(std::istreambuf_iterator<char>(file), {});
      config = toml::parse(original);
    }
    struct Replacement {
      size_t last_line;
      std::string text;
    };
    std::map<size_t, Replacement> replacements;
    std::string additions;
    for (const auto& [name, value] : values) {
      FlagType type;
      {
        // File access must not block live readers, including the audio callback.
        std::lock_guard lock(GetRegistryMutex());
        auto found = GetRegistryIndex().find(name);
        if (found == GetRegistryIndex().end())
          return false;
        const auto& entry = GetRegistryStorage()[found->second];
        if (!ValidateConstraints(entry, value))
          return false;
        type = entry.type;
      }
      size_t first_line = 0, last_line = 0;
      if (const auto* existing = config.get(name)) {
        if (existing->is_table() || existing->is_array())
          return false;
        first_line = existing->source().begin.line;
        last_line = existing->source().end.line;
      }
      if (type == FlagType::String) {
        config.insert_or_assign(name, value);
      } else if (type == FlagType::Boolean) {
        if (value != "true" && value != "false" && value != "0" && value != "1")
          return false;
        config.insert_or_assign(name, value == "true" || value == "1");
      } else if (type == FlagType::Double) {
        double number{};
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc() || end != value.data() + value.size() || !std::isfinite(number))
          return false;
        config.insert_or_assign(name, number);
      } else if (type != FlagType::Command) {
        int64_t number{};
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc() || end != value.data() + value.size())
          return false;
        config.insert_or_assign(name, number);
      } else {
        return false;
      }
      toml::table line;
      line.insert(name, *config.get(name));
      std::ostringstream formatted;
      formatted << line << '\n';
      if (first_line)
        replacements[first_line] = {last_line, formatted.str()};
      else
        additions += formatted.str();
    }
    temporary = config_path;
    temporary += ".settings-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      // Retain the launcher's managed-block markers and migration version,
      // as well as comments and settings this build doesn't know about.
      file << additions;
      std::istringstream input(original);
      std::string text;
      size_t line = 0, skip_until = 0;
      while (std::getline(input, text)) {
        ++line;
        if (line <= skip_until)
          continue;
        const auto replacement = replacements.find(line);
        if (replacement != replacements.end()) {
          file << replacement->second.text;
          skip_until = replacement->second.last_line;
        } else {
          file << text << '\n';
        }
      }
      file.flush();
      if (!file)
        throw std::runtime_error("Could not write settings");
    }
#if REX_PLATFORM_WIN32
    if (!MoveFileExW(temporary.c_str(), config_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      throw std::runtime_error("Could not replace settings");
    }
#else
    std::filesystem::rename(temporary, config_path);
#endif
    REXLOG_INFO("Saved {} in-game settings to {}", values.size(), config_path.string());
    return true;
  } catch (const std::exception& error) {
    std::error_code ignored;
    if (!temporary.empty())
      std::filesystem::remove(temporary, ignored);
    REXLOG_ERROR("Could not save in-game settings: {}", error.what());
    return false;
  }
}

namespace testing {

ScopedLifecycleOverride::ScopedLifecycleOverride() {
  g_lifecycle_override = true;
}

ScopedLifecycleOverride::~ScopedLifecycleOverride() {
  g_lifecycle_override = false;
}

void ResetAllForTesting() {
  ResetAllToDefaults();
  ClearPendingRestartFlags();
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
