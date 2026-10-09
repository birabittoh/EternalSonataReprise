#pragma once

// The start screen's text, from res/lang/*.toml. The ui_language cvar picks
// the language; unset, it follows user_language when the player picked one
// other than English, else the system's, since the screen usually shows
// before any config exists.

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
  kSelectIsoOrFolder,
  kSelectIsoOnly,
  kCopyHint,
  kSelected,
  kTitle,
  kCount,
};

const char* Tr(IntroText id);

// With `arg` in place of the text's "{}".
std::string Tr(IntroText id, std::string_view arg);

const char* TrPhase(Phase phase);

// A progress title the installer reports in English; unknown ones pass through.
std::string TrProgress(const std::string& title);

// The languages, each named in itself, for the screen's language picker.
int IntroLanguageCount();
const char* IntroLanguageName(int index);
int IntroLanguage();

// Switches and persists to `config_path` when one was bound.
void SetIntroLanguage(int index);

// The res/lang code of the language in use ("en", "ja", ...), and of the one
// the process started in.
const char* IntroLanguageCode();
const char* LaunchIntroLanguageCode();
void BindIntroLanguageConfig(const std::filesystem::path& config_path);

// Every language file's text, for baking the glyphs the screen needs.
std::string IntroGlyphText();

}  // namespace eternalsonata
