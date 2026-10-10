#include "ui_text.h"

#include <algorithm>
#include <array>
#include <cstring>
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

REXCVAR_DEFINE_STRING(ui_language, "", "Eternal Sonata",
                      "Interface language, by res/lang file name; empty follows the system");

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
};
constexpr std::array<const char*, kInstallPhaseCount> kPhaseKeys = {
    "phase_extract", "phase_convert", "phase_patch"};
constexpr std::array<const char*, 5> kProgressKeys = {
    "progress_extracting", "progress_unpacking", "progress_converting", "progress_patching",
    "progress_deleting"};

struct Language {
  std::string code;
  std::string name;
  toml::table tables;
};

std::vector<Language> g_languages;
int g_current = -1;
int g_launch = -1;
bool g_confirmed = false;
std::filesystem::path g_config_path;

const std::vector<Language>& Languages() {
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

int Current() {
  if (g_current >= 0)
    return g_current;
  g_current = Find(REXCVAR_GET(ui_language));
  // A text language the player chose, English included, over the system's.
  if (g_current < 0 && (REXCVAR_GET(user_language) != 1 ||
                        rex::cvar::GetFlagSource("user_language") != rex::cvar::Source::kDefault))
    g_current = Find(FromXLanguage(REXCVAR_GET(user_language)));
  if (g_current < 0)
    g_current = FromSystem();
  if (g_current < 0)
    g_current = std::max(Find("en"), 0);
  if (g_launch < 0)
    g_launch = g_current;
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
  const std::string previous = Languages()[size_t(Current())].code;
  g_current = index;
  REXCVAR_SET(ui_language, Languages()[size_t(index)].code);
  InvalidateUiTextLanguage();
  if (!g_config_path.empty())
    rex::cvar::SaveConfigSubset(g_config_path, {"ui_language"});
  InterfaceLanguageChanged(previous.c_str());
}

const char* IntroLanguageCode() {
  const auto& languages = Languages();
  return languages.empty() ? "en" : languages[size_t(Current())].code.c_str();
}

const char* LaunchIntroLanguageCode() {
  const auto& languages = Languages();
  if (languages.empty())
    return "en";
  Current();
  return languages[size_t(g_launch)].code.c_str();
}

void ConfirmIntroLanguage() {
  g_confirmed = true;
}

bool IntroLanguageConfirmed() {
  return g_confirmed;
}

void BindIntroLanguageConfig(const std::filesystem::path& config_path) {
  g_config_path = config_path;
}

std::string IntroGlyphText() {
  std::string text;
  for (const LangFile& file : kLangFiles)
    text.append(file.text, file.size);
  return text;
}

}  // namespace eternalsonata
