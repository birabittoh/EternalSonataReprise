#include "ui_text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

#include "lang.generated.h"
#include "settings.h"
#include "ui_language.h"

namespace eternalsonata {
namespace {

// IntroText order.
constexpr std::array<const char*, size_t(IntroText::kCount)> kKeys = {
    "start",         "extract",       "select_file",      "select_folder",
    "quit",          "language",      "ready",           "ready_release",
    "found",         "found_release", "press_extract",   "need_files",
    "select_iso_only", "copy_hint",
    "selected",      "title",         "unsupported_language",
    "delete_files",  "cancel",        "confirm_delete",
    "not_enough_space",
};
constexpr std::array<const char*, kInstallPhaseCount> kPhaseKeys = {
    "phase_extract", "phase_convert", "phase_patch"};
constexpr std::array<const char*, 5> kProgressKeys = {
    "progress_extracting", "progress_unpacking", "progress_converting", "progress_patching",
    "progress_deleting"};

using StringTable = std::map<std::string, std::string, std::less<>>;

struct Language {
  std::string code;
  std::string name;
  toml::table tables;
  // What mods set, by section, over `tables`.
  std::map<std::string, StringTable, std::less<>> overrides;
};

struct PendingString {
  std::string language, section, key, value;
};

std::vector<PendingString> g_pending;

std::vector<Language> g_languages;
int g_current = -1;
std::atomic<uint32_t> g_current_id{0};
std::filesystem::path g_user_settings_path;

std::vector<Language>& Languages() {
  if (!g_languages.empty())
    return g_languages;
  for (const LangFile& file : kLangFiles) {
    try {
      toml::table table = toml::parse(std::string_view(file.text, file.size));
      std::string name = table["name"].value_or(std::string(file.code));
      g_languages.push_back({file.code, std::move(name), std::move(table)});
    } catch (const toml::parse_error& e) {
      REXLOG_ERROR("res/lang/{}.toml: {}", file.code, e.description());
    }
  }
  return g_languages;
}

int Find(std::string_view code) {
  const auto& languages = Languages();
  for (size_t i = 0; i < languages.size(); ++i) {
    if (languages[i].code == code)
      return int(i);
  }
  return -1;
}

// A mod language is "mod:<XLanguage id>", which no res/lang file is named.
std::string NormalizeCode(std::string_view language) {
  const bool numeric = !language.empty() &&
                       std::all_of(language.begin(), language.end(),
                                   [](char c) { return c >= '0' && c <= '9'; });
  return numeric ? "mod:" + std::string(language) : std::string(language);
}

// The user_language id of a built-in code, or a mod language's own id.
std::string XLanguageOfCode(std::string_view code) {
  static constexpr std::array<std::pair<std::string_view, const char*>, 6> kIds = {{
      {"en", "1"}, {"ja", "2"}, {"de", "3"}, {"fr", "4"}, {"es", "5"}, {"it", "6"}}};
  for (const auto& [name, id] : kIds)
    if (code == name)
      return id;
  return code.rfind("mod:", 0) == 0 ? std::string(code.substr(4)) : std::string();
}

const char* FromXLanguage(uint32_t id) {
  switch (id) {
    case 2: return "ja";
    case 3: return "de";
    case 4: return "fr";
    case 5: return "es";
    case 6: return "it";
    default: return "en";
  }
}

int FromSystem() {
  int count = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&count);
  if (!locales)
    return -1;
  int found = -1;
  for (int i = 0; i < count && found < 0; ++i)
    found = Find(locales[i]->language);
  SDL_free(locales);
  return found;
}

// The interface follows the text language, so one choice drives both.
int Current() {
  const uint32_t id = uint32_t(std::strtoul(SelectedLanguageId().c_str(), nullptr, 10));
  if (g_current >= 0 && id == g_current_id.load())
    return g_current;
  g_current = Find(NormalizeCode(UiLanguageOfXLanguage(id)));
  if (g_current < 0)
    g_current = std::max(Find("en"), 0);
  g_current_id = id;
  return g_current;
}

// The current language's text for `key`, else English's, else null.
const char* Find(const char* section, const char* key) {
  const auto& languages = Languages();
  if (languages.empty())
    return nullptr;
  static const int english = Find("en");
  for (int index : {Current(), english}) {
    if (index < 0)
      continue;
    const auto& overrides = languages[size_t(index)].overrides;
    if (const auto section_it = overrides.find(section); section_it != overrides.end()) {
      if (const auto text = section_it->second.find(key); text != section_it->second.end())
        return text->second.c_str();
    }
    const toml::table* table = languages[size_t(index)].tables[section].as_table();
    if (const auto* text = table ? (*table)[key].as_string() : nullptr)
      return text->get().c_str();
  }
  return nullptr;
}

const char* Lookup(const char* key) {
  const char* text = Find("intro", key);
  return text ? text : key;
}

}  // namespace

const char* Tr(const char* section, const char* key) {
  const char* text = Find(section, key);
  return text ? text : key;
}

const char* TrOr(const char* section, const char* key, const char* fallback) {
  const char* text = Find(section, key);
  return text ? text : fallback;
}

std::string Tr(const char* section, const char* key, std::string_view arg) {
  std::string text = Tr(section, key);
  if (const size_t at = text.find("{}"); at != std::string::npos)
    text.replace(at, 2, arg);
  return text;
}

const char* Tr(IntroText id) {
  return Lookup(kKeys[size_t(id)]);
}

std::string Tr(IntroText id, std::string_view arg) {
  std::string text = Tr(id);
  if (const size_t at = text.find("{}"); at != std::string::npos)
    text.replace(at, 2, arg);
  return text;
}

const char* TrPhase(Phase phase) {
  return Lookup(kPhaseKeys[size_t(phase)]);
}

std::string TrProgress(const std::string& title) {
  const int english = Find("en");
  if (english < 0)
    return title;
  const toml::table* intro = Languages()[size_t(english)].tables["intro"].as_table();
  if (!intro)
    return title;
  for (const char* key : kProgressKeys) {
    if ((*intro)[key].value_or(std::string()) == title)
      return Lookup(key);
  }
  return title;
}

int IntroLanguageCount() {
  return int(Languages().size());
}

std::string UiLanguageOfXLanguage(uint32_t id) {
  return id >= 1 && id <= 6 ? FromXLanguage(id) : std::to_string(id);
}

void AddUiString(std::string_view language, std::string_view section, std::string_view key,
                 std::string_view value) {
  if (language.empty() || section.empty() || key.empty() || value.empty()) {
    REXLOG_WARN("[ui_text] ignoring an interface string with an empty field ({}.{} in '{}')",
                section, key, language);
    return;
  }
  g_pending.push_back({NormalizeCode(language), std::string(section), std::string(key),
                       std::string(value)});
}

void ApplyModUiStrings() {
  const auto options = GetLanguageOptions();
  auto& languages = Languages();
  // Every mod language is pickable, with or without interface text of its own.
  for (const auto& option : options) {
    const std::string code =
        NormalizeCode(UiLanguageOfXLanguage(uint32_t(std::strtoul(option.id, nullptr, 10))));
    if (code.rfind("mod:", 0) == 0 && Find(code) < 0)
      languages.push_back({code, option.label, {}, {}});
  }
  if (g_pending.empty()) {
    g_current = -1;
    return;
  }
  for (PendingString& pending : g_pending) {
    int index = Find(pending.language);
    if (index < 0 && pending.language.rfind("mod:", 0) == 0) {
      const std::string_view id = std::string_view(pending.language).substr(4);
      const auto option = std::find_if(options.begin(), options.end(), [&](const auto& entry) {
        return id == entry.id;
      });
      if (option != options.end()) {
        languages.push_back({pending.language, option->label, {}, {}});
        index = int(languages.size()) - 1;
      }
    }
    if (index < 0) {
      REXLOG_WARN("[ui_text] no interface language '{}' for {}.{}", pending.language,
                  pending.section, pending.key);
      continue;
    }
    // Mods arrive in priority order, so the first to set a string keeps it.
    languages[size_t(index)].overrides[pending.section].emplace(std::move(pending.key),
                                                                 std::move(pending.value));
  }
  g_pending.clear();
  // A saved choice may name a language that only exists now.
  g_current = -1;
  InvalidateUiTextLanguage();
}

const char* IntroLanguageName(int index) {
  return Languages()[size_t(index)].name.c_str();
}

const char* IntroLanguageCodeAt(int index) {
  return Languages()[size_t(index)].code.c_str();
}

int IntroLanguage() {
  return Current();
}

void SetIntroLanguage(int index) {
  if (index < 0 || index >= IntroLanguageCount() || index == Current())
    return;
  const std::string id = XLanguageOfCode(Languages()[size_t(index)].code);
  auto* entry = rex::cvar::GetFlagInfo("user_language");
  if (id.empty() || !entry || !entry->setter || !entry->setter(id))
    return;
  g_current = -1;
  InvalidateUiTextLanguage();
  // The config file is read-only; settings.toml is where user choices live.
  if (!g_user_settings_path.empty())
    rex::cvar::SaveConfigSubset(g_user_settings_path, {"user_language"});
}

void BindIntroLanguageConfig(const std::filesystem::path& user_settings_path) {
  g_user_settings_path = user_settings_path;
  // No language chosen yet: start in the system's, text included.
  if (rex::cvar::GetFlagSource("user_language") != rex::cvar::Source::kDefault)
    return;
  const int system = FromSystem();
  auto* entry = rex::cvar::GetFlagInfo("user_language");
  if (system < 0 || !entry || !entry->setter)
    return;
  if (const std::string id = XLanguageOfCode(Languages()[size_t(system)].code); !id.empty())
    entry->setter(id);
}

std::string IntroGlyphText() {
  std::string text;
  for (const LangFile& file : kLangFiles)
    text.append(file.text, file.size);
  return text;
}

}  // namespace eternalsonata
