#include "overworld_system.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <rex/runtime.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "area_names.generated.h"
#include "eternalsonata_overworld_api.h"
#include "field_player_model_override.h"
#include "force_load_area.h"
#include "room_presence.h"

namespace eternalsonata {
namespace {

rex::Runtime* g_runtime = nullptr;

void CopyText(char* destination, size_t capacity, const std::string& source) {
  if (!capacity) {
    return;
  }
  const size_t count = std::min(capacity - 1, source.size());
  std::memcpy(destination, source.data(), count);
  destination[count] = 0;
}

EternalSonataOverworldEvent MakeEvent(int action) {
  EternalSonataOverworldEvent event{};
  event.action = action;
  const auto area = GetRoomPresence().CurrentArea();
  CopyText(event.area_id, sizeof(event.area_id), area.id);
  CopyText(event.area_name, sizeof(event.area_name), area.name);
  return event;
}

void Publish(const char* name, const EternalSonataOverworldEvent& event,
             uint64_t value) {
  if (!g_runtime || !g_runtime->mod_registry()) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = value;
  payload.f64 = static_cast<double>(value);
  payload.bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(&event), sizeof(event));
  g_runtime->mod_registry()->Publish(name, payload);
}

}  // namespace

void BindOverworldSystem(rex::Runtime* runtime) { g_runtime = runtime; }

void NotifyOverworldAreaEntered(const char* area_id) {
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_ENTER_AREA);
  const auto area = GetRoomPresence().DescribeArea(area_id ? area_id : "");
  CopyText(event.area_id, sizeof(event.area_id), area.id);
  CopyText(event.area_name, sizeof(event.area_name), area.name);
  Publish(ETERNALSONATA_OVERWORLD_EVENT_AREA_ENTERED, event, 0);
}

void NotifyOverworldFieldAction(uint32_t object, int32_t animation) {
  if (animation < 16 || animation > 35 ||
      !GetRoomPresence().IsFieldLeader(object)) {
    return;
  }
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_FIELD_ANIMATION);
  event.character = FieldPlayerModelOverride::PartyLeaderCharacter();
  event.animation_id = animation;
  auto* memory = rex::system::kernel_state()->memory();
  const auto* host = memory->TranslateVirtual<const uint8_t*>(object);
  if (host) {
    event.field_object_id = rex::memory::load_and_swap<uint32_t>(host + 4);
    event.position_x = rex::memory::load_and_swap<float>(host + 32);
    event.position_y = rex::memory::load_and_swap<float>(host + 36);
    event.position_z = rex::memory::load_and_swap<float>(host + 40);
  }
  Publish(ETERNALSONATA_OVERWORLD_EVENT_ACTION, event,
          static_cast<uint64_t>(animation));
}

// A conversation line is just a SetText on the text manager window the field
// script owns, so `window` is published to tell concurrent windows apart. That
// box is reused, so object prompts arrive here too.
void NotifyOverworldDialogue(uint32_t window, const char* text) {
  if (!text || !GetRoomPresence().IsFieldActive() ||
      GetRoomPresence().IsBattleActive()) {
    return;
  }
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_START_DIALOGUE);
  event.character = FieldPlayerModelOverride::PartyLeaderCharacter();
  CopyText(event.dialogue, sizeof(event.dialogue), text);
  Publish(ETERNALSONATA_OVERWORLD_EVENT_DIALOGUE_STARTED, event, window);
}

void NotifyOverworldBattleStarted(uint32_t encounter, uint32_t music,
                                  uint32_t rule, uint32_t target) {
  EternalSonataOverworldEvent event = MakeEvent(
      ETERNALSONATA_OVERWORLD_ACTION_START_BATTLE);
  event.character = FieldPlayerModelOverride::PartyLeaderCharacter();
  event.encounter_id = static_cast<int32_t>(encounter);
  event.battle_music_id = static_cast<int32_t>(music);
  event.battle_rule_id = static_cast<int32_t>(rule);
  event.battle_target_id = static_cast<int32_t>(target);
  Publish(ETERNALSONATA_OVERWORLD_EVENT_BATTLE_STARTED, event, encounter);
}

}  // namespace eternalsonata

// Public C ABI (see eternalsonata_overworld_api.h). AreaNameTable() is an
// unordered_map; sort once for a stable, readable enumeration order.
namespace {

const std::vector<std::pair<std::string, std::string>>& SortedAreas() {
  static const std::vector<std::pair<std::string, std::string>> sorted = [] {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto& [id, name] : eternalsonata::AreaNameTable()) {
      v.emplace_back(id, name);
    }
    std::sort(v.begin(), v.end());
    return v;
  }();
  return sorted;
}

}  // namespace

using namespace eternalsonata;

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataOverworldAbiVersion(void) {
  return ETERNALSONATA_OVERWORLD_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsAreaLoadingAvailable(void) {
  return !GetRoomPresence().IsBattleActive() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataForceLoadArea(const char* area_id) {
  if (!area_id || area_id[0] == '\0') {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (GetRoomPresence().IsBattleActive()) {
    return ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE;
  }
  GetForceLoadArea().Request(area_id);
  return ETERNALSONATA_OVERWORLD_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAreaCount(void) {
  return static_cast<int>(SortedAreas().size());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAreaInfo(int index, char* id_out, int id_len,
                                                             char* name_out, int name_len) {
  const auto& areas = SortedAreas();
  if (index < 0 || index >= static_cast<int>(areas.size())) {
    return ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT;
  }
  if (id_out && id_len > 0) {
    const std::string& id = areas[index].first;
    const size_t n = std::min(static_cast<size_t>(id_len - 1), id.size());
    std::memcpy(id_out, id.data(), n);
    id_out[n] = '\0';
  }
  if (name_out && name_len > 0) {
    const std::string& name = areas[index].second;
    const size_t n = std::min(static_cast<size_t>(name_len - 1), name.size());
    std::memcpy(name_out, name.data(), n);
    name_out[n] = '\0';
  }
  return ETERNALSONATA_OVERWORLD_OK;
}
