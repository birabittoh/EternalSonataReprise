// eternalsonata - the mod-facing achievement API.
//
// The catalogue itself is the SDK's (rex::system::AchievementManager, filled
// from the title's XDBF at XEX load). This file only adds what a mod needs on
// top of it: an id it does not have to pick, a way to take one back out again,
// and the same read shapes the other APIs here use. See
// eternalsonata_achievement_api.h for the contract.
//
// Custom ids come from 0x10000 up, well clear of the title's 1..22, which is
// also what the status menu's third tab keys off (see achievements_menu.cpp).

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>

#include "achievements_menu.h"
#include "eternalsonata_achievement_api.h"
#include "settings.h"

namespace eternalsonata {
namespace {

constexpr uint32_t kCustomIdMin = ETERNALSONATA_CUSTOM_ACHIEVEMENT_ID_MIN;
constexpr uint32_t kCustomIdMax = ETERNALSONATA_CUSTOM_ACHIEVEMENT_ID_MAX;

std::mutex g_mutex;              // guards g_custom_ids
std::set<uint32_t> g_custom_ids; // what this API handed out, still registered

rex::system::AchievementManager* Manager() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? &kernel->achievements() : nullptr;
}

bool IsCustomId(int id) {
  return id >= static_cast<int>(kCustomIdMin) && id <= static_cast<int>(kCustomIdMax);
}

void CopyString(char* out, size_t size, const std::string& text) {
  const size_t n = std::min(text.size(), size - 1);
  std::memcpy(out, text.data(), n);
  out[n] = '\0';
}

void Fill(EternalSonataAchievement* out, const rex::system::AchievementInfo& info,
          rex::system::AchievementManager* manager) {
  *out = {};
  out->id = static_cast<int32_t>(info.id);
  out->is_custom = IsCustomId(static_cast<int>(info.id)) ? 1 : 0;
  out->unlocked = manager->IsUnlocked(info.id) ? 1 : 0;
  out->gamerscore = static_cast<int32_t>(info.gamerscore);
  out->secret = (info.flags & rex::system::kAchievementFlagShowUnachieved) ? 0 : 1;
  CopyString(out->name, sizeof(out->name), info.label);
  CopyString(out->description, sizeof(out->description),
             out->unlocked || !out->secret ? info.description
                                           : info.unachieved_description);
  out->unlocked_at = out->unlocked ? manager->GetUnlockTime(info.id) : 0u;
}

// The translation for the language the process booted in, or null. Picked once,
// at registration: user_language is restart-required, so it cannot change under
// a running process.
const EternalSonataAchievementTranslation* BootTranslation(
    const EternalSonataCustomAchievementData* data) {
  if (!data->translations || data->translation_count <= 0) {
    return nullptr;
  }
  const uint32_t language = BootUserLanguageId();
  for (int i = 0; i < data->translation_count; ++i) {
    if (static_cast<uint32_t>(data->translations[i].language) == language) {
      return &data->translations[i];
    }
  }
  return nullptr;
}

std::vector<rex::system::AchievementInfo> SortedCatalogue(
    rex::system::AchievementManager* manager) {
  std::vector<rex::system::AchievementInfo> catalogue = manager->ListAchievements();
  std::sort(catalogue.begin(), catalogue.end(),
            [](const rex::system::AchievementInfo& a,
               const rex::system::AchievementInfo& b) { return a.id < b.id; });
  return catalogue;
}

}  // namespace
}  // namespace eternalsonata

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataAchievementAbiVersion(void) {
  return ETERNALSONATA_ACHIEVEMENT_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsAchievementSystemAvailable(void) {
  return eternalsonata::Manager() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAchievementLanguage(void) {
  return static_cast<int>(eternalsonata::BootUserLanguageId());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRegisterCustomAchievement(
    const EternalSonataCustomAchievementData* data) {
  using namespace eternalsonata;
  if (!data || !data->name || !data->name[0]) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_INVALID_ARGUMENT;
  }
  auto* manager = Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }

  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Lowest free id, so unregistering hands it back.
    for (uint32_t candidate = kCustomIdMin; candidate <= kCustomIdMax; ++candidate) {
      if (!g_custom_ids.count(candidate) && !manager->FindAchievement(candidate)) {
        id = candidate;
        break;
      }
    }
    if (!id) {
      return ETERNALSONATA_ACHIEVEMENT_ERR_FULL;
    }
    g_custom_ids.insert(id);
  }

  const EternalSonataAchievementTranslation* translated = BootTranslation(data);
  const char* name = translated && translated->name ? translated->name : data->name;
  const char* description = translated && translated->description
                                ? translated->description
                                : data->description;
  const char* locked = translated && translated->locked_description
                           ? translated->locked_description
                           : data->locked_description;

  rex::system::AchievementInfo info;
  info.id = id;
  info.label = name;
  info.description = description ? description : "";
  info.unachieved_description = locked ? locked : info.description;
  info.icon_path = data->icon_path ? data->icon_path : "";
  info.gamerscore = data->gamerscore > 0 ? static_cast<uint32_t>(data->gamerscore) : 0u;
  info.flags = data->secret ? 0u : rex::system::kAchievementFlagShowUnachieved;
  manager->RegisterAchievement(std::move(info));

  achievements_menu::InvalidateRows();
  REXLOG_INFO("[achievements] mod registered '{}' as id {}", name, id);
  return static_cast<int>(id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataUnregisterCustomAchievement(int id) {
  using namespace eternalsonata;
  if (!IsCustomId(id)) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_NOT_CUSTOM;
  }
  auto* manager = Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_custom_ids.erase(static_cast<uint32_t>(id))) {
      return ETERNALSONATA_ACHIEVEMENT_ERR_NO_SUCH_ACHIEVEMENT;
    }
  }

  // The manager has no remove call, so the catalogue is replaced without it.
  std::vector<rex::system::AchievementInfo> catalogue = manager->ListAchievements();
  catalogue.erase(std::remove_if(catalogue.begin(), catalogue.end(),
                                 [id](const rex::system::AchievementInfo& info) {
                                   return info.id == static_cast<uint32_t>(id);
                                 }),
                  catalogue.end());
  manager->ReplaceAchievements(std::move(catalogue));

  achievements_menu::InvalidateRows();
  return ETERNALSONATA_ACHIEVEMENT_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAchievementCount(void) {
  auto* manager = eternalsonata::Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }
  return static_cast<int>(manager->ListAchievements().size());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAchievement(
    int id, EternalSonataAchievement* out) {
  using namespace eternalsonata;
  if (!out || id <= 0) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_INVALID_ARGUMENT;
  }
  auto* manager = Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }
  const auto info = manager->FindAchievement(static_cast<uint32_t>(id));
  if (!info) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_NO_SUCH_ACHIEVEMENT;
  }
  Fill(out, *info, manager);
  return ETERNALSONATA_ACHIEVEMENT_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAchievements(
    EternalSonataAchievement* out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_INVALID_ARGUMENT;
  }
  auto* manager = Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }
  const std::vector<rex::system::AchievementInfo> catalogue = SortedCatalogue(manager);
  if (max == 0) {
    return static_cast<int>(catalogue.size());
  }
  int written = 0;
  for (const auto& info : catalogue) {
    if (written >= max) {
      break;
    }
    Fill(&out[written], info, manager);
    ++written;
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsAchievementUnlocked(int id) {
  auto* manager = eternalsonata::Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }
  if (id <= 0 || !manager->FindAchievement(static_cast<uint32_t>(id))) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_NO_SUCH_ACHIEVEMENT;
  }
  return manager->IsUnlocked(static_cast<uint32_t>(id)) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataUnlockAchievement(int id,
                                                                   int show_toast) {
  auto* manager = eternalsonata::Manager();
  if (!manager) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE;
  }
  if (id <= 0) {
    return ETERNALSONATA_ACHIEVEMENT_ERR_INVALID_ARGUMENT;
  }
  const auto result = manager->UnlockAchievement(
      static_cast<uint32_t>(id),
      show_toast ? rex::system::AchievementNotification::kShow
                 : rex::system::AchievementNotification::kSuppress);
  switch (result) {
    case rex::system::AchievementUnlockResult::kUnlocked:
      achievements_menu::InvalidateRows();
      return 1;
    case rex::system::AchievementUnlockResult::kAlreadyUnlocked:
      return 0;
    default:
      return ETERNALSONATA_ACHIEVEMENT_ERR_NO_SUCH_ACHIEVEMENT;
  }
}
