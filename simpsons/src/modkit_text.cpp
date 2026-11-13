// Language packs use the original text tables and lookup paths. Only the
// returned string changes; graphics resources and dialogue IDs stay intact.
#include "modkit_text.h"
#include "guest_memory.h"
#include "host_memory.h"
#include "localization.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_8282D5B8);
REX_EXTERN(sub_8282D5B8);
REX_EXTERN(__imp__sub_8282D618);
REX_EXTERN(sub_8282D618);

namespace modkit_text {
namespace {
using namespace simpsons::modkit;
struct Line {
  std::string source8, source16;
  uint32_t narrow = 0, wide = 0;
};
struct Table {
  const std::map<uint32_t, Line>* lines = nullptr;
};
std::map<std::string, std::map<uint32_t, Line>> g_lines;
std::unordered_map<uint32_t, Table> g_tables;
std::unordered_set<uint32_t> g_keys;
std::mutex g_mutex;
std::atomic<bool> g_active{false};

std::string Wide(std::string_view text) {
  const auto utf16 = rex::string::to_utf16(std::string(text));
  std::string bytes;
  for (char16_t c : utf16) {
    bytes += char(c >> 8);
    bytes += char(c);
  }
  bytes.append(2, '\0');
  return bytes;
}
bool Same(const uint8_t* original, const std::string& expected, bool wide) {
  const size_t step = wide ? 2 : 1;
  for (size_t i = 0; i < expected.size(); i += step) {
    if (original[i] != uint8_t(expected[i]) ||
        (wide && original[i + 1] != uint8_t(expected[i + 1])))
      return false;
    if (!original[i] && (!wide || !original[i + 1]))
      return i + step == expected.size();
  }
  return false;
}
uint32_t Lookup(uint8_t* base, uint32_t table, uint32_t key, uint32_t original) {
  if (!g_active.load() || !original)
    return 0;
  std::lock_guard lock(g_mutex);
  if (!g_active.load())
    return 0;
  if (!g_keys.contains(key))
    return 0;
  auto found = g_tables.find(table);
  if (found == g_tables.end()) {
    Table match;
    try {
      const auto size = simpsons::LoadGuestU32(base, table + 4);
      if (size < 32 || size > 4 * 1024 * 1024)
        return 0;
      const auto hash = RelocatedTextTableHash({simpsons::GuestToHost(base, table), size}, table);
      if (const auto entry = g_lines.find(hash); entry != g_lines.end()) {
        match.lines = &entry->second;
        REXLOG_INFO("[modkit-text] matched table {} with {} translations", hash.substr(0, 12),
                    entry->second.size());
      }
    } catch (const std::exception& error) {
      REXLOG_WARN("[modkit-text] {}", error.what());
    }
    found = g_tables.emplace(table, match).first;
  }
  if (!found->second.lines)
    return 0;
  const auto line = found->second.lines->find(key);
  if (line == found->second.lines->end())
    return 0;
  const bool wide = (simpsons::LoadGuestU32(base, table + 8) & 0x80000000u) != 0;
  if (!Same(simpsons::GuestToHost(base, original),
            wide ? line->second.source16 : line->second.source8, wide))
    return 0;
  return wide ? line->second.wide : line->second.narrow;
}
void Forget(uint32_t table) {
  if (!g_active.load())
    return;
  std::lock_guard lock(g_mutex);
  g_tables.erase(table);
}
}  // namespace

void Init(rex::Runtime* runtime, const GameTextTables& tables) {
  std::lock_guard lock(g_mutex);
  g_active.store(false);
  g_tables.clear();
  g_lines.clear();
  g_keys.clear();
  if (tables.empty())
    return;
  struct Bytes {
    std::string digest;
    uint32_t key;
    std::string narrow, wide;
  };
  std::vector<Bytes> buffers;
  size_t total = 0;
  for (const auto& [digest, lines] : tables)
    for (const auto& [key, text] : lines) {
      g_keys.insert(key);
      auto& line = g_lines[digest][key];
      line.source8 = MenuText(text.source);
      line.source8 += '\0';
      line.source16 = Wide(text.source);
      Bytes buffer{digest, key, MenuText(text.text), Wide(text.text)};
      buffer.narrow += '\0';
      total += buffer.narrow.size();
      total = (total + 1) & ~size_t(1);
      total += buffer.wide.size();
      if (total > 16 * 1024 * 1024)
        throw Failure("resource_limit", "Game translation strings exceed 16 MiB");
      buffers.push_back(std::move(buffer));
    }
  const auto pool = host_memory::AllocBlock(uint32_t(total));
  if (!pool)
    throw Failure("allocation_failed", "Cannot allocate native translation strings");
  uint32_t offset = 0;
  auto* destination = runtime->memory()->TranslateVirtual<uint8_t*>(pool);
  for (const auto& buffer : buffers) {
    auto& line = g_lines.at(buffer.digest).at(buffer.key);
    line.narrow = pool + offset;
    std::memcpy(destination + offset, buffer.narrow.data(), buffer.narrow.size());
    offset += uint32_t(buffer.narrow.size());
    offset = (offset + 1) & ~1u;
    line.wide = pool + offset;
    std::memcpy(destination + offset, buffer.wide.data(), buffer.wide.size());
    offset += uint32_t(buffer.wide.size());
  }
  g_active.store(true);
  REXLOG_INFO("[modkit-text] prepared {} native translations in {} bytes", buffers.size(), total);
}
void Invalidate() {
  std::lock_guard lock(g_mutex);
  g_tables.clear();
}
void Shutdown() {
  g_active.store(false);
  std::lock_guard lock(g_mutex);
  g_tables.clear();
  g_lines.clear();
  g_keys.clear();
}
}  // namespace modkit_text

REX_FUNC(sub_8282D5B8) {
  const auto table = ctx.r3.u32, key = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_8282D5B8(ctx, base);
  if ((ctx.r3.u32 & 0xFF) && out) {
    const auto replacement =
        modkit_text::Lookup(base, table, key, simpsons::LoadGuestU32(base, out));
    if (replacement)
      simpsons::StoreGuestU32(base, out, replacement);
  }
}
REX_FUNC(sub_8282D618) {
  modkit_text::Forget(ctx.r3.u32);
  __imp__sub_8282D618(ctx, base);
}
