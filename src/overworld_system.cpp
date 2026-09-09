#include "overworld_system.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>

#include <rex/runtime.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_overworld_api.h"
#include "field_player_model_override.h"
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
