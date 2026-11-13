#include "modkit_meshes.h"
#include "guest_memory.h"
#include "resource_id.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace modkit_meshes {
namespace {
std::atomic<bool> g_active{false};
std::mutex g_mutex;
rex::Runtime* g_runtime = nullptr;
std::shared_ptr<const simpsons::modkit::MeshVariants> g_variants;
std::shared_ptr<const simpsons::modkit::TextureVariants> g_textures;
std::set<std::string> g_errors;
std::set<uint32_t> g_image_keys;

bool Access(uint32_t address, uint32_t size, bool write = false) {
  if (!address || !size || uint64_t(address) + size > 0x100000000ull)
    return false;
  const uint64_t end = uint64_t(address) + size;
  for (uint64_t cursor = address; cursor < end;) {
    auto* heap = g_runtime->memory()->LookupHeap(uint32_t(cursor));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(cursor), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead) ||
        (write && !(info.protect & rex::memory::kMemoryProtectWrite)))
      return false;
    const auto offset = uint32_t(cursor) & (heap->page_size() - 1);
    if (info.region_size <= offset)
      return false;
    const uint64_t next = std::min({end, cursor + info.region_size - offset,
                                    uint64_t(info.allocation_base) + info.allocation_size});
    if (next <= cursor || (cursor < 0xE0000000ull && next > 0xE0000000ull))
      return false;
    cursor = next;
  }
  return true;
}
uint32_t Little(const uint8_t* base, uint32_t address) {
  const auto* p = simpsons::GuestToHost(base, address);
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool ImageKeyInUse(const uint8_t* base, uint32_t key) {
  // sub_826F8520 / sub_826F7F00: the image cache chains native textures
  // through EA2F +28 and compares their content keys at +4.
  constexpr uint32_t table_at = 0x82D62FB4, plugin_at = 0x82CF0600;
  if (!Access(table_at, 4) || !Access(plugin_at, 4))
    throw simpsons::modkit::Failure("invalid_texture", "Native image cache is unavailable");
  const auto table = simpsons::LoadGuestU32(base, table_at);
  if (!table)
    return false;
  if (!Access(table, 8) || simpsons::LoadGuestU32(base, plugin_at) != 88)
    throw simpsons::modkit::Failure("invalid_texture", "Native image cache layout is unsupported");
  const auto buckets = simpsons::LoadGuestU32(base, table),
             count = simpsons::LoadGuestU32(base, table + 4);
  const uint64_t bucket = uint64_t(buckets) + uint64_t(key % std::max(count, 1u)) * 4;
  if (!count || count > 65536 || bucket > UINT32_MAX || !Access(uint32_t(bucket), 4))
    throw simpsons::modkit::Failure("invalid_texture", "Native image cache buckets are invalid");
  auto texture = simpsons::LoadGuestU32(base, uint32_t(bucket));
  std::set<uint32_t> seen;
  while (texture) {
    if (seen.size() >= 256 || !seen.insert(texture).second || !Access(texture, 120))
      throw simpsons::modkit::Failure("invalid_texture", "Native image cache chain is invalid");
    if (simpsons::LoadGuestU32(base, texture + 92) == key)
      return true;
    texture = simpsons::LoadGuestU32(base, texture + 116);
  }
  return false;
}
}  // namespace

void Apply(uint8_t* base, uint32_t stream) {
  if (!g_active.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(g_mutex);
  if (!g_active.load(std::memory_order_relaxed) || !Access(stream, 24))
    return;
  try {
    const auto buffer = simpsons::LoadGuestU32(base, stream + 20),
               cursor = simpsons::LoadGuestU32(base, stream + 12);
    const uint64_t header = uint64_t(buffer) + cursor;
    if (header > UINT32_MAX || cursor < 12 || !Access(uint32_t(header), 8))
      return;
    const auto padded = simpsons::LoadGuestU32(base, uint32_t(header) + 4);
    const uint64_t guid = header + 8 + padded;
    if (!padded || padded > 256 || guid + 16 > 0x100000000ull || !Access(uint32_t(guid), 16))
      return;
    std::array<uint32_t, 4> words{};
    for (uint32_t i = 0; i < 4; ++i)
      words[i] = simpsons::LoadGuestU32(base, uint32_t(guid) + i * 4);
    const auto id = simpsons::modkit::ResourceId(words);
    const bool mesh = g_variants && g_variants->Contains(id);
    const bool texture = g_textures && g_textures->Contains(id);
    if (!mesh && !texture)
      return;
    if (mesh && texture)
      throw simpsons::modkit::Failure("invalid_resource",
                                      "One native identity cannot be both a mesh and a texture");
    const auto chunk = uint32_t(header) - 12;
    if (!Access(chunk, 12) || Little(base, chunk) != 0x716)
      return;
    const uint64_t bytes = uint64_t(Little(base, chunk + 4)) + 12;
    if (bytes < 64 || bytes > 16 * 1024 * 1024 || !Access(chunk, uint32_t(bytes), true))
      throw simpsons::modkit::Failure("invalid_mesh",
                                      "Selected asset is not in a complete writable input buffer");
    auto* destination = simpsons::GuestToHost(base, chunk);
    std::string owner;
    std::vector<uint8_t> edited;
    if (mesh) {
      auto result = g_variants->Apply({destination, size_t(bytes)});
      if (!result)
        return;
      owner = std::move(result->owner);
      edited = std::move(result->bytes);
    } else {
      auto result = g_textures->Apply({destination, size_t(bytes)});
      if (!result)
        return;
      owner = std::move(result->owner);
      edited = std::move(result->bytes);
      const simpsons::modkit::TextureResource source({destination, size_t(bytes)});
      auto word = [&](const std::vector<uint8_t>& data, uint32_t at) {
        return uint32_t(data.at(at)) << 24 | uint32_t(data.at(at + 1)) << 16 |
               uint32_t(data.at(at + 2)) << 8 | data.at(at + 3);
      };
      for (const auto& image : source.textures()) {
        const auto at = source.body_offset + image.record + 92;
        auto key = word(edited, at);
        if (key == word(source.data, at))
          continue;
        if (g_image_keys.size() >= 65536)
          throw simpsons::modkit::Failure("resource_limit",
                                          "Authored image cache identity limit reached");
        bool available = false;
        for (size_t attempt = 0; attempt < 128; ++attempt, ++key) {
          if (key && !g_image_keys.contains(key) && !ImageKeyInUse(base, key)) {
            available = true;
            break;
          }
        }
        if (!available)
          throw simpsons::modkit::Failure("invalid_texture", "No free native image cache identity");
        for (uint32_t i = 0; i < 4; ++i)
          edited.at(at + i) = uint8_t(key >> (24 - i * 8));
        g_image_keys.insert(key);
      }
    }
    // Commit after full source, layout, bounds and image-identity validation.
    // The original loader retains its input and creates the native resources.
    std::memcpy(destination, edited.data(), edited.size());
    if (mesh)
      REXLOG_INFO("[modkit-mesh] applied {} mesh {} ({} bytes)", owner, id, bytes);
    else
      REXLOG_INFO("[modkit-texture] applied {} texture {} ({} bytes)", owner, id, bytes);
  } catch (const std::exception& error) {
    const std::string message = error.what();
    if (g_errors.size() < 256 && g_errors.insert(message).second)
      REXLOG_WARN("[modkit-mesh] {}", message);
  }
}
void Init(rex::Runtime* runtime, std::shared_ptr<const simpsons::modkit::MeshVariants> variants,
          std::shared_ptr<const simpsons::modkit::TextureVariants> textures) {
  std::lock_guard lock(g_mutex);
  g_active.store(false, std::memory_order_release);
  g_runtime = runtime;
  g_variants = std::move(variants);
  g_textures = std::move(textures);
  g_errors.clear();
  g_image_keys.clear();
  g_active.store(
      runtime && ((g_variants && g_variants->size()) || (g_textures && g_textures->size())),
      std::memory_order_release);
}
void Shutdown() {
  g_active.store(false, std::memory_order_release);
  std::lock_guard lock(g_mutex);
  g_runtime = nullptr;
  g_variants.reset();
  g_textures.reset();
  g_errors.clear();
  g_image_keys.clear();
}
}  // namespace modkit_meshes
