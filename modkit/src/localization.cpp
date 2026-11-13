#include "localization.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace simpsons::modkit {
namespace {
bool Letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool Digit(char c) {
  return c >= '0' && c <= '9';
}
bool Identifier(std::string_view value) {
  return !value.empty() && (Letter(value[0]) || value[0] == '_') &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return Letter(c) || Digit(c) || c == '_' || c == '.' || c == '-';
         });
}
std::multiset<std::string> Tokens(std::string_view text) {
  std::multiset<std::string> result;
  for (size_t i = 0; i < text.size(); ++i) {
    const size_t start = i;
    if (text[i] == '[') {
      const auto end = text.find_first_of("]\r\n", i + 1);
      if (end != std::string_view::npos && text[end] == ']')
        i = end;
    } else if (text[i] == '{' && (!i || text[i - 1] != '{')) {
      const auto end = text.find('}', i + 1);
      if (end != std::string_view::npos && (end + 1 == text.size() || text[end + 1] != '}') &&
          Identifier(text.substr(i + 1, end - i - 1)))
        i = end;
    } else if (text[i] == '%') {
      size_t end = i + 1;
      if (end < text.size() && text[end] == '%') {
        result.emplace("%%");
        ++i;
        continue;
      }
      while (end < text.size() && Digit(text[end]))
        ++end;
      const size_t digits = end;
      if (end < text.size() && end > i + 1 && text[end] == '$')
        ++end;
      else
        end = i + 1;
      while (end < text.size() &&
             std::string_view("-+ #0").find(text[end]) != std::string_view::npos)
        ++end;
      while (end < text.size() && Digit(text[end]))
        ++end;
      if (end < text.size() && text[end] == '.') {
        ++end;
        while (end < text.size() && Digit(text[end]))
          ++end;
      }
      if (end < text.size() &&
          std::string_view("sdifuxXc").find(text[end]) != std::string_view::npos)
        i = end;
      else if (digits > i + 1)
        i = digits - 1;
    }
    if (i != start)
      result.emplace(text.substr(start, i - start + 1));
  }
  return result;
}
std::map<std::string, std::string> ReadCatalog(const Manifest& mod, const Json& path,
                                               size_t& budget) {
  if (!path.is_string())
    throw Failure("invalid_translation", "A locale catalog path must be text");
  const auto text = ReadText(Inside(mod.folder, path.get<std::string>()), 4 * 1024 * 1024);
  if (text.size() > 32 * 1024 * 1024 - budget)
    throw Failure("resource_limit", "Mod locale catalogs exceed 32 MiB");
  budget += text.size();
  const auto data = Json::parse(text);
  if (!data.is_object() || data.size() > 20000)
    throw Failure("invalid_translation",
                  "A locale catalog is an object with at most 20000 strings");
  std::map<std::string, std::string> result;
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (it.key().empty() || it.key().size() > 160 || it.key().find('\0') != std::string::npos ||
        !it.value().is_string())
      throw Failure("invalid_translation", "Locale catalogs need text keys and strings");
    auto value = it.value().get<std::string>();
    if (value.size() > 65536 || value.find('\0') != std::string::npos)
      throw Failure("invalid_translation", "A locale string cannot contain NUL or exceed 64 KiB");
    result.emplace(it.key(), std::move(value));
  }
  return result;
}
void Validate(const std::map<std::string, std::string>& source,
              const std::map<std::string, std::string>& translated) {
  for (const auto& [key, value] : translated) {
    const auto original = source.find(key);
    if (original == source.end())
      throw Failure("invalid_translation", "Unknown translation key: " + key);
    if (Tokens(original->second) != Tokens(value))
      throw Failure("invalid_translation",
                    "Keep button tags and placeholders in translation: " + key);
  }
}
}  // namespace

std::string NormalizeLocale(std::string_view locale) {
  if (locale.empty() || locale.size() > 35)
    throw Failure("invalid_locale", "Use a language code such as fr or pt-BR");
  std::string result;
  size_t part = 0, parts = 1;
  for (char c : locale) {
    if (c == '-' || c == '_') {
      if ((parts == 1 && part < 2) || !part || ++parts > 4)
        throw Failure("invalid_locale", "Invalid language code");
      part = 0;
      result += '-';
    } else {
      if ((!Letter(c) && (parts == 1 || !Digit(c))) || ++part > 8)
        throw Failure("invalid_locale", "Invalid language code");
      result += c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }
  }
  if (!part || (parts == 1 && part < 2))
    throw Failure("invalid_locale", "Invalid language code");
  return result;
}
void ValidateTranslation(std::string_view source, std::string_view translated) {
  if (Tokens(source) != Tokens(translated))
    throw Failure("invalid_translation", "Keep button tags and placeholders unchanged in translations");
}

std::string MenuText(std::string_view text) {
  constexpr uint32_t punctuation[] = {
      0x20AC, 0x81,   0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021, 0x2C6,  0x2030, 0x160,
      0x2039, 0x152,  0x8D,   0x17D,  0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x2DC,  0x2122, 0x161,  0x203A, 0x153,  0x9D,   0x17E,  0x178};
  std::string result;
  for (size_t i = 0; i < text.size();) {
    const auto first = uint8_t(text[i++]);
    uint32_t point = first;
    unsigned following = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      point &= 0x1F;
      following = 1;
    } else if (first >= 0xE0 && first <= 0xEF) {
      point &= 0x0F;
      following = 2;
    } else if (first >= 0xF0 && first <= 0xF4) {
      point &= 7;
      following = 3;
    } else if (first >= 0x80)
      throw Failure("invalid_text", "Menu text must be UTF-8");
    for (unsigned n = 0; n < following; ++n) {
      if (i == text.size() || (uint8_t(text[i]) & 0xC0) != 0x80)
        throw Failure("invalid_text", "Menu text must be UTF-8");
      point = (point << 6) | (uint8_t(text[i++]) & 0x3F);
    }
    if (!point || (following == 1 && point < 0x80) || (following == 2 && point < 0x800) ||
        (following == 3 && point < 0x10000) || point > 0x10FFFF ||
        (point >= 0xD800 && point <= 0xDFFF))
      throw Failure("invalid_text", "Menu text has an invalid Unicode character");
    if (point < 0x80 || (point >= 0xA0 && point <= 0xFF))
      result += char(point);
    else {
      const auto found = std::find(std::begin(punctuation), std::end(punctuation), point);
      if (found == std::end(punctuation))
        throw Failure("unsupported_font",
                      "The original menu font cannot display this character; a compatible font "
                      "import is required");
      result += char(0x80 + (found - std::begin(punctuation)));
    }
  }
  return result;
}

Localizations::Localizations(const std::vector<Manifest>& manifests) {
  size_t budget = 0;
  for (const auto& mod : manifests) {
    auto& target = mods_[mod.id];
    target.fallback = NormalizeLocale(mod.source.value("default_locale", "en"));
    if (!mod.source.contains("locales"))
      continue;
    const auto& locales = mod.source.at("locales");
    if (!locales.is_object() || locales.size() > 64)
      throw Failure("invalid_translation", "locales must map language codes to catalog paths");
    for (auto it = locales.begin(); it != locales.end(); ++it) {
      auto language = NormalizeLocale(it.key());
      if (target.locales.contains(language))
        throw Failure("invalid_translation", "Duplicate normalized locale");
      target.locales.emplace(std::move(language), ReadCatalog(mod, it.value(), budget));
    }
    const auto source = target.locales.find(target.fallback);
    if (source == target.locales.end())
      throw Failure("invalid_translation", "The default locale needs a source catalog");
    for (const auto& [language, catalog] : target.locales)
      Validate(source->second, catalog);
  }
  // Dependency order is also increasing asset priority. Language packs depend
  // on the mod they translate and override only their own translated keys.
  for (const auto& mod : manifests) {
    if (!mod.source.contains("translations"))
      continue;
    const auto& translations = mod.source.at("translations");
    if (!translations.is_object() || translations.size() > 64)
      throw Failure("invalid_translation", "translations must map mod IDs to locale catalogs");
    for (auto it = translations.begin(); it != translations.end(); ++it) {
      const auto found = mods_.find(it.key());
      if (!mod.dependencies.contains(it.key()) || found == mods_.end())
        throw Failure("invalid_translation",
                      "A language pack must depend on the mod it translates");
      auto& target = found->second;
      if (!target.locales.contains(target.fallback))
        throw Failure("invalid_translation", "The translated mod has no source catalog");
      if (!it.value().is_object() || it.value().size() > 64)
        throw Failure("invalid_translation", "A language pack needs locale catalog paths");
      std::set<std::string> normalized;
      for (auto locale = it.value().begin(); locale != it.value().end(); ++locale) {
        const auto language = NormalizeLocale(locale.key());
        if (!normalized.insert(language).second)
          throw Failure("invalid_translation", "Duplicate normalized locale");
        const auto catalog = ReadCatalog(mod, locale.value(), budget);
        Validate(target.locales.at(target.fallback), catalog);
        auto& destination = target.locales[language];
        for (const auto& [key, value] : catalog)
          destination[key] = value;
      }
    }
  }
}

std::string Localizations::Text(std::string_view owner, std::string_view locale,
                                std::string_view key, const Json& values) const {
  if (key.empty() || key.size() > 160 || key.find('\0') != std::string_view::npos)
    throw Failure("invalid_argument", "Text keys need 1 to 160 bytes");
  std::string text(key);
  const auto mod = mods_.find(std::string(owner));
  if (mod != mods_.end()) {
    auto language = NormalizeLocale(locale);
    std::vector<std::string> candidates{language};
    for (auto dash = language.rfind('-'); dash != std::string::npos; dash = language.rfind('-')) {
      language.resize(dash);
      candidates.push_back(language);
    }
    candidates.push_back(mod->second.fallback);
    for (const auto& candidate : candidates) {
      const auto catalog = mod->second.locales.find(candidate);
      if (catalog == mod->second.locales.end())
        continue;
      const auto entry = catalog->second.find(std::string(key));
      if (entry != catalog->second.end()) {
        text = entry->second;
        break;
      }
    }
  }
  if (!values.is_object())
    throw Failure("invalid_argument", "Text format values must be an object");
  std::string result;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '{' && (!i || text[i - 1] != '{')) {
      const auto end = text.find('}', i + 1);
      if (end != std::string::npos && (end + 1 == text.size() || text[end + 1] != '}') &&
          Identifier(std::string_view(text).substr(i + 1, end - i - 1))) {
        const auto name = text.substr(i + 1, end - i - 1);
        const auto value = values.find(name);
        if (value == values.end())
          throw Failure("invalid_argument", "Missing translation format value: " + name);
        if (!value->is_string() && !value->is_number() && !value->is_boolean())
          throw Failure("invalid_argument", "Text format values are strings, numbers or booleans");
        result += value->is_string() ? value->get<std::string>() : value->dump();
        i = end + 1;
      } else
        result += text[i++];
    } else
      result += text[i++];
    if (result.size() > 65536)
      throw Failure("resource_limit", "Formatted text exceeds 64 KiB");
  }
  return result;
}

}  // namespace simpsons::modkit
