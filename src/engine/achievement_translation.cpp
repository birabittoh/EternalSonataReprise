// eternalsonata - ReXGlue Recompiled Project
// See achievement_translation.h for details.

#include "achievement_translation.h"

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <rex/embedded_metadata.h>
#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

#include "achievement_text.h"
#include "icon.generated.h"
#include "settings.h"

namespace eternalsonata {
namespace {

// The three keys a mod publishes per achievement, keyed by AchievementInfo::id
// rather than by row: the overlay's row order is presentation, and would shift
// under a mod if the title ever gained an achievement.
std::string KeyFor(const char* prefix, uint32_t id) {
  return std::string(prefix) + std::to_string(id);
}

// The boot language's own table, else the built-in language a mod language
// borrows its BTX block from (the live cvar), else English.
std::span<const AchievementText> TextFor(uint32_t language) {
  for (const uint32_t id : {language, REXCVAR_GET(user_language), 1u}) {
    if (id < std::size(kAchievementTextByLanguage) && !kAchievementTextByLanguage[id].empty())
      return kAchievementTextByLanguage[id];
  }
  return kAchievementTextEnglish;
}

}  // namespace

void ApplyAchievementTranslations(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel)
    return;

  // The overlay's icon cache looks here before the guest's XDBF.
  for (const auto& image : kAchievementImages) {
    rex::RegisterEmbeddedMetadataAsset("icons/" + std::to_string(image.id) + ".png", image.png,
                                       image.size);
  }

  const uint32_t language = BootUserLanguageId();
  const auto loaded = kernel->achievements().ListAchievements();
  const auto text = TextFor(language);
  std::vector<rex::system::AchievementInfo> catalogue;
  for (size_t i = 0; i < std::size(kAchievements); ++i) {
    const auto& record = kAchievements[i];
    rex::system::AchievementInfo info;
    info.id = record.id;
    info.label = text[i].label;
    info.description = text[i].description;
    info.unachieved_description = text[i].unachieved_description;
    info.image_id = record.image_id;
    info.gamerscore = record.gamerscore;
    info.flags = record.flags;
    // Metadata the SDK loaded may name an icon file.
    auto it = std::find_if(loaded.begin(), loaded.end(),
                           [&](const auto& a) { return a.id == record.id; });
    if (it != loaded.end())
      info.icon_path = it->icon_path;
    catalogue.push_back(std::move(info));
  }

  size_t translated = 0;
  // Unlike everything this project draws into the game's own screens, the
  // overlay and the toast are ImGui, not the guest's single-byte glyph atlas.
  // So the published UTF-8 goes through untouched, and this is the one place in
  // the language feature where a mod can use whatever characters it likes.
  for (auto& achievement : catalogue) {
    bool touched = false;
    if (const char* text = FindNativeString(language, KeyFor("achv_name_", achievement.id))) {
      achievement.label = text;
      touched = true;
    }
    if (const char* text = FindNativeString(language, KeyFor("achv_desc_", achievement.id))) {
      achievement.description = text;
      touched = true;
    }
    if (const char* text =
            FindNativeString(language, KeyFor("achv_desc_locked_", achievement.id))) {
      achievement.unachieved_description = text;
      touched = true;
    }
    translated += touched ? 1 : 0;
  }
  kernel->achievements().ReplaceAchievements(std::move(catalogue));
  if (translated)
    REXLOG_INFO("[achievements] {} of {} translated for language {}", translated,
                kernel->achievements().ListAchievements().size(), language);
}

}  // namespace eternalsonata
