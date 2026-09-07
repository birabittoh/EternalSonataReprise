// eternalsonata - ReXGlue Recompiled Project
// See achievement_translation.h for details.

#include "achievement_translation.h"

#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>

#include "settings.h"

namespace eternalsonata {
namespace {

// The three keys a mod publishes per achievement, keyed by AchievementInfo::id
// rather than by row: the overlay's row order is presentation, and would shift
// under a mod if the title ever gained an achievement.
std::string KeyFor(const char* prefix, uint32_t id) {
  return std::string(prefix) + std::to_string(id);
}

}  // namespace

void ApplyAchievementTranslations(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel)
    return;

  const uint32_t language = BootUserLanguageId();
  std::vector<rex::system::AchievementInfo> catalogue =
      kernel->achievements().ListAchievements();
  if (catalogue.empty())
    return;

  // Unlike everything this project draws into the game's own screens, the
  // overlay and the toast are ImGui, not the guest's single-byte glyph atlas.
  // So the published UTF-8 goes through untouched, and this is the one place in
  // the language feature where a mod can use whatever characters it likes.
  size_t translated = 0;
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
  if (!translated)
    return;

  // Replaces rather than merges, so an achievement nobody translated keeps the
  // XDBF strings it was just read back out with. Most languages will translate
  // none of them, and blanks there would be worse than English.
  kernel->achievements().ReplaceAchievements(std::move(catalogue));
  REXLOG_INFO("[achievements] {} of {} translated for language {}", translated,
              kernel->achievements().ListAchievements().size(), language);
}

}  // namespace eternalsonata
