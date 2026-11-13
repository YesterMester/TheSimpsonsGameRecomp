// The save library (saves.h): named saves grouped by playthrough, with exact
// snapshots of the game, pictures and copies of the game's slot A.
//
// The game's slots are SIMPSONS_SLOT1 to 4 (A to D) under <title>/00000001,
// each with a 328-byte header under <title>/Headers/00000001: two words, the
// text the Saved Games screen shows (UTF-16, big-endian, from +8: "7:05.05
// (3%) Springfield" is play time, completion and where) and the data's file
// name in ASCII at +264. A slot's data works as another slot once that name is
// changed. A watcher thread compares the slots with what the library last
// knew of them (slots.toml): on the first run every slot with progress is
// imported as a playthrough of its own; after that, new progress in slot A
// becomes an autosave of the current playthrough, and progress the game made
// in a slot while the library wasn't looking becomes an autosave of the
// playthrough that slot belonged to. The game rewrites slot A as a level
// starts without new progress, so a slot whose text hasn't changed is no new
// save.

#include "saves.h"

#include "game_canvas.h"
#include "game_thread.h"
#include "guest_memory.h"
#include "host_memory.h"
#include "snapshot.h"
#include "modkit_game.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <SDL3/SDL_surface.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(saves_autosaves, 3, "Game",
                     "How many of the game's autosaves the save library keeps for each "
                     "playthrough");
REXCVAR_DEFINE_STRING(saves_test_file, "", "Game",
                      "Development: save library commands read from this file ('create NAME', "
                      "'list', 'load ID|latest', 'rename ID NAME', 'delete ID')");

namespace saves {
namespace {

namespace fs = std::filesystem;
using simpsons::LoadGuestU32;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrontendState = 0x82D08C98;  // 9 while a level runs
// The game flow manager; its current episode at +96 has its name at +244.
constexpr uint32_t kGameFlowManager = 0x82D08BA8;
constexpr int kThumbnailWidth = 480;
constexpr int kSlotBackups = 10;
constexpr size_t kHeaderSize = 328, kHeaderText = 8, kHeaderName = 264;
constexpr size_t kPictureCache = 16;
constexpr int kPictureLoads = 2;

rex::Runtime* g_runtime = nullptr;

std::mutex g_mutex;  // the state below, shared with the background work
struct Worker {
  std::thread thread;
  std::shared_ptr<std::atomic<bool>> finished;
};
std::vector<Worker> g_workers;
std::atomic<bool> g_busy{false};
std::deque<std::function<void()>> g_on_game_thread;
// A snapshot read for PutRead.
snapshot::ImagePtr g_read;
std::shared_ptr<const simpsons::modkit::PreparedSources> g_read_sources;
Save g_read_save;
ReadState g_read_state = ReadState::kNone;
// Development: put it as soon as a level runs, then call this.
bool g_auto_put = false;
Done g_auto_put_done;
// The game just before the pause menu opened.
std::shared_ptr<const canvas::Picture> g_pause_picture;
uint64_t g_pause_pictures = 0;
// Saves' pictures by save id, the least recently used dropped first.
struct CachedPicture {
  std::shared_ptr<const canvas::Picture> picture;
  bool loading = false;
  uint64_t used = 0;
};
std::map<std::string, CachedPicture> g_pictures;
uint64_t g_picture_uses = 0;
int g_picture_loads = 0;
// What the game's slots held when the library last looked (slots.toml).
struct Known {
  std::string hash, text, playthrough;
};
std::array<Known, 4> g_known;
// The slots' watcher.
std::thread g_watcher;
std::mutex g_watcher_mutex;
std::condition_variable g_watcher_cv;
bool g_stop = false;
std::atomic<bool> g_tracking{false};

void OnGameThread(std::function<void()> work) {
  std::lock_guard lock(g_mutex);
  g_on_game_thread.push_back(std::move(work));
}

void Background(std::function<void()> work) {
  auto finished = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard lock(g_mutex);
  g_workers.push_back({std::thread([work = std::move(work), finished] {
                         work();
                         *finished = true;
                       }),
                       finished});
}

// --- Places and files --------------------------------------------------------

// <user data>/<XUID>/<title>: the game's content (00000001) and its headers.
fs::path TitleFolder() {
  auto* kernel = g_runtime ? g_runtime->kernel_state() : nullptr;
  if (!kernel || !kernel->user_profile() || g_runtime->user_data_root().empty())
    return {};
  return g_runtime->user_data_root() / fmt::format("{:016X}", kernel->user_profile()->xuid()) /
         fmt::format("{:08X}", kernel->title_id());
}

bool InsideRoot(const fs::path& folder) {
  const auto root = Root().lexically_normal();
  const auto path = folder.lexically_normal();
  return !root.empty() && path.parent_path() == root && !path.filename().empty() &&
         path.filename().string()[0] != '.' && path.filename() != "chunks" &&
         path.filename() != "slot-backups";
}

std::string NewId(const char* prefix) {
  static std::mt19937 random{std::random_device{}()};
  static std::mutex mutex;
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  std::lock_guard lock(mutex);
  return fmt::format("{}{}-{:04x}", prefix, stamp, random() & 0xFFFF);
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream data;
  data << in.rdbuf();
  return data.str();
}

// Written beside it, then moved over it.
bool WriteFile(const fs::path& path, const std::string& data) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out)
      return false;
  }
  fs::rename(temporary, path, error);
  return !error;
}

int64_t UnixTime(fs::file_time_type time) {
  const auto file_now = fs::file_time_type::clock::now();
  const auto system_now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(
             (system_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                               time - file_now))
                 .time_since_epoch())
      .count();
}

// --- The game's slots --------------------------------------------------------

std::string SlotName(int slot) {
  return fmt::format("SIMPSONS_SLOT{}", slot + 1);
}
fs::path SlotData(const fs::path& title, int slot) {
  return title / "00000001" / SlotName(slot) / SlotName(slot);
}
fs::path SlotHeader(const fs::path& title, int slot) {
  return title / "Headers" / "00000001" / (SlotName(slot) + ".header");
}

struct SlotFiles {
  std::string data, header;
};

bool ReadSlot(const fs::path& title, int slot, SlotFiles& files) {
  files.data = ReadFile(SlotData(title, slot));
  files.header = ReadFile(SlotHeader(title, slot));
  return !files.data.empty() && files.header.size() >= kHeaderSize;
}

// Slot files as another slot's: the header names its data file.
SlotFiles Rehome(SlotFiles files, int slot) {
  const auto name = SlotName(slot);
  if (files.header.size() >= kHeaderName + name.size())
    std::memcpy(files.header.data() + kHeaderName, name.data(), name.size());
  return files;
}

bool WriteSlot(const fs::path& title, int slot, const SlotFiles& files) {
  const auto rehomed = Rehome(files, slot);
  return WriteFile(SlotData(title, slot), rehomed.data) &&
         WriteFile(SlotHeader(title, slot), rehomed.header);
}

std::string SlotText(const std::string& header) {
  std::string text;
  for (size_t i = kHeaderText; i + 1 < kHeaderName && i + 1 < header.size(); i += 2) {
    const uint16_t c = uint16_t(uint8_t(header[i]) << 8 | uint8_t(header[i + 1]));
    if (!c)
      break;
    if (c < 0x80) {
      text += char(c);
    } else if (c < 0x800) {
      text += char(0xC0 | c >> 6);
      text += char(0x80 | (c & 0x3F));
    } else {
      text += char(0xE0 | c >> 12);
      text += char(0x80 | (c >> 6 & 0x3F));
      text += char(0x80 | (c & 0x3F));
    }
  }
  return text;
}

// "3:03:44.46 (39%) Big Super Happy Fun Fun Game": play time, completion and
// where.
void ParseSlotText(const std::string& text, Save& save) {
  const size_t open = text.find(" ("), close = text.find("%)");
  if (open == std::string::npos || close == std::string::npos || close < open) {
    save.location = text;
    return;
  }
  double seconds = 0;
  size_t start = 0;
  const std::string time = text.substr(0, open);
  while (start <= time.size()) {
    size_t colon = time.find(':', start);
    if (colon == std::string::npos)
      colon = time.size();
    seconds = seconds * 60 + std::strtod(time.c_str() + start, nullptr);
    start = colon + 1;
  }
  save.play_time = seconds;
  save.completion = std::strtof(text.c_str() + open + 2, nullptr);
  auto where = text.substr(close + 2);
  while (!where.empty() && where.front() == ' ')
    where.erase(where.begin());
  save.location = where;
}

bool EmptySlot(const std::string& text) {
  Save save;
  ParseSlotText(text, save);
  return save.play_time <= 0 && save.completion <= 0;
}

std::string SlotHash(const SlotFiles& files) {
  return fmt::format("{:016x}", uint64_t(XXH3_64bits(files.data.data(), files.data.size())));
}

void LoadKnown() {
  std::array<Known, 4> known;
  try {
    const auto table = toml::parse_file(rex::path_to_utf8(Root() / "slots.toml"));
    for (int slot = 0; slot < 4; ++slot) {
      const std::string letter(1, char('A' + slot));
      known[size_t(slot)] = {table[letter]["hash"].value_or(std::string()),
                             table[letter]["text"].value_or(std::string()),
                             table[letter]["playthrough"].value_or(std::string())};
    }
  } catch (const toml::parse_error&) {
  }
  std::lock_guard lock(g_mutex);
  g_known = known;
}

void StoreKnown() {
  toml::table table;
  {
    std::lock_guard lock(g_mutex);
    for (int slot = 0; slot < 4; ++slot) {
      const auto& known = g_known[size_t(slot)];
      table.insert(std::string(1, char('A' + slot)),
                   toml::table{{"hash", known.hash},
                               {"text", known.text},
                               {"playthrough", known.playthrough}});
    }
  }
  std::ostringstream out;
  out << table << "\n";
  WriteFile(Root() / "slots.toml", out.str());
}

void SetKnown(int slot, const Known& known) {
  {
    std::lock_guard lock(g_mutex);
    g_known[size_t(slot)] = known;
  }
  StoreKnown();
}

Known GetKnown(int slot) {
  std::lock_guard lock(g_mutex);
  return g_known[size_t(slot)];
}

// Keeps a copy of the game's slot A among the last few.
void BackupSlotA() {
  const auto title = TitleFolder();
  SlotFiles files;
  if (title.empty() || !ReadSlot(title, 0, files))
    return;
  const auto backups = Root() / "slot-backups";
  const auto folder = backups / NewId("");
  WriteFile(SlotData(folder, 0), files.data);
  WriteFile(SlotHeader(folder, 0), files.header);
  std::error_code error;
  std::vector<fs::path> old;
  for (const auto& entry : fs::directory_iterator(backups, error))
    old.push_back(entry.path());
  std::sort(old.begin(), old.end());
  for (size_t i = 0; i + kSlotBackups < old.size(); ++i)
    fs::remove_all(old[i], error);
}

// --- Metadata ----------------------------------------------------------------

bool WriteMeta(const Save& save) {
  toml::table table{
      {"name", save.name},
      {"playthrough", save.playthrough},
      {"playthrough_name", save.playthrough_name},
      {"kind", save.kind},
      {"created", save.created},
      {"location", save.location},
      {"play_time", save.play_time},
      {"completion", double(save.completion)},
      {"exact", save.exact},
      {"layout", save.layout},
      {"bytes", int64_t(save.bytes)},
      {"version", 1},
  };
  std::ostringstream out;
  out << table << "\n";
  return WriteFile(save.folder / "save.toml", out.str());
}

bool ReadMeta(const fs::path& folder, Save& save) {
  try {
    const auto table = toml::parse_file(rex::path_to_utf8(folder / "save.toml"));
    save.id = folder.filename().string();
    save.folder = folder;
    save.name = table["name"].value_or(std::string());
    save.playthrough = table["playthrough"].value_or(std::string());
    save.playthrough_name = table["playthrough_name"].value_or(std::string());
    save.kind = table["kind"].value_or(std::string("manual"));
    save.created = table["created"].value_or(int64_t(0));
    save.location = table["location"].value_or(std::string());
    save.play_time = table["play_time"].value_or(0.0);
    save.completion = float(table["completion"].value_or(0.0));
    save.exact = table["exact"].value_or(false);
    save.layout = table["layout"].value_or(std::string());
    save.bytes = uint64_t(table["bytes"].value_or(int64_t(0)));
    return true;
  } catch (const toml::parse_error&) {
    return false;
  }
}

// Where the game is and how far along, from its memory (game thread).
void Describe(PPCContext& ctx, uint8_t* base, Save& save) {
  const uint32_t manager = LoadGuestU32(base, kGameFlowManager);
  const uint32_t episode = manager ? LoadGuestU32(base, manager + 96) : 0;
  const uint32_t name = episode ? LoadGuestU32(base, episode + 244) : 0;
  if (name >= 0x10000) {
    const char* text = reinterpret_cast<const char*>(simpsons::GuestToHost(base, name));
    size_t length = 0;
    while (length < 64 && text[length] >= 32 && text[length] < 127)
      ++length;
    save.location.assign(text, length);
  }
  save.play_time = scores::Get(ctx, base, "SCORE_TOTAL_GAME_TIME");
  const float scale = scores::Get(ctx, base, "SCORE_GAME_PERCENT_COMPLETE_SCALE");
  save.completion =
      scale > 0 ? 100.0f * scores::Get(ctx, base, "SCORE_GAME_PERCENT_COMPLETE") / scale : 0.0f;
}

// --- Pictures ----------------------------------------------------------------

// The last frame the game showed, scaled down (any thread).
bool CaptureFrame(std::vector<uint8_t>& rgba, int& width, int& height) {
  auto* graphics = g_runtime ? dynamic_cast<rex::graphics::GraphicsSystem*>(
                                   g_runtime->graphics_system())
                             : nullptr;
  rex::ui::RawImage image;
  if (!graphics || !graphics->presenter() || !graphics->presenter()->CaptureGuestOutput(image) ||
      !image.width || !image.height)
    return false;
  width = std::min<int>(kThumbnailWidth, int(image.width));
  height = std::max(1, int(int64_t(image.height) * width / image.width));
  rgba.assign(size_t(width) * height * 4, 255);
  // A box filter over the source pixels each thumbnail pixel covers.
  for (int y = 0; y < height; ++y) {
    const uint32_t y0 = uint32_t(int64_t(y) * image.height / height);
    const uint32_t y1 = std::max(y0 + 1, uint32_t(int64_t(y + 1) * image.height / height));
    for (int x = 0; x < width; ++x) {
      const uint32_t x0 = uint32_t(int64_t(x) * image.width / width);
      const uint32_t x1 = std::max(x0 + 1, uint32_t(int64_t(x + 1) * image.width / width));
      uint32_t sum[3] = {}, count = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* row = image.data.data() + image.stride * sy;
        for (uint32_t sx = x0; sx < x1; ++sx) {
          sum[0] += row[sx * 4 + 0];
          sum[1] += row[sx * 4 + 1];
          sum[2] += row[sx * 4 + 2];
          ++count;
        }
      }
      uint8_t* out = rgba.data() + (size_t(y) * width + x) * 4;
      for (int c = 0; c < 3; ++c)
        out[c] = uint8_t(sum[c] / count);
    }
  }
  return true;
}

bool WritePng(const fs::path& path, const std::vector<uint8_t>& rgba, int width, int height) {
  if (rgba.empty())
    return false;
  SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                                               const_cast<uint8_t*>(rgba.data()), width * 4);
  if (!surface)
    return false;
  const bool ok = SDL_SavePNG(surface, rex::path_to_utf8(path).c_str());
  SDL_DestroySurface(surface);
  return ok;
}

std::shared_ptr<canvas::Picture> ReadPng(const fs::path& path, uint64_t id) {
  SDL_Surface* loaded = SDL_LoadPNG(rex::path_to_utf8(path).c_str());
  if (!loaded)
    return nullptr;
  SDL_Surface* surface = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(loaded);
  if (!surface)
    return nullptr;
  auto picture = std::make_shared<canvas::Picture>();
  picture->width = uint32_t(surface->w);
  picture->height = uint32_t(surface->h);
  picture->id = id;
  picture->rgba.resize(size_t(surface->w) * surface->h * 4);
  for (int y = 0; y < surface->h; ++y)
    std::memcpy(picture->rgba.data() + size_t(y) * surface->w * 4,
                static_cast<const uint8_t*>(surface->pixels) + size_t(y) * surface->pitch,
                size_t(surface->w) * 4);
  SDL_DestroySurface(surface);
  return picture;
}

// The memory set aside for the recompilation's own code, which exact saves
// depend on (host_memory.h).
std::string Layout() {
  std::string text;
  for (const auto& region : host_memory::Regions())
    text += fmt::format("{:08X}:{:X};", region.address, region.size);
  return fmt::format("{:016x}", uint64_t(XXH3_64bits(text.data(), text.size())));
}

uint64_t PictureId(const Save& save) {
  const std::string key = save.id + "/" + std::to_string(save.created);
  return uint64_t(XXH3_64bits(key.data(), key.size())) | 1;
}

// --- Playthroughs ------------------------------------------------------------

Playthrough ReadCurrent() {
  try {
    const auto table = toml::parse_file(rex::path_to_utf8(Root() / "current.toml"));
    return {table["id"].value_or(std::string()), table["name"].value_or(std::string())};
  } catch (const toml::parse_error&) {
    return {};
  }
}

void WriteCurrent(const Playthrough& playthrough) {
  toml::table table{{"id", playthrough.id}, {"name", playthrough.name}};
  std::ostringstream out;
  out << table << "\n";
  WriteFile(Root() / "current.toml", out.str());
}

// A playthrough's name, from its newest save.
std::string PlaythroughName(const std::string& id) {
  for (const auto& save : List(id))
    if (!save.playthrough_name.empty())
      return save.playthrough_name;
  return {};
}

// --- The slots' watcher ------------------------------------------------------

// A save of a slot as the game left it.
bool SaveSlot(const SlotFiles& files, const std::string& text, const Playthrough& playthrough,
              const std::string& kind, const std::string& name, int64_t created, bool picture,
              const std::string& mod_state) {
  Save save;
  save.id = NewId("");
  save.name = name;
  save.kind = kind;
  save.created = created;
  save.playthrough = playthrough.id;
  save.playthrough_name = playthrough.name;
  ParseSlotText(text, save);
  save.folder = Root() / save.id;
  const auto temporary = Root() / ("." + save.id);
  std::error_code error;
  fs::create_directories(temporary, error);
  const auto rehomed = Rehome(files, 0);
  bool ok = !error && WriteFile(SlotData(temporary / "slots", 0), rehomed.data) &&
            WriteFile(SlotHeader(temporary / "slots", 0), rehomed.header);
  if (ok && !mod_state.empty()) ok = WriteFile(temporary / "mods.json", mod_state);
  if (ok && picture) {
    std::vector<uint8_t> frame;
    int width = 0, height = 0;
    if (CaptureFrame(frame, width, height))
      WritePng(temporary / "thumb.png", frame, width, height);
  }
  for (const auto& entry : fs::recursive_directory_iterator(temporary, error))
    if (entry.is_regular_file(error))
      save.bytes += entry.file_size(error);
  const auto folder = save.folder;
  save.folder = temporary;
  ok = ok && WriteMeta(save);
  if (ok) {
    fs::rename(temporary, folder, error);
    ok = !error;
  }
  if (!ok)
    fs::remove_all(temporary, error);
  REXLOG_INFO("[saves] {} {} of '{}': {} ({})", kind, ok ? save.id : "FAILED", playthrough.name,
              text, name);
  return ok;
}

// The watcher never executes mod code. Request a bounded game-thread capture
// and let the existing save worker commit the sidecar with the slot files.
std::string CaptureCheckpointState() {
  if (!modkit_game::Active()) return {};
  if (modkit_game::IsGameThread()) return modkit_game::SaveState(false).dump();
  auto promise = std::make_shared<std::promise<std::string>>();
  auto state = promise->get_future();
  OnGameThread([promise] {
    try { promise->set_value(modkit_game::SaveState(false).dump()); }
    catch (...) { promise->set_exception(std::current_exception()); }
  });
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (state.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
    std::lock_guard lock(g_watcher_mutex);
    if (g_stop || Clock::now() >= deadline)
      throw std::runtime_error("Game thread did not provide checkpoint mod state");
  }
  return state.get();
}

// Keeps a playthrough's newest autosaves.
void PruneAutosaves(const std::string& playthrough) {
  const int keep = std::max(1, REXCVAR_GET(saves_autosaves));
  int seen = 0;
  for (const auto& save : List(playthrough)) {
    if (save.kind != "auto" || ++seen <= keep)
      continue;
    std::error_code error;
    fs::remove_all(save.folder, error);
  }
}

// Compares a slot with what the library knew of it; new progress becomes a
// save. first: the library's first look, which imports every slot.
void CheckSlot(const fs::path& title, int slot, bool first) {
  SlotFiles files;
  if (!ReadSlot(title, slot, files))
    return;
  const std::string text = SlotText(files.header), hash = SlotHash(files);
  Known known = GetKnown(slot);
  if (hash == known.hash)
    return;
  const char letter = char('A' + slot);
  Known now{hash, text, known.playthrough};
  if (EmptySlot(text) || (text == known.text && !first)) {
    SetKnown(slot, now);
    return;
  }
  std::error_code error;
  const auto written = fs::last_write_time(SlotData(title, slot), error);
  const int64_t created = error ? int64_t(std::time(nullptr)) : UnixTime(written);
  Playthrough playthrough;
  if (slot == 0 && g_tracking.load())
    playthrough = Current();
  else if (!known.playthrough.empty())
    playthrough = {known.playthrough, PlaythroughName(known.playthrough)};
  const bool import = playthrough.id.empty();
  if (import)
    playthrough = {NewId("pt-"), fmt::format("Slot {}", letter)};
  if (playthrough.name.empty())
    playthrough.name = fmt::format("Slot {}", letter);
  std::string mod_state;
  if (!import) {
    try { mod_state = CaptureCheckpointState(); }
    catch (const std::exception& error) {
      REXLOG_ERROR("[saves] Checkpoint mod state could not be saved: {}", error.what());
      return;
    }
    // Capturing host state can take a frame. Retry rather than combining it
    // with a slot the game has replaced in the meantime.
    SlotFiles current;
    if (!ReadSlot(title, slot, current) || SlotHash(current) != hash) return;
  }
  const bool ok =
      import ? SaveSlot(files, text, playthrough, "imported",
                        fmt::format("Imported from slot {}", letter), created, false, {})
             : SaveSlot(files, text, playthrough, "auto", "Autosave", created,
                        slot == 0 && g_tracking, mod_state);
  if (!ok)
    return;
  if (!import)
    PruneAutosaves(playthrough.id);
  now.playthrough = playthrough.id;
  SetKnown(slot, now);
}

void Watch() {
  const auto title = TitleFolder();
  if (title.empty() || Root().empty())
    return;
  std::error_code error;
  fs::create_directories(Root(), error);
  const bool first = !fs::exists(Root() / "slots.toml", error);
  LoadKnown();
  for (int slot = 0; slot < 4; ++slot)
    CheckSlot(title, slot, first);
  StoreKnown();
  if (ReadCurrent().id.empty()) {
    const auto latest = Latest();
    if (!latest.playthrough.empty())
      WriteCurrent({latest.playthrough, latest.playthrough_name});
  }
  // Slot A from now on, once the game has finished writing it.
  fs::file_time_type last_time{};
  bool changed = false;
  Clock::time_point stable{};
  std::unique_lock lock(g_watcher_mutex);
  while (!g_stop) {
    g_watcher_cv.wait_for(lock, std::chrono::milliseconds(700));
    if (g_stop)
      break;
    lock.unlock();
    const auto time = fs::last_write_time(SlotData(title, 0), error);
    const auto header_time = fs::last_write_time(SlotHeader(title, 0), error);
    const auto newest = std::max(time, header_time);
    if (!error && newest != last_time) {
      last_time = newest;
      changed = true;
      stable = Clock::now() + std::chrono::milliseconds(1500);
    } else if (changed && Clock::now() >= stable) {
      changed = false;
      CheckSlot(title, 0, false);
    }
    lock.lock();
  }
}

// --- Development commands ----------------------------------------------------

void Command(PPCContext& ctx, uint8_t* base, const std::string& line) {
  std::istringstream in(line);
  std::string op, id;
  in >> op;
  if (op == "create" || op == "checkpoint") {
    std::string name;
    std::getline(in >> std::ws, name);
    auto done = [](bool ok, const Save& save) {
      REXLOG_INFO("[saves] create {}: {} '{}' at {} ({:.1f} MB)", ok ? "done" : "FAILED", save.id,
                  save.name, save.location, save.bytes / 1048576.0);
    };
    if (op == "checkpoint") CreateCheckpoint(ctx, base, name, "manual", done);
    else Create(ctx, base, name, "manual", done);
  } else if (op == "list") {
    for (const auto& save : List())
      REXLOG_INFO("[saves] {} '{}' {} {} {} {:.0f}s {:.1f}% {} {:.1f} MB", save.id, save.name,
                  save.kind, save.playthrough_name, save.location, save.play_time,
                  save.completion, save.exact ? "exact" : "slot", save.bytes / 1048576.0);
  } else if (op == "load") {
    in >> id;
    const Save save = id == "latest" ? Latest() : Save{};
    if (id == "latest")
      id = save.id;
    const bool ok = Read(id);
    std::lock_guard lock(g_mutex);
    g_auto_put = ok;
    g_auto_put_done = [](bool ok, const Save& save) {
      REXLOG_INFO("[saves] load {}: {} '{}'", ok ? "done" : "FAILED", save.id, save.name);
    };
    if (!ok)
      REXLOG_INFO("[saves] load FAILED: {} can't be read", id);
  } else if (op == "rename") {
    std::string name;
    in >> id;
    std::getline(in >> std::ws, name);
    REXLOG_INFO("[saves] rename {}: {}", id, Rename(id, name) ? "done" : "FAILED");
  } else if (op == "delete") {
    in >> id;
    REXLOG_INFO("[saves] delete {}: {}", id, Remove(id) ? "done" : "FAILED");
  }
}

std::string Lower(std::string text) {
  for (char& c : text)
    c = char(std::tolower(uint8_t(c)));
  return text;
}

}  // namespace

void Init(rex::Runtime* runtime) {
  g_runtime = runtime;
  g_stop = false;
  if (kLibraryEnabled)
    g_watcher = std::thread(Watch);
}

void Shutdown() {
  {
    std::lock_guard lock(g_watcher_mutex);
    g_stop = true;
  }
  g_watcher_cv.notify_all();
  if (g_watcher.joinable())
    g_watcher.join();
  for (;;) {
    std::vector<Worker> workers;
    {
      std::lock_guard lock(g_mutex);
      workers.swap(g_workers);
    }
    if (workers.empty())
      break;
    for (auto& worker : workers)
      if (worker.thread.joinable())
        worker.thread.join();
  }
}

fs::path Root() {
  if (!g_runtime || g_runtime->user_data_root().empty())
    return {};
  return g_runtime->user_data_root() / "saves";
}

std::vector<Save> List(const std::string& playthrough, const std::string& search) {
  std::vector<std::string> words;
  {
    std::istringstream in(Lower(search));
    for (std::string word; in >> word;)
      words.push_back(word);
  }
  std::vector<Save> list;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(Root(), error)) {
    Save save;
    if (!entry.is_directory(error) || !InsideRoot(entry.path()) || !ReadMeta(entry.path(), save))
      continue;
    if (!playthrough.empty() && save.playthrough != playthrough)
      continue;
    const std::string text = Lower(save.name + " " + save.playthrough_name + " " + save.location);
    if (std::all_of(words.begin(), words.end(),
                    [&](const std::string& word) { return text.find(word) != std::string::npos; }))
      list.push_back(std::move(save));
  }
  std::sort(list.begin(), list.end(), [](const Save& a, const Save& b) {
    return a.created != b.created ? a.created > b.created : a.id > b.id;
  });
  return list;
}

void Scan(std::function<void(std::vector<Save>)> done) {
  Background([done = std::move(done)] {
    auto list = List();
    OnGameThread([done, list = std::move(list)]() mutable {
      if (done)
        done(std::move(list));
    });
  });
}

Save Latest(const std::string& playthrough) {
  auto list = List(playthrough);
  return list.empty() ? Save{} : list.front();
}

std::vector<Playthrough> Playthroughs() {
  std::vector<Playthrough> playthroughs;
  for (const auto& save : List()) {
    if (std::none_of(playthroughs.begin(), playthroughs.end(),
                     [&](const Playthrough& p) { return p.id == save.playthrough; }))
      playthroughs.push_back({save.playthrough, save.playthrough_name});
  }
  return playthroughs;
}

bool Exact(const Save& save) {
  // Saves from before the layout was kept are checked as they're read.
  return save.exact && (save.layout.empty() || save.layout == Layout()) &&
         modkit_game::Compatible(save.folder);
}

bool Rename(const std::string& id, const std::string& name) {
  Save save;
  if (name.empty() || !InsideRoot(Root() / id) || !ReadMeta(Root() / id, save))
    return false;
  save.name = name;
  return WriteMeta(save);
}

bool Remove(const std::string& id) {
  const auto folder = Root() / id;
  Save save;
  if (!InsideRoot(folder) || !ReadMeta(folder, save))
    return false;
  std::error_code error;
  fs::remove_all(folder, error);
  if (error)
    return false;
  {
    std::lock_guard lock(g_mutex);
    g_pictures.erase(id);
  }
  if (save.exact)
    Background([] {
      std::vector<fs::path> files;
      for (const auto& save : List())
        if (save.exact)
          files.push_back(save.folder / "state.snap");
      const size_t removed = snapshot::CollectChunks(Root() / "chunks", files);
      REXLOG_INFO("[saves] removed {} unused chunks", removed);
    });
  return true;
}

Playthrough Current() {
  std::lock_guard lock(g_mutex);
  return ReadCurrent();
}

void SetCurrent(const Playthrough& playthrough) {
  std::lock_guard lock(g_mutex);
  WriteCurrent(playthrough);
}

Playthrough NewPlaythrough(const std::string& name) {
  Playthrough playthrough{NewId("pt-"), name};
  SetCurrent(playthrough);
  return playthrough;
}

bool Busy() {
  return g_busy.load();
}

static bool CreateSave(PPCContext& ctx, uint8_t* base, const std::string& name,
                       const std::string& kind, Done done, const std::string& replace,
                       bool exact) {
  modkit_game::Context context(ctx, base);
  const auto title = TitleFolder();
  if (title.empty() || g_busy.exchange(true))
    return false;
  std::string mod_state;
  exact = exact && modkit_game::SupportsExact();
  try {
    mod_state = modkit_game::SaveState(exact).dump();
  } catch (const std::exception& error) {
    REXLOG_ERROR("[saves] Cannot save this mod state: {}", error.what());
    snapshot::Release();
    g_busy = false;
    return false;
  }
  Save save;
  save.id = NewId("");
  save.name = name.empty() ? "Save" : name;
  save.kind = kind;
  save.created = int64_t(std::time(nullptr));
  save.folder = Root() / save.id;
  auto playthrough = Current();
  if (playthrough.id.empty())
    playthrough = NewPlaythrough("Playthrough");
  save.playthrough = playthrough.id;
  save.playthrough_name = playthrough.name;
  Describe(ctx, base, save);
  // The slot and the picture now, before the game moves on.
  SlotFiles slot;
  ReadSlot(title, 0, slot);
  // Outside Springfield the episode has no name to read: the slot's.
  if (save.location.empty() && !slot.header.empty()) {
    Save described;
    ParseSlotText(SlotText(slot.header), described);
    save.location = described.location;
  }
  std::shared_ptr<const canvas::Picture> frame;
  {
    std::lock_guard lock(g_mutex);
    frame = g_pause_picture;
  }
  auto image = std::shared_ptr<snapshot::Image>(exact ? snapshot::Take(ctx).release() : nullptr,
                                                snapshot::ImageDeleter());
  if (!exact) snapshot::Release();
  save.exact = image != nullptr;
  if (save.exact)
    save.layout = Layout();
  Background([save, slot = std::move(slot), frame, image, replace, mod_state = std::move(mod_state),
              done = std::move(done)]() mutable {
    const auto start = Clock::now();
    const auto temporary = Root() / ("." + save.id);
    std::error_code error;
    fs::create_directories(temporary, error);
    bool ok = !error;
    if (ok) ok = WriteFile(temporary / "mods.json", mod_state);
    // No picture from before the pause: the frame on screen now.
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    if (frame) {
      rgba = frame->rgba;
      width = int(frame->width);
      height = int(frame->height);
    } else {
      CaptureFrame(rgba, width, height);
    }
    if (ok && !rgba.empty())
      WritePng(temporary / "thumb.png", rgba, width, height);
    if (ok && !slot.data.empty()) {
      const auto rehomed = Rehome(slot, 0);
      ok = WriteFile(SlotData(temporary / "slots", 0), rehomed.data) &&
           WriteFile(SlotHeader(temporary / "slots", 0), rehomed.header);
    }
    snapshot::Stats stats;
    if (ok && image)
      ok = snapshot::Write(*image, temporary / "state.snap", Root() / "chunks", &stats);
    save.bytes = stats.stored;
    for (const auto& entry : fs::recursive_directory_iterator(temporary, error))
      if (entry.is_regular_file(error) && entry.path().filename() != "state.snap")
        save.bytes += entry.file_size(error);
    const auto folder = save.folder;
    save.folder = temporary;
    ok = ok && WriteMeta(save);
    if (ok) {
      fs::rename(temporary, folder, error);
      ok = !error;
    }
    save.folder = folder;
    if (!ok)
      fs::remove_all(temporary, error);
    REXLOG_INFO("[saves] wrote {} in {:.0f} ms ({:.1f} MB, {} of {} chunks new)", save.id,
                std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                save.bytes / 1048576.0, stats.new_chunks, stats.chunks);
    if (ok && !replace.empty() && replace != save.id)
      Remove(replace);
    g_busy = false;
    OnGameThread([ok, save, done = std::move(done)] {
      if (done)
        done(ok, save);
    });
  });
  return true;
}

bool Create(PPCContext& ctx, uint8_t* base, const std::string& name, const std::string& kind,
            Done done, const std::string& replace) {
  return CreateSave(ctx, base, name, kind, std::move(done), replace, true);
}
bool CreateCheckpoint(PPCContext& ctx, uint8_t* base, const std::string& name,
                      const std::string& kind, Done done, const std::string& replace) {
  return CreateSave(ctx, base, name, kind, std::move(done), replace, false);
}

bool PrepareSlot(const Save& save, bool restore_checkpoint) {
  // Checkpoint preflight validates the worker's captured sidecar. Re-reading
  // it here would put large mod states back on the game thread and race that
  // immutable capture. Exact loads retain their independent compatibility gate.
  if (!restore_checkpoint && !modkit_game::Compatible(save.folder, !save.exact)) return false;
  const auto title = TitleFolder();
  SlotFiles files;
  if (title.empty() || !ReadSlot(save.folder / "slots", 0, files))
    return false;
  // Progress the game wrote just now (leaving a level saves) becomes an
  // autosave first, before its slot is replaced.
  CheckSlot(title, 0, false);
  if (restore_checkpoint && !modkit_game::PrepareCheckpoint(save.folder)) return false;
  BackupSlotA();
  if (!WriteSlot(title, 0, files)) {
    if (restore_checkpoint) modkit_game::CancelRestore();
    return false;
  }
  const auto text = SlotText(files.header);
  SetKnown(0, {SlotHash(files), text, save.playthrough});
  SetCurrent({save.playthrough, save.playthrough_name});
  return true;
}

void BackupSlot() {
  BackupSlotA();
}

std::string Where(PPCContext& ctx, uint8_t* base) {
  Save save;
  Describe(ctx, base, save);
  if (save.location.empty()) {
    const auto title = TitleFolder();
    SlotFiles files;
    if (!title.empty() && ReadSlot(title, 0, files))
      ParseSlotText(SlotText(files.header), save);
  }
  return save.location;
}

bool SlotAEmpty() {
  const auto title = TitleFolder();
  SlotFiles files;
  return title.empty() || !ReadSlot(title, 0, files) || EmptySlot(SlotText(files.header));
}

void Track(bool on) {
  g_tracking = on;
}

bool Tracking() {
  return g_tracking.load();
}

bool Read(const std::string& id) {
  Save save;
  if (!InsideRoot(Root() / id) || !ReadMeta(Root() / id, save) || !save.exact)
    return false;
  if (g_busy.exchange(true))
    return false;
  {
    std::lock_guard lock(g_mutex);
    g_read.reset();
    g_read_sources.reset();
    g_read_save = save;
    g_read_state = ReadState::kReading;
  }
  Background([save]() mutable {
    const auto start = Clock::now();
    auto image = snapshot::Read(save.folder / "state.snap", Root() / "chunks");
    const bool fits = image && snapshot::Fits(*image);
    std::shared_ptr<const simpsons::modkit::PreparedSources> sources;
    if (fits) {
      try { sources = modkit_game::VerifyRestoreSources(); }
      catch (const std::exception& error) {
        REXLOG_ERROR("[saves] mod source preflight failed: {}", error.what());
        image.reset();
      }
    }
    REXLOG_INFO("[saves] read {} in {:.0f} ms{}", save.id,
                std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                !image ? " (FAILED)" : fits ? "" : " (an older build's: checkpoint only)");
    std::lock_guard lock(g_mutex);
    if (g_read_state != ReadState::kReading || g_read_save.id != save.id)
      return;
    g_read_state = !image ? ReadState::kFailed : fits ? ReadState::kReady : ReadState::kOlder;
    if (fits && image) {
      g_read = std::move(image);
      g_read_sources = std::move(sources);
    } else
      g_busy = false;
  });
  return true;
}

ReadState ReadStatus() {
  std::lock_guard lock(g_mutex);
  return g_read_state;
}

bool PutRead(PPCContext& ctx, uint8_t* base) {
  snapshot::Image* image;
  std::shared_ptr<const simpsons::modkit::PreparedSources> sources;
  fs::path folder;
  {
    std::lock_guard lock(g_mutex);
    if (g_read_state != ReadState::kReady || !g_read)
      return false;
    image = g_read.get();
    sources = g_read_sources;
    folder = g_read_save.folder;
  }
  // The game thread owns a ready snapshot until it is dropped.
  if (!modkit_game::PrepareRestore(folder, ctx, base, sources.get()))
    return false;
  if (!snapshot::Put(ctx, *image)) {
    modkit_game::CancelRestore();
    return false;
  }
  modkit_game::CommitRestore(ctx, base);
  std::lock_guard lock(g_mutex);
  g_read.reset();
  g_read_sources.reset();
  g_read_state = ReadState::kNone;
  g_busy = false;
  return true;
}

void DropRead() {
  std::lock_guard lock(g_mutex);
  if (g_read_state == ReadState::kNone)
    return;
  g_read.reset();
  g_read_sources.reset();
  g_read_state = ReadState::kNone;
  g_busy = false;
}

std::shared_ptr<const canvas::Picture> Picture(const Save& save) {
  if (save.id.empty())
    return nullptr;
  {
    std::lock_guard lock(g_mutex);
    auto& cached = g_pictures[save.id];
    cached.used = ++g_picture_uses;
    if (cached.picture || cached.loading || g_picture_loads >= kPictureLoads)
      return cached.picture;
    cached.loading = true;
    ++g_picture_loads;
    // The least recently used go.
    while (g_pictures.size() > kPictureCache) {
      auto oldest = g_pictures.end();
      for (auto it = g_pictures.begin(); it != g_pictures.end(); ++it)
        if (!it->second.loading &&
            (oldest == g_pictures.end() || it->second.used < oldest->second.used))
          oldest = it;
      if (oldest == g_pictures.end())
        break;
      g_pictures.erase(oldest);
    }
  }
  const auto path = save.folder / "thumb.png";
  const auto id = save.id;
  const uint64_t picture_id = PictureId(save);
  Background([path, id, picture_id] {
    std::error_code error;
    auto picture = fs::exists(path, error) ? ReadPng(path, picture_id) : nullptr;
    std::lock_guard lock(g_mutex);
    --g_picture_loads;
    const auto found = g_pictures.find(id);
    if (found == g_pictures.end())
      return;
    found->second.loading = false;
    // None: an empty picture, so it isn't read again.
    found->second.picture = picture ? picture : std::make_shared<canvas::Picture>();
  });
  return nullptr;
}

std::shared_ptr<const canvas::Picture> PausePicture() {
  std::lock_guard lock(g_mutex);
  return g_pause_picture;
}

void RememberFrame() {
  Background([] {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    if (!CaptureFrame(rgba, width, height))
      return;
    auto picture = std::make_shared<canvas::Picture>();
    picture->width = uint32_t(width);
    picture->height = uint32_t(height);
    picture->rgba = std::move(rgba);
    std::lock_guard lock(g_mutex);
    picture->id = (++g_pause_pictures << 1) | (uint64_t(1) << 63);
    g_pause_picture = std::move(picture);
  });
}

void Tick(PPCContext& ctx, uint8_t* base) {
  if (!g_runtime)
    return;
  // Watcher requests execute here too. Native serializers get a valid
  // read-only game context rather than depending on a save-menu callback.
  modkit_game::Context context(ctx, base);
  // Finished work.
  std::deque<std::function<void()>> work;
  std::vector<Worker> finished;
  {
    std::lock_guard lock(g_mutex);
    work.swap(g_on_game_thread);
    for (auto it = g_workers.begin(); it != g_workers.end();) {
      if (*it->finished) {
        finished.push_back(std::move(*it));
        it = g_workers.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& worker : finished)
    worker.thread.join();
  for (auto& item : work) {
    const PPCContext saved = ctx;
    item();
    ctx = saved;
  }
  // Development: a save loaded by command goes under the game once a level
  // runs.
  Done done;
  Save save;
  bool failed = false;
  {
    std::lock_guard lock(g_mutex);
    if (g_auto_put && (g_read_state == ReadState::kFailed || g_read_state == ReadState::kOlder ||
                       (g_read_state == ReadState::kReady &&
                        LoadGuestU32(base, kFrontendState) == 9))) {
      failed = g_read_state == ReadState::kFailed || g_read_state == ReadState::kOlder;
      g_auto_put = false;
      done = std::move(g_auto_put_done);
      save = g_read_save;
    }
  }
  if (done && !failed && !snapshot::Ready()) {
    // Next frame, once the game can be frozen.
    std::lock_guard lock(g_mutex);
    g_auto_put = true;
    g_auto_put_done = std::move(done);
    return;
  }
  if (done) {
    bool ok = false;
    if (!failed) {
      ok = PrepareSlot(save, false) && PutRead(ctx, base);
      if (!ok)
        DropRead();
    } else {
      DropRead();
    }
    // Runs after the game carries on from the save; keeps the registers Put
    // gave the game.
    done(ok, save);
  }
  // Development commands.
  const auto& path = REXCVAR_GET(saves_test_file);
  if (path.empty())
    return;
  std::ifstream in(path);
  if (!in)
    return;
  std::string line;
  std::getline(in, line);
  in.close();
  // The development transport follows the same audio preparation as the save
  // screen. Keep the command pending until capture is possible, rather than
  // silently turning its first request into a checkpoint with paused audio.
  std::istringstream command(line);
  std::string operation;
  command >> operation;
  if (operation == "create" && !Busy() && modkit_game::SupportsExact() && !snapshot::Ready())
    return;
  std::error_code error;
  fs::remove(path, error);
  Command(ctx, base, line);
}

}  // namespace saves
