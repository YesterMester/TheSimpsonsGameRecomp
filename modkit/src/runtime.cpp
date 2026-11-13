#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "api_catalog.h"
#include "storage.h"
#include "scenes.h"
#include "project_documents.h"
#include "localization.h"
#include "text_table.h"
#include "models.h"
#include "meshes.h"
#include "textures.h"
#include "source.h"
#include "missions.h"

namespace simpsons::modkit {
namespace {
std::atomic<uint64_t> next_instance{0};
class DataMod final : public Mod {
 public:
  using Mod::Mod;
  void Start() override {}
  void Event(std::string_view, const Json&) override {}
  Json Save() override { return Json::object(); }
  void Load(const Json&) override {}
};
bool ShortId(const std::string& id) {
  return !id.empty() && id.size() <= 80 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
bool Type(const Json& value, std::string_view type) {
  if (type == "string" || type == "handle") return value.is_string();
  if (type == "number") return value.is_number() && std::isfinite(value.get<double>());
  if (type == "boolean") return value.is_boolean();
  if (type == "vec3") {
    if (!value.is_object()) return false;
    for (const auto* axis : {"x", "y", "z"})
      if (!value.contains(axis) || !value.at(axis).is_number() ||
          !std::isfinite(value.at(axis).get<double>()) ||
          std::abs(value.at(axis).get<double>()) > std::numeric_limits<float>::max()) return false;
    return true;
  }
  if (type == "entity transform") {
    if (!value.is_object()) return false;
    for (const auto* field : {"position", "right", "up", "forward"})
      if (!value.contains(field) || !Type(value.at(field), "vec3")) return false;
    return true;
  }
  if (type == "object") return value.is_object();
  if (type == "array") return value.is_array();
  return type == "any";
}
void ValidateArguments(const Json& operation, const Json& args) {
  if (!args.is_object()) throw Failure("invalid_argument", "Arguments must be an object");
  std::set<std::string> allowed;
  for (const auto& parameter : operation.at("parameters")) {
    const auto name = parameter.at("name").get<std::string>();
    allowed.insert(name);
    if (!args.contains(name)) {
      if (!parameter.value("optional", false))
        throw Failure("invalid_argument", "Missing argument: " + name);
      continue;
    }
    if (!Type(args.at(name), parameter.at("type").get<std::string>()))
      throw Failure("invalid_argument", "Wrong type for " + name);
  }
  for (auto it = args.begin(); it != args.end(); ++it)
    if (!allowed.contains(it.key()))
      throw Failure("invalid_argument", "Unknown argument: " + it.key());
}
void ValidateTimer(const Json& timer) {
  if (!timer.is_object() || !timer.contains("id") || !timer.at("id").is_string() ||
      !ShortId(timer.at("id").get<std::string>()) ||
      !timer.contains("seconds") || !Type(timer.at("seconds"), "number"))
    throw Failure("invalid_argument", "Timers need an id and a finite delay in seconds");
  const double seconds = timer.at("seconds").get<double>();
  const auto interval = timer.value("interval", Json(0));
  if (seconds < 0 || seconds > 86400 || !Type(interval, "number") ||
      interval.get<double>() < 0 || interval.get<double>() > 86400 ||
      (interval.get<double>() > 0 && interval.get<double>() < 0.001) ||
      timer.value("data", Json::object()).dump().size() > 65536)
    throw Failure("invalid_argument", "Timer delays are 0 to 86400 seconds; repeats need at least 0.001 seconds and data fits 64 KiB");
}
void ValidateTimers(const Json& saved) {
  if (!saved.is_array() || saved.size() > 64)
    throw Failure("save_incompatible", "A mod can save at most 64 timers");
  std::set<std::string> ids;
  for (const auto& timer : saved) {
    ValidateTimer(timer);
    if (!ids.insert(timer.at("id").get<std::string>()).second)
      throw Failure("save_incompatible", "Saved timer ids must be unique");
  }
}
}  // namespace

Runtime::Runtime(GameCall game, Log log, std::string locale, HostServices host)
    : game_(std::move(game)), host_(std::move(host)), instance_(++next_instance),
      missions_(std::make_unique<Missions>()),
      log_(std::move(log)), locale_(NormalizeLocale(locale)) {}
Runtime::~Runtime() { Stop(); }
Json Runtime::Catalog() { return Json::parse(kModkitApiCatalog); }

void Runtime::Load(const fs::path& root, const fs::path& storage,
                   const std::vector<std::string>& enabled) {
  if (started_ || !mods_.empty()) throw Failure("already_started", "Stop the mod runtime before loading");
  storage_ = storage;
  stopping_ = false;
  if (enabled.empty()) return;
  std::vector<Manifest> manifests;
  if (fs::is_directory(root))
    for (const auto& entry : fs::directory_iterator(root))
      if (entry.is_directory() && !Utf8(entry.path().filename()).starts_with('.') &&
          fs::is_regular_file(entry.path() / "mod.json"))
        try { manifests.push_back(Manifest::Read(entry.path(), false)); }
        catch (const std::exception& error) { Message(Utf8(entry.path().filename()), error.what()); }
  auto order = Resolve(std::move(manifests), enabled);
  for (auto& manifest : order) {
    const auto checked = Manifest::Read(manifest.folder);
    for (const auto* field : {"id", "version", "dependencies", "conflicts"})
      if (checked.source.value(field, Json()) != manifest.source.value(field, Json()))
        throw Failure("invalid_manifest", "Mod metadata changed during launch: " + manifest.id);
    manifest = checked;
  }
  auto localizations = std::make_shared<Localizations>(order);
  auto game_text = std::make_shared<GameTextTables>(ReadGameTextTables(order));
  for (auto& manifest : order)
    PrepareCode(manifest, storage_ / ".native-images");
  auto model_edits = std::make_shared<ModelVariants>(order);
  auto mesh_edits = std::make_shared<MeshVariants>(order);
  auto texture_edits = std::make_shared<TextureVariants>(order);
  std::vector<std::unique_ptr<Mod>> prepared;
  for (auto& m : order) {
    if (m.kind == "lua")
      prepared.push_back(MakeLuaMod(*this, std::move(m)));
    else if (m.kind == "native")
      prepared.push_back(MakeNativeMod(*this, std::move(m)));
    else
      prepared.push_back(std::make_unique<DataMod>(*this, std::move(m)));
  }
  mods_ = std::move(prepared);
  localizations_ = std::move(localizations);
  game_text_ = std::move(game_text);
  model_edits_ = std::move(model_edits);
  mesh_edits_ = std::move(mesh_edits);
  texture_edits_ = std::move(texture_edits);
  std::vector<std::string> owners;
  for (const auto& mod : mods_)
    owners.push_back(mod->manifest.id);
  storage_cache_ = std::make_shared<PersistentStorage>(storage_, owners);
}

void Runtime::CheckThread() const {
  if (!started_ || stopping_ || thread_ != std::this_thread::get_id())
    throw Failure("wrong_thread", "Game APIs are available inside game-thread mod callbacks");
}

void Runtime::Tick(bool ready, double seconds) {
  if (!started_) {
    thread_ = std::this_thread::get_id();
    started_ = true;
    preparing_ = true;
    for (auto& mod : mods_) {
      try { mod->Start(); }
      catch (const std::exception& error) {
        mod->failed = true;
        ++mod->errors;
        Message(mod->manifest.id, error.what());
      }
    }
    Dispatch("initialize", Json::object());
    preparing_ = false;
  }
  CheckThread();
  const bool simulated = ready && std::isfinite(seconds) && seconds > 0;
  if (simulated) simulation_time_ += std::min(seconds, 86400.0);
  if (level_ready_ != ready) {
    level_ready_ = ready;
    Dispatch(ready ? "level.ready" : "level.unloading", {{"epoch", epoch_}});
  }
  ++frame_;
  Dispatch("frame", {{"frame", frame_}, {"level_ready", ready}});
  if (simulated) {
    Dispatch("simulation", {{"seconds", seconds}, {"frame", frame_}});
    AdvanceTimers();
  }
  AdvanceMissions();
}

void Runtime::InvalidateWorld() {
  if (!started_) return;
  CheckThread();
  if (level_ready_) Dispatch("level.unloading", {{"epoch", epoch_}});
  level_ready_ = false;
  ++epoch_;
}

void Runtime::Dispatch(std::string_view event, const Json& data) {
  if (dispatching_) throw Failure("reentrant_event", "Nested mod event dispatch is not allowed");
  dispatching_ = true;
  struct DispatchScope { bool& flag; ~DispatchScope() { flag = false; } } scope{dispatching_};
  if (event != "frame" && event != "simulation") Trace(event, data);
  for (auto& mod : mods_) Invoke(*mod, event, data);
}

void Runtime::Invoke(Mod& mod, std::string_view event, const Json& data) {
    if (!mod.failed)
      for (const auto& [dependency, requirement] : mod.manifest.dependencies)
        if (Find(dependency).failed) {
          mod.failed = true;
          Message(mod.manifest.id, "Disabled because dependency failed: " + dependency);
          break;
        }
    if (mod.failed) {
      Retire(mod);
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    try { mod.Event(event, data); }
    catch (const std::exception& error) {
      ++mod.errors;
      mod.failed = true;
      Message(mod.manifest.id, std::string(event) + ": " + error.what());
    }
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    mod.cpu_ms += elapsed;
    mod.max_ms = std::max(mod.max_ms, elapsed);
    if (mod.failed) {
      Retire(mod);
    }
}

void Runtime::Retire(Mod& mod) {
  std::erase_if(pages_, [&](const auto& entry) { return entry.second.owner == mod.manifest.id; });
  std::erase_if(timers_, [&](const auto& entry) { return entry.first.first == mod.manifest.id; });
  if (mod.retired) return;
  mod.retired = true;
  if (mod.failed) {
    try { mod.retired_missions = missions_->Save(mod.manifest.id); }
    catch (const std::exception& error) {
      Message(mod.manifest.id, std::string("Mission recovery state unavailable: ") + error.what());
    }
  }
  if (mod.failed && mod.manifest.save_mode != "stateless") {
    try {
      mod.retired_state = Serialize(mod);
    } catch (const std::exception& error) {
      Message(mod.manifest.id, std::string("Recovery state unavailable: ") + error.what());
    }
  }
  try { mod.Stop(); }
  catch (const std::exception& error) { Message(mod.manifest.id, std::string("Mod shutdown: ") + error.what()); }
  if (host_.release) {
    try { host_.release(Owner(mod)); }
    catch (const std::exception& error) { Message(mod.manifest.id, std::string("Resource cleanup: ") + error.what()); }
  }
  missions_->Release(mod.manifest.id);
}

void Runtime::Stop() {
  // Shutdown must not call game APIs. The game thread may already be parked.
  stopping_ = true;
  for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
    Retire(**it);
  }
  if (storage_cache_ && storage_cache_.use_count() == 1) {
    storage_cache_->Flush();
    for (const auto& mod : mods_) {
      const auto status = storage_cache_->Status(mod->manifest.id);
      for (const auto& error : status.at("errors"))
        Message(mod->manifest.id, "Storage write failed: " + error.at("message").get<std::string>());
    }
  }
  mods_.clear();
  pages_.clear();
  timers_.clear();
  last_timer_ = {};
  simulation_time_ = 0;
  started_ = level_ready_ = false;
  thread_ = {};
  storage_cache_.reset();
  localizations_.reset();
  game_text_.reset();
  model_edits_.reset();
  mesh_edits_.reset();
  texture_edits_.reset();
  restored_resources_.clear();
  pending_mission_restore_.clear();
  last_mission_ = {};
}

Mod& Runtime::Find(std::string_view id) {
  const auto found = std::find_if(mods_.begin(), mods_.end(), [&](const auto& mod) {
    return mod->manifest.id == id;
  });
  if (found == mods_.end()) throw Failure("missing_mod", "Mod is not loaded: " + std::string(id));
  return **found;
}

Json Runtime::Call(Mod& mod, std::string_view method, const Json& args) {
  CheckThread();
  if (mod.failed) throw Failure("disabled_mod", "This mod was disabled after an error");
  static const auto catalog = Catalog();
  const auto found = std::find_if(catalog.at("operations").begin(), catalog.at("operations").end(),
                                 [&](const auto& op) {
                                   // Compare as strings: json == string_view is ambiguous with MSVC's
                                   // standard library once json converts implicitly.
                                   const auto& name = op.at("name");
                                   return name.is_string() &&
                                          name.template get_ref<const std::string&>() == method;
                                 });
  if (found == catalog.at("operations").end())
    throw Failure("unsupported", "Unknown game API: " + std::string(method));
  ValidateArguments(*found, args);
  if (serializing_ && (found->value("mutates", false) || method == "timers.schedule" ||
      method == "timers.cancel" || method == "ui.register_page" || method == "ui.remove_page" ||
      (method.starts_with("missions.") && method != "missions.get" && method != "missions.list")))
    throw Failure("read_only", "Save callbacks may inspect state but cannot change the game or owned services");
  if (preparing_ && found->value("mutates", false))
    throw Failure("restore_preflight", "Initialization and restore preflight cannot change gameplay or persistent storage");
  if (method == "system.try_call") {
    const std::string target = args.at("method");
    try {
      if (target == "system.try_call")
        throw Failure("invalid_argument", "system.try_call cannot wrap itself");
      // Re-enter normal dispatch so validation, ownership, serialization and
      // preflight rules apply to the target. This catches host API failures,
      // never arbitrary Lua functions or the interpreter's budget interrupt.
      return {{"ok", true}, {"value", Call(mod, target, args.at("arguments"))}};
    } catch (const Failure& error) {
      return {{"ok", false}, {"code", error.code}, {"error", error.what()}};
    }
  }
  if (method == "system.describe") return catalog;
  if (method == "mods.status") return Status();
  if (method == "missions.restore") {
    const auto pending = pending_mission_restore_.find(mod.manifest.id);
    if (!preparing_ || pending == pending_mission_restore_.end())
      throw Failure("restore_only", "missions.restore is available only during state.loaded preflight");
    auto state = args.at("state");
    // JSON arrays of length zero come back from Lua as empty tables.
    for (const auto* field : {"missions", "pending"})
      if (state.contains(field) && state.at(field).is_object() && state.at(field).empty())
        state[field] = Json::array();
    missions_->Restore(mod.manifest.id, state);
    pending_mission_restore_.erase(pending);
    return missions_->Call(mod.manifest.id, "missions.list", Json::object());
  }
  if (method.starts_with("missions.")) {
    if (!level_ready_ && (method == "missions.start" || method == "missions.restart" ||
        method == "missions.advance" || method == "missions.advance_batch" || method == "missions.checkpoint"))
      throw Failure("not_ready", "Load a level before advancing a mission");
    return missions_->Call(mod.manifest.id, method, args);
  }
  if (method == "timers.list") return Timers(mod.manifest.id);
  if (method == "timers.cancel") {
    return timers_.erase({mod.manifest.id, args.at("id").get<std::string>()}) != 0;
  }
  if (method == "timers.schedule") {
    ValidateTimer(args);
    const TimerKey key{mod.manifest.id, args.at("id").get<std::string>()};
    if (!timers_.contains(key) && (timers_.size() >= 1024 || Timers(mod.manifest.id).size() >= 64))
      throw Failure("resource_limit", "Timers are limited to 64 per mod and 1024 in total");
    timers_[key] = {simulation_time_ + args.at("seconds").get<double>(), args.value("interval", 0.0),
                    args.value("data", Json::object()), ++timer_generation_};
    return key.second;
  }
  if (method == "locale.current") return locale_;
  if (method == "locale.text")
    return localizations_->Text(mod.manifest.id, locale_, args.at("key").get<std::string>(),
                                args.value("values", Json::object()));
  if (method == "log.info") {
    mod.Message(args.at("message").get<std::string>());
    return nullptr;
  }
  if (method == "scenes.document") {
    return {{"document", ValidateScene(mod.manifest.source.value("scene", EmptyScene()))},
            {"fingerprint", mod.manifest.fingerprint}};
  }
  if (method == "projects.save_all") {
    const auto& documents = args.at("documents");
    if (!documents.is_object() || documents.empty() || documents.size() > 2)
      throw Failure("invalid_document", "Save a scene, a mission, or both project documents");
    Json checked = Json::object();
    for (const auto& [kind, value] : documents.items()) checked[kind] = ValidateProjectDocument(kind, value);
    // One storage replacement carries both documents and one source revision.
    storage_cache_->Write(mod.manifest.id, "project-drafts.json",
        {{"schema", 1}, {"fingerprint", mod.manifest.fingerprint}, {"documents", checked}});
    return checked;
  }
  if (method == "projects.document" || method == "projects.validate" || method == "projects.save") {
    const auto kind = args.at("kind").get<std::string>();
    const auto empty = EmptyProjectDocument(kind);
    if (method == "projects.document")
      return {{"document", ValidateProjectDocument(kind, mod.manifest.source.value(kind, empty))},
              {"fingerprint", mod.manifest.fingerprint}};
    const auto document = ValidateProjectDocument(kind, args.at("document"));
    if (method == "projects.save")
      storage_cache_->Write(mod.manifest.id, kind + "-draft.json",
          {{"schema", 1}, {"fingerprint", mod.manifest.fingerprint}, {kind, document}});
    return document;
  }
  if (method == "scenes.validate") return ValidateScene(args.at("document"));
  if (method == "scenes.save") {
    const auto document = ValidateScene(args.at("document"));
    storage_cache_->Write(mod.manifest.id, "scene-draft.json",
        {{"schema", 1}, {"fingerprint", mod.manifest.fingerprint}, {"scene", document}});
    return document;
  }
  if (method == "storage.read") {
    return storage_cache_->Read(mod.manifest.id, args.at("path").get<std::string>());
  }
  if (method == "storage.status") return storage_cache_->Status(mod.manifest.id);
  if (method == "storage.write") {
    storage_cache_->Write(mod.manifest.id, args.at("path").get<std::string>(), args.at("value"));
    return nullptr;
  }
  if (method == "ui.remove_page") {
    pages_.erase(mod.manifest.id + ":" + args.at("id").get<std::string>());
    return nullptr;
  }
  if (method == "ui.register_page") {
    const auto& description = args.at("page");
    MenuPage page;
    page.owner = mod.manifest.id;
    page.id = description.at("id").get<std::string>();
    page.title = description.at("title").get<std::string>();
    if (!ShortId(page.id) || page.title.empty() || page.title.size() > 160)
      throw Failure("invalid_argument", "Menu pages need an id and title");
    MenuText(page.title);
    if (!description.at("rows").is_array() || description.at("rows").size() > 128)
      throw Failure("invalid_argument", "A menu page has at most 128 rows");
    std::set<std::string> ids;
    for (const auto& item : description.at("rows")) {
      MenuRow row;
      row.id = item.at("id").get<std::string>();
      row.label = item.at("label").get<std::string>();
      row.type = item.value("type", "action");
      if (!ShortId(row.id) || !ids.insert(row.id).second || row.label.empty() || row.label.size() > 240)
        throw Failure("invalid_argument", "Menu row ids must be unique and labels must fit");
      MenuText(row.label);
      row.value = item.value("value", Json());
      if (row.type == "toggle") {
        if (!row.value.is_boolean()) throw Failure("invalid_argument", "Toggle values are boolean");
      } else if (row.type == "slider") {
        row.minimum = item.at("min").get<double>();
        row.maximum = item.at("max").get<double>();
        row.step = item.at("step").get<double>();
        if (!std::isfinite(row.minimum) || !std::isfinite(row.maximum) || !std::isfinite(row.step) ||
            row.minimum > row.maximum || row.step <= 0 || !Type(row.value, "number") ||
            row.value.get<double>() < row.minimum || row.value.get<double>() > row.maximum)
          throw Failure("invalid_argument", "Invalid slider range, step or value");
      } else if (row.type == "select") {
        row.choices = item.at("choices").get<std::vector<Json>>();
        if (row.choices.empty() || row.choices.size() > 128)
          throw Failure("invalid_argument", "A selector needs 1 to 128 choices");
        bool selected = false;
        for (const auto& choice : row.choices) {
          if (!choice.at("label").is_string() || !choice.contains("value"))
            throw Failure("invalid_argument", "Each choice needs a label and value");
          MenuText(choice.at("label").get<std::string>());
          selected |= choice.at("value") == row.value;
        }
        if (!selected) throw Failure("invalid_argument", "Selector value is absent from its choices");
      } else if (row.type != "action" && row.type != "text")
        throw Failure("unsupported", "Unsupported menu widget: " + row.type);
      page.rows.push_back(std::move(row));
    }
    const std::string id = page.owner + ":" + page.id;
    if (!pages_.contains(id) && pages_.size() >= 128)
      throw Failure("resource_limit", "Too many mod menu pages");
    pages_[id] = std::move(page);
    return id;
  }
  return host_.call ? host_.call(Owner(mod), method, args) : game_(method, args);
}

void Runtime::Activate(std::string_view page_id, std::string_view row_id, const Json& value,
                       uint32_t player) {
  CheckThread();
  if (player > 1) throw Failure("invalid_argument", "Menu player must be 0 or 1");
  const auto page = pages_.find(std::string(page_id));
  if (page == pages_.end()) throw Failure("stale_menu", "This mod menu has closed");
  const auto row = std::find_if(page->second.rows.begin(), page->second.rows.end(),
                               [&](const auto& r) { return r.id == row_id; });
  if (row == page->second.rows.end()) throw Failure("stale_menu", "This mod menu row has gone");
  if (row->type == "text") return;
  if (row->type == "toggle" && !value.is_boolean()) throw Failure("invalid_argument", "Expected a toggle value");
  if (row->type == "slider" && (!Type(value, "number") || value.get<double>() < row->minimum ||
                                value.get<double>() > row->maximum))
    throw Failure("invalid_argument", "Slider value is outside its range");
  if (row->type == "select" && std::none_of(row->choices.begin(), row->choices.end(),
                                             [&](const auto& c) { return c.at("value") == value; }))
    throw Failure("invalid_argument", "Unknown selector value");
  const auto owner = page->second.owner;
  const auto local_page = page->second.id;
  const auto local_row = row->id;
  // Event handlers may rebuild their page; do not keep iterators across them.
  row->value = value;
  auto& mod = Find(owner);
  const Json event = {{"page", local_page}, {"row", local_row}, {"value", value}, {"player", player}};
  Trace("ui.action", {{"owner", owner}, {"data", event}});
  Invoke(mod, "ui.action", event);
}

Json Runtime::Status() const {
  Json result = Json::array();
  for (const auto& mod : mods_)
    result.push_back({{"id", mod->manifest.id}, {"name", mod->manifest.name},
                      {"version", mod->manifest.version}, {"author", mod->manifest.author},
                      {"kind", mod->manifest.kind}, {"failed", mod->failed},
                      {"fingerprint", mod->manifest.fingerprint}, {"calls", mod->calls},
                      {"errors", mod->errors}, {"cpu_ms", mod->cpu_ms}, {"max_ms", mod->max_ms}});
  return result;
}

bool Runtime::SupportsExact() const {
  return std::all_of(mods_.begin(), mods_.end(), [](const auto& mod) {
    return !mod->failed && mod->manifest.save_mode != "checkpoint";
  });
}

Json Runtime::Save(bool exact) {
  return SaveInternal(exact, false);
}

Json Runtime::SaveInternal(bool exact, bool recover) {
  if (!active()) return {{"api", 1}, {"mods", Json::array()}};
  CheckThread();
  Json mods = Json::array();
  for (auto& mod : mods_) {
    if ((mod->failed && (!recover || (mod->manifest.save_mode != "stateless" && !mod->retired_state))) ||
        (exact && mod->manifest.save_mode == "checkpoint"))
      throw Failure("save_incompatible", mod->manifest.name + " does not support this save");
    auto state = mod->manifest.save_mode == "stateless" ? Json::object() :
                 mod->failed ? *mod->retired_state : Serialize(*mod);
    if (!state.is_object() || state.dump().size() > 1024 * 1024)
      throw Failure("save_incompatible", "Mod state must be an object no larger than 1 MiB");
    auto resources = host_.save ? host_.save(Owner(*mod)) : Json::object();
    if (!resources.is_object() || resources.dump().size() > 65536)
      throw Failure("save_incompatible", "Owned host resources must fit a 64 KiB object");
    if (mod->failed && recover && !mod->retired_missions)
      throw Failure("save_incompatible", "Mission recovery state is unavailable: " + mod->manifest.name);
    auto mission_state = mod->failed ? *mod->retired_missions : missions_->Save(mod->manifest.id);
    mods.push_back({{"id", mod->manifest.id}, {"version", mod->manifest.version},
                    {"fingerprint", mod->manifest.fingerprint}, {"state", std::move(state)},
                    {"timers", Timers(mod->manifest.id)}, {"host", std::move(resources)},
                    {"missions", std::move(mission_state)}});
  }
  Json result = {{"api", 1}, {"kind", exact ? "exact" : "checkpoint"},
                 {"frame", frame_}, {"mods", std::move(mods)}};
  if (result.dump().size() > 16 * 1024 * 1024)
    throw Failure("resource_limit", "Combined saved mod state is limited to 16 MiB");
  return result;
}

Json Runtime::Serialize(Mod& mod) {
  const bool previous = serializing_;
  serializing_ = true;
  struct SaveScope {
    bool& flag; bool previous;
    ~SaveScope() { flag = previous; }
  } scope{serializing_, previous};
  auto state = mod.manifest.save_mode == "stateless" ? Json::object() : mod.Save();
  if (!state.is_object() || state.dump().size() > 1024 * 1024)
    throw Failure("resource_limit", "Mod state must fit a 1 MiB object");
  return state;
}

void Runtime::ValidateSave(const Json& state) const {
  if (!state.is_object() || state.value("api", 0) != 1 || !state.contains("mods") ||
      !state.at("mods").is_array() || state.at("mods").size() != mods_.size())
    throw Failure("save_incompatible", "The save's mod profile does not match this game");
  std::vector<Json> resources;
  for (size_t i = 0; i < mods_.size(); ++i) {
    const auto& saved = state.at("mods").at(i);
    const auto& mod = *mods_[i];
    if (mod.failed || saved.at("id") != mod.manifest.id || saved.at("version") != mod.manifest.version ||
        saved.at("fingerprint") != mod.manifest.fingerprint || !saved.at("state").is_object() ||
        saved.at("state").dump().size() > 1024 * 1024)
      throw Failure("save_incompatible", "Mod state does not match: " + mod.manifest.name);
    ValidateTimers(saved.value("timers", Json::array()));
    Missions::ValidateSaved(saved.value("missions", Missions::Empty()));
    auto owned = saved.value("host", Json::object());
    if (!owned.is_object() || owned.dump().size() > 65536)
      throw Failure("save_incompatible", "Invalid owned host resource state");
    resources.push_back(std::move(owned));
  }
  if (host_.validate) host_.validate(resources);
}

std::unique_ptr<Runtime> Runtime::PrepareRestore(const Json& state, bool checkpoint) {
  ValidateSave(state);
  if (!checkpoint && (!SupportsExact() || state.value("kind", "exact") != "exact"))
    throw Failure("save_incompatible", "Checkpoint mod state cannot restore an exact snapshot");
  return PrepareReplacement(&state, checkpoint);
}

std::unique_ptr<Runtime> Runtime::PrepareRestore(const Json& state, bool checkpoint,
                                              const PreparedSources& sources) {
  ValidateSave(state);
  if (!checkpoint && (!SupportsExact() || state.value("kind", "exact") != "exact"))
    throw Failure("save_incompatible", "Checkpoint mod state cannot restore an exact snapshot");
  return PrepareReplacement(&state, checkpoint, &sources);
}

std::unique_ptr<Runtime> Runtime::PrepareInitialState() {
  return PrepareReplacement(nullptr, true);
}
std::unique_ptr<Runtime> Runtime::PrepareInitialState(const PreparedSources& sources) {
  return PrepareReplacement(nullptr, true, &sources);
}

std::vector<Manifest> Runtime::Sources() const {
  std::vector<Manifest> result;
  for (const auto& mod : mods_) result.push_back(mod->manifest);
  return result;
}

std::unique_ptr<Runtime> Runtime::PrepareReload(const PreparedSources& sources) {
  CheckThread();
  const auto state = SaveInternal(false, true);
  return PrepareReplacement(&state, false, &sources, true);
}

std::unique_ptr<Runtime> Runtime::PrepareReplacement(const Json* state, bool checkpoint,
                                                 const PreparedSources* sources, bool reload) {
  if (active()) CheckThread();
  std::optional<PreparedSources> synchronous;
  if (!sources) {
    synchronous = PrepareSources(Sources(), storage_ / ".native-images", false);
    sources = &*synchronous;
  }
  sources->Validate(Sources(), reload);
  auto next = std::make_unique<Runtime>(game_, log_, locale_, host_);
  next->localizations_ = localizations_;
  next->game_text_ = game_text_;
  next->model_edits_ = model_edits_;
  next->mesh_edits_ = mesh_edits_;
  next->texture_edits_ = texture_edits_;
  next->storage_ = storage_;
  next->storage_cache_ = storage_cache_;
  next->preparing_ = true;
  next->started_ = true;
  next->thread_ = std::this_thread::get_id();
  next->frame_ = state ? state->value("frame", uint64_t(0)) : 0;
  next->epoch_ = epoch_ + 1;
  next->level_ready_ = reload ? level_ready_ : !checkpoint;
  next->inspection_ = inspection_.load();
  for (const auto& prepared : sources->mods) {
    auto manifest = prepared;
    if (manifest.kind == "lua") next->mods_.push_back(MakeLuaMod(*next, std::move(manifest)));
    else if (manifest.kind == "native") next->mods_.push_back(MakeNativeMod(*next, std::move(manifest)));
    else next->mods_.push_back(std::make_unique<DataMod>(*next, std::move(manifest)));
    next->mods_.back()->Start();
  }
  // Build the replacement menus in their own runtime, then restore persistent
  // state. Original callbacks and menus remain live until the caller commits.
  next->Dispatch("initialize", Json::object());
  for (size_t i = 0; i < next->mods_.size(); ++i) {
    auto& mod = *next->mods_[i];
    if (mod.failed) throw Failure("save_incompatible", "Mod initialization failed: " + mod.manifest.name);
    if (state && mod.manifest.save_mode != "stateless")
      mod.Load(state->at("mods").at(i).at("state"));
    if (state) {
      next->restored_resources_[mod.manifest.id] = state->at("mods").at(i).value("host", Json::object());
      std::erase_if(next->timers_, [&](const auto& entry) { return entry.first.first == mod.manifest.id; });
      for (const auto& timer : state->at("mods").at(i).value("timers", Json::array())) {
        const TimerKey key{mod.manifest.id, timer.at("id").get<std::string>()};
        next->timers_[key] = {timer.at("seconds").get<double>(), timer.value("interval", 0.0),
                             timer.value("data", Json::object()), ++next->timer_generation_};
      }
      // Schema checks and migrations run while gameplay/storage mutations
      // are still blocked. A failure discards this replacement only.
      const auto mission_state = state->at("mods").at(i).value("missions", Missions::Empty());
      next->pending_mission_restore_[mod.manifest.id] = mission_state;
      next->Invoke(mod, "state.loaded", {{"epoch", next->epoch_},
          {"kind", reload ? "reload" : checkpoint ? "checkpoint" : "exact"},
          {"missions", mission_state}});
      if (mod.failed) throw Failure("save_incompatible", "Saved state validation failed: " + mod.manifest.name);
      if (next->pending_mission_restore_.contains(mod.manifest.id)) {
        next->missions_->Restore(mod.manifest.id, mission_state);
        next->pending_mission_restore_.erase(mod.manifest.id);
      }
    }
  }
  next->preparing_ = false;
  return next;
}

void Runtime::ApplyRestoredResources(bool checkpoint) {
  if (active()) {
    CheckThread();
    // The caller commits guest memory and retires the old runtime first.
    // Preflight only retains these values; it never acquires host resources.
    if (host_.restore)
      for (auto& mod : mods_) {
        if (mod->failed) continue;
        const auto found = restored_resources_.find(mod->manifest.id);
        if (found == restored_resources_.end()) continue;
        try { host_.restore(Owner(*mod), found->second, checkpoint); }
        catch (const std::exception& error) {
          mod->failed = true;
          ++mod->errors;
          Message(mod->manifest.id, std::string("Resource restore: ") + error.what());
          Retire(*mod);
        }
      }
    restored_resources_.clear();
  }
}

void Runtime::NotifyRestored(bool checkpoint) {
  ApplyRestoredResources(checkpoint);
  if (active()) Dispatch("save.restored", {{"epoch", epoch_}, {"kind", checkpoint ? "checkpoint" : "exact"}});
}

void Runtime::NotifyReloaded() {
  ApplyRestoredResources(false);
  if (active()) Dispatch("code.reloaded", {{"epoch", epoch_}, {"level_ready", level_ready_}});
}
void Runtime::EntityRemoved(const HostOwner& owner, const Json& data) {
  CheckThread();
  if (owner.instance != instance_) return;
  auto& mod = Find(owner.mod);
  if (mod.failed || mod.retired) return;
  Trace("entity.removed", {{"owner", owner.mod}, {"data", data}});
  Invoke(mod, "entity.removed", data);
}
void Runtime::EntityDied(const HostOwner& owner, const Json& data) {
  CheckThread();
  if (owner.instance != instance_) return;
  auto& mod = Find(owner.mod);
  if (mod.failed || mod.retired) return;
  Trace("entity.died", {{"owner", owner.mod}, {"data", data}});
  Invoke(mod, "entity.died", data);
}
const GameTextTables& Runtime::GameTexts() const {
  static const GameTextTables empty;
  return game_text_ ? *game_text_ : empty;
}

std::vector<fs::path> Runtime::AssetRoots() const {
  std::vector<fs::path> roots;
  for (auto it = mods_.rbegin(); it != mods_.rend(); ++it)
    if (fs::is_directory((*it)->manifest.folder / "gamedata"))
      roots.push_back((*it)->manifest.folder / "gamedata");
  return roots;
}
std::vector<MenuPage> Runtime::Pages() const {
  std::vector<MenuPage> result;
  for (const auto& [id, page] : pages_) result.push_back(page);
  return result;
}
void Runtime::Message(std::string_view owner, std::string_view message) {
  Trace("log", {{"owner", owner}, {"message", message.substr(0, 8192)}});
  if (log_) {
    // Failure reporting must not leave dispatch half-open or interrupt
    // retirement when an external logging sink is unavailable.
    try { log_("[modkit:" + std::string(owner) + "] " + std::string(message.substr(0, 8192))); }
    catch (...) {}
  }
}

}  // namespace simpsons::modkit
