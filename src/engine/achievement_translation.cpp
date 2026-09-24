// eternalsonata - ReXGlue Recompiled Project
// See achievement_translation.h for details.

#include "achievement_translation.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>
#include <rex/system/xex_module.h>

#include "settings.h"

namespace eternalsonata {
namespace {

// The three keys a mod publishes per achievement, keyed by AchievementInfo::id
// rather than by row: the overlay's row order is presentation, and would shift
// under a mod if the title ever gained an achievement.
std::string KeyFor(const char* prefix, uint32_t id) {
  return std::string(prefix) + std::to_string(id);
}

constexpr uint32_t kJapaneseLanguageId = 2;
constexpr uint16_t kNoString = 0xFFFF;

// PAL's XDBF has no Japanese, but a JP copy keeps its original xex beside the
// converted one, and that XDBF has the official Japanese names under the same
// achievement and string ids.
size_t ApplyJapaneseXdbf(rex::Runtime* runtime,
                         std::vector<rex::system::AchievementInfo>& catalogue) {
  const auto path = runtime->game_data_root() / "default.xex.orig";
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return 0;
  const std::vector<uint8_t> xex{std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>()};
  std::vector<uint8_t> image;
  if (!rex::runtime::XexModule::ExtractBaseImage(xex.data(), xex.size(), image))
    return 0;
  static constexpr uint8_t kMagic[] = {'X', 'D', 'B', 'F'};
  const auto at = std::search(image.begin(), image.end(), std::begin(kMagic), std::end(kMagic));
  if (at == image.end())
    return 0;
  const rex::system::util::XdbfGameData xdbf(&*at, size_t(image.end() - at));
  if (!xdbf.is_valid())
    return 0;

  const auto japanese = rex::system::XLanguage::kJapanese;
  size_t translated = 0;
  for (const auto& entry : xdbf.GetAchievements()) {
    auto it = std::find_if(catalogue.begin(), catalogue.end(),
                           [&](const auto& a) { return a.id == entry.id; });
    if (it == catalogue.end())
      continue;
    const std::string label = xdbf.GetStringTableEntry(japanese, entry.label_id);
    if (label.empty())
      continue;
    it->label = label;
    it->description = xdbf.GetStringTableEntry(japanese, entry.description_id);
    if (entry.unachieved_id != kNoString)
      it->unachieved_description = xdbf.GetStringTableEntry(japanese, entry.unachieved_id);
    ++translated;
  }
  return translated;
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

  size_t translated = 0;
  if (language == kJapaneseLanguageId)
    translated = ApplyJapaneseXdbf(runtime, catalogue);

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
