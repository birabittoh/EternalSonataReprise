#pragma once

// The interface text of the start screen and our overlays, from
// res/lang/*.toml: one table per screen, each key looked up in the language in
// use, then in English. The ui_language cvar picks
// the language; unset, it follows user_language when the player picked one
// other than English, else the system's, since the screen usually shows
// before any config exists.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "install_pipeline.h"

namespace eternalsonata {

// In the order of the keys in IntroText's table in intro_text.cpp.
enum class IntroText {
  kStart,
  kExtract,
  kSelectFile,
  kSelectFolder,
  kQuit,
  kLanguage,
  kReady,
  kReadyRelease,
  kFound,
  kFoundRelease,
  kPressExtract,
  kNeedFiles,
  kSelectIsoOnly,
  kCopyHint,
  kSelected,
  kTitle,
  kUnsupportedLanguage,
  kDeleteFiles,
  kCancel,
  kConfirmDelete,
  kCount,
};

// The key itself when no language has it, which shows as a typo, not a crash.
const char* Tr(const char* section, const char* key);

// `fallback` when no language has the key.
const char* TrOr(const char* section, const char* key, const char* fallback);

std::string Tr(const char* section, const char* key, std::string_view arg);

const char* Tr(IntroText id);

// With `arg` in place of the text's "{}".
std::string Tr(IntroText id, std::string_view arg);

const char* TrPhase(Phase phase);

// A progress title the installer reports in English; unknown ones pass through.
std::string TrProgress(const std::string& title);

// The languages, each named in itself, for the screen's language picker.
int IntroLanguageCount();
const char* IntroLanguageName(int index);
const char* IntroLanguageCodeAt(int index);
int IntroLanguage();

// Switches and persists to `config_path` when one was bound.
void SetIntroLanguage(int index);

// The res/lang code of the language in use ("en", "ja", ...), and of the one
// the process started in.
const char* IntroLanguageCode();
const char* LaunchIntroLanguageCode();

// The player started the game from the start screen in the interface
// language, which the release has text for, so the text follows it.
void ConfirmIntroLanguage();
bool IntroLanguageConfirmed();
void BindIntroLanguageConfig(const std::filesystem::path& config_path);

// Mod supplied interface text. `language` is a res/lang code ("fr") to replace
// a string of a shipped language, or a mod language's XLanguage id ("9"), which
// then joins the interface languages under its label. Collected until
// ApplyModUiStrings, since mod languages register after the mods announce
// their strings. The first mod to set a string keeps it.
void AddUiString(std::string_view language, std::string_view section, std::string_view key,
                 std::string_view value);
void ApplyModUiStrings();

// `language` for AddUiString, from an XLanguage id.
std::string UiLanguageOfXLanguage(uint32_t id);

// Every language file's text, for baking the glyphs the screen needs.
std::string IntroGlyphText();

}  // namespace eternalsonata
