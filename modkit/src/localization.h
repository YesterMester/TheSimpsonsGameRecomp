#pragma once

#include "runtime.h"

namespace simpsons::modkit {

std::string NormalizeLocale(std::string_view locale);
// Original menu fonts consume Windows-1252 bytes, while mod source and JSON
// use UTF-8. Reject unsupported glyphs before a page enters the native UI.
std::string MenuText(std::string_view utf8);
void ValidateTranslation(std::string_view source, std::string_view translated);

// Immutable after launch, so a text lookup never reads files on the game
// thread. Keys belong to a mod, even when a separate language pack adds text.
class Localizations {
 public:
  explicit Localizations(const std::vector<Manifest>& manifests);
  std::string Text(std::string_view owner, std::string_view locale, std::string_view key,
                   const Json& values = Json::object()) const;

 private:
  struct Catalogs {
    std::string fallback = "en";
    std::map<std::string, std::map<std::string, std::string>> locales;
  };
  std::map<std::string, Catalogs> mods_;
};

}  // namespace simpsons::modkit
