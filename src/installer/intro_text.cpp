#include "intro_text.h"

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

REXCVAR_DEFINE_STRING(ui_language, "", "Eternal Sonata",
                      "Start screen language, by res/lang file name; empty follows the system");

namespace eternalsonata {
namespace {

// IntroText order.
constexpr std::array<const char*, size_t(IntroText::kCount)> kKeys = {
    "start",         "extract",       "select_iso",      "select_folder",
    "quit",          "language",      "ready",           "ready_release",
    "found",         "found_release", "press_extract",   "need_files",
    "select_iso_or_folder",           "select_iso_only", "copy_hint",
    "selected",      "title",
};
constexpr std::array<const char*, kInstallPhaseCount> kPhaseKeys = {
    "phase_extract", "phase_convert", "phase_patch"};
constexpr std::array<const char*, 4> kProgressKeys = {
    "progress_extracting", "progress_unpacking", "progress_converting", "progress_patching"};

struct Language {
  std::string code;
  std::string name;
  toml::table intro;
};

std::vector<Language> g_languages;
int g_current = -1;
std::filesystem::path g_config_path;

const std::vector<Language>& Languages() {
  if (!g_languages.empty())
    return g_languages;
  for (const LangFile& file : kLangFiles) {
    try {
      toml::table table = toml::parse(std::string_view(file.text, file.size));
      Language language{file.code, table["name"].value_or(std::string(file.code)), {}};
      if (const toml::table* intro = table["intro"].as_table())
        language.intro = *intro;
      g_languages.push_back(std::move(language));
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
  if (g_current < 0 && REXCVAR_GET(user_language) != 1)
    g_current = Find(FromXLanguage(REXCVAR_GET(user_language)));
  if (g_current < 0)
    g_current = FromSystem();
  if (g_current < 0)
    g_current = std::max(Find("en"), 0);
  return g_current;
}

// The current language's text for `key`, else English's, else the key.
const char* Lookup(const char* key) {
  const auto& languages = Languages();
  if (languages.empty())
    return key;
  for (int index : {Current(), Find("en")}) {
    if (index < 0)
      continue;
    if (const auto* text = languages[size_t(index)].intro[key].as_string())
      return text->get().c_str();
  }
  return key;
}

}  // namespace

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
  const toml::table& intro = Languages()[size_t(english)].intro;
  for (const char* key : kProgressKeys) {
    if (intro[key].value_or(std::string()) == title)
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

int IntroLanguage() {
  return Current();
}

void SetIntroLanguage(int index) {
  if (index < 0 || index >= IntroLanguageCount() || index == Current())
    return;
  g_current = index;
  REXCVAR_SET(ui_language, Languages()[size_t(index)].code);
  if (!g_config_path.empty())
    rex::cvar::SaveConfigSubset(g_config_path, {"ui_language"});
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
