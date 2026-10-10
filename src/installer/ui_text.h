#pragma once

// The interface text of the start screen and our overlays, from
// res/lang/*.toml: one table per screen, each key looked up in the language in
// use, then in English. The language is the user_language
// choice, so the interface and the game text always agree; with none chosen yet
// it is the system's, since the screen usually shows before any config exists.

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
  kNotEnoughSpace,
  kCount,
};

// The key itself when no language has it, which shows as a typo, not a crash.
const char* Tr(const char* section, const char* key);

// `fallback` when no language has the key.
const char* TrOr(const char* section, const char* key, const char* fallback);

// The [sdk_ui] table, for the SDK's own overlays (rex::ui::SetUiTextProvider).
// Null when no language has the key, which keeps the SDK's English text.
const char* SdkUiText(const char* key);

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

// Sets user_language to that language and saves it to settings.toml.
void SetIntroLanguage(int index);

// Binds the settings.toml SetIntroLanguage saves to, and adopts the system's
// language when none was chosen.
void BindIntroLanguageConfig(const std::filesystem::path& user_settings_path);

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
