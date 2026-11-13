#include "entity_assets.h"

#include "guest_memory.h"
#include "spawner.h"
#include "resource_id.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace entity_assets {
namespace {
// sub_82711460: lis -32041 / lwz -13604. The table stores 48-byte
// resource entries; sub_82710EB0 follows +44 and compares all four GUID words.
constexpr uint32_t kResources = 0x82D6CADC;
constexpr uint32_t kMetaModel = 0x820B7030;
struct LoadedName {
  uint32_t payload = 0;
  std::string name;
  bool ambiguous = false;
};
std::mutex g_names_mutex;
std::map<std::string, LoadedName> g_names;
constexpr size_t kMaxNames = 4096;

bool Readable(uint32_t at, uint32_t bytes) {
  return at && !(at & 3) && spawner::Readable(at, bytes);
}

uint32_t Resource(const uint8_t* base, const Model& model) {
  if (!Readable(kResources, 4))
    return 0;
  const auto registry = simpsons::LoadGuestU32(base, kResources);
  if (!Readable(registry, 8))
    return 0;
  const auto buckets = simpsons::LoadGuestU32(base, registry);
  const auto count = simpsons::LoadGuestU32(base, registry + 4);
  if (!count || count > 65536)
    return 0;
  // sub_82745390 hashes the GUID's sixteen big-endian bytes with 65599.
  uint32_t hash = 0;
  for (const auto word : model.guid)
    for (int shift = 24; shift >= 0; shift -= 8)
      hash = hash * 65599u + ((word >> shift) & 255u);
  const uint64_t bucket = uint64_t(buckets) + (hash % count) * 4;
  if (bucket > UINT32_MAX || !Readable(uint32_t(bucket), 4))
    return 0;
  auto entry = simpsons::LoadGuestU32(base, uint32_t(bucket));
  for (size_t visited = 0; entry && visited < 256; ++visited) {
    if (!Readable(entry, 48))
      return 0;
    bool matches = true;
    for (size_t i = 0; i < model.guid.size(); ++i)
      if (simpsons::LoadGuestU32(base, entry + 28 + uint32_t(i) * 4) != model.guid[i])
        matches = false;
    if (matches)
      return entry;
    entry = simpsons::LoadGuestU32(base, entry + 44);
  }
  return 0;
}
}  // namespace

std::string Model::Id() const {
  return simpsons::modkit::ResourceId(guid);
}

std::string ReadName(const uint8_t* base, uint32_t address, size_t limit) {
  if (!base || !address || !limit || limit > 1024)
    return {};
  std::string result;
  while (result.size() < limit) {
    const uint64_t cursor = uint64_t(address) + result.size();
    if (cursor > UINT32_MAX)
      return {};
    size_t count = std::min<size_t>(32, limit - result.size());
    if (!spawner::Readable(uint32_t(cursor), uint32_t(count)))
      count = 1;
    if (!spawner::Readable(uint32_t(cursor), uint32_t(count)))
      return {};
    const auto* text = simpsons::GuestToHost(base, uint32_t(cursor));
    for (size_t i = 0; i < count; ++i) {
      if (!text[i])
        return result;
      if (text[i] < 32 || text[i] > 126)
        return {};
      result += char(text[i]);
    }
  }
  return {};
}

std::optional<Model> ReadStreamName(const uint8_t* base, uint32_t stream) {
  if (!base || !Readable(stream, 24))
    return {};
  // Before sub_82711B28 reads its header length, the stream's +12 cursor
  // addresses that word inside the +20 memory buffer. Names follow the next
  // word (padded byte length), and the GUID follows that padded name.
  const uint64_t header = uint64_t(simpsons::LoadGuestU32(base, stream + 20)) +
                          simpsons::LoadGuestU32(base, stream + 12);
  if (header > UINT32_MAX || !Readable(uint32_t(header), 8))
    return {};
  const auto header_size = simpsons::LoadGuestU32(base, uint32_t(header));
  const auto padded = simpsons::LoadGuestU32(base, uint32_t(header) + 4);
  const auto guid = header + 8 + padded;
  if (!padded || padded > 256 || header_size > 4096 || guid + 20 > header + 4 + header_size ||
      guid + 20 > 0x100000000ull || !Readable(uint32_t(guid), 16))
    return {};
  auto name = ReadName(base, uint32_t(header) + 8, padded);
  if (!name.ends_with(".meta.xml"))
    return {};
  Model result;
  for (size_t i = 0; i < result.guid.size(); ++i)
    result.guid[i] = simpsons::LoadGuestU32(base, uint32_t(guid) + uint32_t(i) * 4);
  if (result.Id().empty())
    return {};
  result.name = std::move(name);
  return result;
}

void Loaded(const uint8_t* base, const Model& pending) {
  const auto id = pending.Id();
  if (!base || pending.name.empty() || id.empty())
    return;
  const auto entry = Resource(base, pending);
  if (!entry)
    return;
  const auto payload = simpsons::LoadGuestU32(base, entry + 8);
  if (!Readable(payload, 32) || simpsons::LoadGuestU32(base, payload) != 0x4D4D646C ||
      simpsons::LoadGuestU32(base, payload + 4) != 10)
    return;
  for (size_t i = 0; i < pending.guid.size(); ++i)
    if (simpsons::LoadGuestU32(base, payload + 16 + uint32_t(i) * 4) != pending.guid[i])
      return;
  std::lock_guard lock(g_names_mutex);
  auto found = g_names.find(id);
  if (found != g_names.end() && found->second.payload == payload) {
    // Reused resources can be named again. Conflicting names are ambiguous,
    // never a reason to relabel an existing native allocation.
    if (found->second.name != pending.name)
      found->second.ambiguous = true;
    return;
  }
  if (found == g_names.end() && g_names.size() == kMaxNames)
    g_names.erase(g_names.begin());
  g_names[id] = {payload, pending.name, false};
}

void Reset() {
  std::lock_guard lock(g_names_mutex);
  g_names.clear();
}

std::optional<Model> ModelOf(const uint8_t* base, uint32_t entity) {
  if (!base || !Readable(entity, 140))
    return {};
  const auto model = simpsons::LoadGuestU32(base, entity + 136);
  if (!Readable(model, 20) || simpsons::LoadGuestU32(base, model) != kMetaModel)
    return {};
  const auto prototype = simpsons::LoadGuestU32(base, model + 16);
  // sub_826A8760 retains this relocated MMdl v10 resource. Other model
  // classes and versions need their own reviewed identity adapter.
  if (!Readable(prototype, 84) || simpsons::LoadGuestU32(base, prototype) != 0x4D4D646C ||
      simpsons::LoadGuestU32(base, prototype + 4) != 10)
    return {};
  Model result;
  bool nonzero = false;
  for (size_t i = 0; i < result.guid.size(); ++i) {
    result.guid[i] = simpsons::LoadGuestU32(base, prototype + 16 + uint32_t(i) * 4);
    nonzero |= result.guid[i] != 0;
  }
  if (!nonzero)
    return {};
  const auto entry = Resource(base, result);
  // A GUID match alone must not label a different or replaced allocation.
  if (entry && simpsons::LoadGuestU32(base, entry + 8) == prototype) {
    result.name = ReadName(base, simpsons::LoadGuestU32(base, entry));
    if (result.name.empty()) {
      std::lock_guard lock(g_names_mutex);
      const auto found = g_names.find(result.Id());
      if (found != g_names.end() && found->second.payload == prototype && !found->second.ambiguous)
        result.name = found->second.name;
    }
  }
  return result;
}
}  // namespace entity_assets
