#include "text_table.h"
#include "localization.h"
#include "sha256.h"

#include <charconv>
#include <cstring>

namespace simpsons::modkit {
namespace {
uint32_t Big(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store(uint8_t* p, uint32_t value) {
  for (int i = 3; i >= 0; --i) {
    p[i] = uint8_t(value);
    value >>= 8;
  }
}
}  // namespace

GameTextTables ReadGameTextTables(const std::vector<Manifest>& manifests) {
  GameTextTables result;
  size_t budget = 0;
  for (const auto& mod : manifests) {
    if (!mod.source.contains("game_text"))
      continue;
    const auto data = ReadText(Inside(mod.folder, mod.source.at("game_text").get<std::string>()),
                               16 * 1024 * 1024);
    if (data.size() > 32 * 1024 * 1024 - budget)
      throw Failure("resource_limit", "Game translation catalogs exceed 32 MiB");
    budget += data.size();
    const auto catalog = Json::parse(data);
    if (!catalog.is_object() || catalog.value("version", Json()) != 1 ||
        !catalog.at("tables").is_object() || catalog.at("tables").size() > 4096)
      throw Failure("invalid_translation", "Invalid game text catalog");
    for (auto table = catalog.at("tables").begin(); table != catalog.at("tables").end(); ++table) {
      const auto& digest = table.key();
      if (digest.size() != 64 ||
          digest.find_first_not_of("0123456789abcdef") != std::string::npos ||
          !table.value().is_object() || table.value().size() > 20000)
        throw Failure("invalid_translation",
                      "A game text table needs its source SHA-256 and text IDs");
      for (auto entry = table.value().begin(); entry != table.value().end(); ++entry) {
        uint32_t key;
        const auto parsed =
            std::from_chars(entry.key().data(), entry.key().data() + entry.key().size(), key, 16);
        if (entry.key().size() != 8 || parsed.ec != std::errc() ||
            parsed.ptr != entry.key().data() + entry.key().size())
          throw Failure("invalid_translation", "Game text IDs need eight hexadecimal digits");
        GameTextLine line{entry.value().at("source").get<std::string>(),
                          entry.value().at("text").get<std::string>()};
        if (line.source.size() > 65536 || line.text.size() > 65536)
          throw Failure("resource_limit", "Game text lines are limited to 64 KiB");
        ValidateTranslation(line.source, line.text);
        MenuText(line.source);
        MenuText(line.text);
        result[digest][key] = std::move(line);
      }
    }
  }
  return result;
}

std::string RelocatedTextTableHash(std::span<const uint8_t> data, uint32_t address) {
  if (data.size() < 32 || data.size() > 4 * 1024 * 1024 || std::memcmp(data.data(), "2HCL", 4) ||
      Big(data.data() + 4) != data.size())
    throw Failure("invalid_translation", "Invalid loaded text table");
  const auto count = Big(data.data() + 16), columns = Big(data.data() + 20);
  const uint64_t entries = uint64_t(count) * columns;
  const uint64_t start = 32 + uint64_t(count) * 4;
  if (columns < 1 || columns > 2 || start + entries * 4 > data.size() || Big(data.data() + 12))
    throw Failure("invalid_translation", "Unsupported loaded text table layout");
  std::vector<uint8_t> normalized(data.begin(), data.end());
  Store(normalized.data() + 24, 0);
  Store(normalized.data() + 28, 0);
  for (uint64_t i = 0; i < entries; ++i) {
    auto* field = normalized.data() + start + i * 4;
    const uint32_t pointer = Big(field), offset = pointer - address;
    if (offset < start + entries * 4 || offset >= data.size())
      throw Failure("invalid_translation", "A loaded text pointer is outside its table");
    Store(field, offset);
  }
  sha256::SHA256 hash;
  return hash(normalized.data(), normalized.size());
}

}  // namespace simpsons::modkit
