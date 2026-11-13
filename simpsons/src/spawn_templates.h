#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

// Read-only views of the original world's definition index and stream owners.
// Nothing here allocates guest data, makes an entity or extends a stream's life.
namespace spawn_templates {
using Guid = std::array<uint32_t, 4>;
struct Memory {
  std::function<bool(uint32_t, uint32_t)> readable;
  std::function<uint32_t(uint32_t)> word;
  std::function<uint8_t(uint32_t)> byte;
  bool Has(uint32_t at, uint32_t bytes) const {
    return at && !(at & 3) && uint64_t(at) + bytes <= 0x100000000ull && readable(at, bytes);
  }
};

// A read-only batch may visit the same committed page thousands of times.
// Cache only pages whose complete 4 KiB range was readable; partial mappings
// keep the original exact-range guard. Discard the view before any guest call
// that can allocate, unload or restore memory.
inline Memory ReadOnlyPages(const Memory& source) {
  auto pages = std::make_shared<std::set<uint32_t>>();
  return {[readable = source.readable, pages](uint32_t at, uint32_t bytes) {
            const uint64_t end = uint64_t(at) + bytes;
            if (!at || !bytes || end > 0x100000000ull)
              return false;
            if (bytes > 1024 * 1024)
              return readable(at, bytes);
            for (uint64_t page = at & ~4095u; page < end; page += 4096) {
              if (pages->contains(uint32_t(page)))
                continue;
              if (!readable(uint32_t(page), 4096))
                return readable(at, bytes);
              pages->insert(uint32_t(page));
            }
            return true;
          }, source.word, source.byte};
}
struct Definition {
  uint32_t data = 0, hash = 0;
  Guid guid{};
};
inline uint32_t Hash(const Guid& guid) {
  uint32_t hash = 0;
  for (auto word : guid)
    for (int shift = 24; shift >= 0; shift -= 8)
      hash = hash * 65599 + ((word >> shift) & 255);
  return hash;
}
inline std::optional<Definition> Read(const Memory& memory, uint32_t at) {
  if (!memory.Has(at, 32) || !(memory.byte(at + 16) & 1))
    return {};
  // Compiled GUID offsets are signed and relative to their own field, not
  // the chunk. A record can refer backwards into the dictionary's GUID pool.
  const auto offset = int32_t(memory.word(at + 24));
  const int64_t target = int64_t(at) + 24 + offset;
  if (!offset || target <= 0 || target > UINT32_MAX || !memory.Has(uint32_t(target), 16))
    return {};
  Definition result{at, memory.word(at + 28)};
  for (uint32_t i = 0; i < result.guid.size(); ++i)
    result.guid[i] = memory.word(uint32_t(target) + i * 4);
  return result;
}
inline std::optional<Definition> Find(const Memory& memory, uint32_t world, const Guid& guid) {
  if (!memory.Has(world, 96))
    return {};
  const auto buckets = memory.word(world + 72), count = memory.word(world + 76);
  if (!count || count > 65536 || !memory.Has(buckets, count * 4))
    return {};
  auto node = memory.word(buckets + (Hash(guid) % count) * 4);
  std::set<uint32_t> visited;
  while (node) {
    if (visited.size() == 16384 || !visited.insert(node).second || !memory.Has(node, 32))
      return {};
    if (auto record = Read(memory, node); record && record->guid == guid)
      return record;
    node = memory.word(node + 12);
  }
  return {};
}
inline std::vector<Definition> List(const Memory& memory, uint32_t world) {
  if (!memory.Has(world, 96))
    return {};
  const auto buckets = memory.word(world + 72), count = memory.word(world + 76);
  if (!count || count > 65536 || !memory.Has(buckets, count * 4))
    return {};
  std::vector<Definition> result;
  std::set<uint32_t> visited;
  for (uint32_t bucket = 0; bucket < count; ++bucket) {
    auto node = memory.word(buckets + bucket * 4);
    while (node) {
      if (visited.size() == 16384 || !visited.insert(node).second || !memory.Has(node, 32))
        return {};
      if (auto record = Read(memory, node)) {
        if (Hash(record->guid) % count != bucket)
          return {};
        result.push_back(*record);
      }
      node = memory.word(node + 12);
    }
  }
  return result;
}
// Only fully loaded files lend templates. The key at +96 belongs to this
// particular load, so allocation reuse or reloading the same file expires it.
inline std::map<uint32_t, uint32_t> Owners(const Memory& memory, uint32_t manager) {
  if (!memory.Has(manager, 8736))
    return {};
  std::map<uint32_t, uint32_t> result;
  std::set<uint32_t> visited;
  for (auto offset : {8704u, 8712u, 8720u, 8728u}) {
    auto node = memory.word(manager + offset);
    while (node) {
      if (visited.size() == 1024 || !visited.insert(node).second || !memory.Has(node, 112))
        return {};
      if (memory.word(node + 100) == 4 && memory.word(node + 96)) {
        const auto prefix = memory.word(node + 108), key = memory.word(node + 96);
        const auto [entry, inserted] = result.emplace(prefix, key);
        if (!inserted && entry->second != key)
          entry->second = 0;  // ambiguous owner
      }
      node = memory.word(node);
    }
  }
  return result;
}

// Definitions are linked to a load, even when several files share namespace
// zero. Follow the world's actual layer lists instead of guessing that owner
// from the GUID prefix (sub_826A3350 and sub_826A2468).
inline std::map<uint32_t, uint32_t> DefinitionOwners(const Memory& memory, uint32_t world) {
  if (!memory.Has(world, 72))
    return {};
  std::map<uint32_t, uint32_t> result;
  std::set<uint32_t> layers, records;
  auto layer = memory.word(world + 56);
  uint32_t previous = 0;
  while (layer) {
    if (layers.size() == 1024 || !layers.insert(layer).second || !memory.Has(layer, 20) ||
        memory.word(layer + 4) != previous)
      return {};
    const auto key = memory.word(layer + 8);
    auto record = memory.word(layer + 16);
    auto link = layer + 16;
    while (record) {
      if (records.size() == 16384 || !records.insert(record).second || !memory.Has(record, 32) ||
          memory.word(record) != link)
        return {};
      if (key)
        result.emplace(record, key);
      link = record + 4;
      record = memory.word(link);
    }
    previous = layer;
    layer = memory.word(layer);
  }
  return memory.word(world + 60) == previous ? result : std::map<uint32_t, uint32_t>{};
}

struct File {
  uint32_t address = 0, key = 0, state = 0, prefix = 0;
};
inline std::vector<File> Files(const Memory& memory, uint32_t manager) {
  if (!memory.Has(manager, 8736))
    return {};
  std::vector<File> result;
  std::set<uint32_t> visited, keys;
  for (auto offset : {8704u, 8712u, 8720u, 8728u}) {
    auto node = memory.word(manager + offset);
    uint32_t previous = 0;
    while (node) {
      if (visited.size() == 1024 || !visited.insert(node).second || !memory.Has(node, 320) ||
          memory.word(node + 4) != previous)
        return {};
      File file{node, memory.word(node + 96), memory.word(node + 100), memory.word(node + 108)};
      if (!file.key || !keys.insert(file.key).second || file.state > 6)
        return {};
      result.push_back(file);
      previous = node;
      node = memory.word(node);
    }
    if (memory.word(manager + offset + 4) != previous)
      return {};
  }
  return result;
}
}  // namespace spawn_templates
