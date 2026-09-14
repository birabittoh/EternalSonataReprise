// Public ABI for events produced while the party is on a field map.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETERNALSONATA_OVERWORLD_ABI_VERSION 1u

enum {
  ETERNALSONATA_OVERWORLD_OK = 0,
  ETERNALSONATA_OVERWORLD_QUEUED = 1,
  ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE = -1,
  ETERNALSONATA_OVERWORLD_ERR_INVALID_ARGUMENT = -2,
  ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE = -3
};

#define ETERNALSONATA_OVERWORLD_EVENT_AREA_ENTERED \
  "eternalsonata.overworld.area_entered"
#define ETERNALSONATA_OVERWORLD_EVENT_ACTION \
  "eternalsonata.overworld.action"
#define ETERNALSONATA_OVERWORLD_EVENT_DIALOGUE_STARTED \
  "eternalsonata.overworld.dialogue_started"
#define ETERNALSONATA_OVERWORLD_EVENT_BATTLE_STARTED \
  "eternalsonata.overworld.battle_started"

enum {
  ETERNALSONATA_OVERWORLD_ACTION_NONE = 0,
  ETERNALSONATA_OVERWORLD_ACTION_ENTER_AREA = 1,
  ETERNALSONATA_OVERWORLD_ACTION_FIELD_ANIMATION = 2,
  ETERNALSONATA_OVERWORLD_ACTION_START_BATTLE = 3,
  ETERNALSONATA_OVERWORLD_ACTION_START_DIALOGUE = 4
};

// Immutable data copied into payload.bytes. area_id is the cfdata id, while
// area_name is the same localized display name used by Discord presence.
// animation_id is the game's exact field animation slot. Slots 16 through 35
// are the authored interaction motions used for chests, doors, climbing and
// other traversal. encounter_id and the battle fields are the exact arguments
// passed to the game's battle transition.
typedef struct EternalSonataOverworldEvent {
  int32_t action;
  int32_t character;
  int32_t animation_id;
  int32_t encounter_id;
  int32_t battle_music_id;
  int32_t battle_rule_id;
  int32_t battle_target_id;
  uint32_t field_object_id;
  float position_x;
  float position_y;
  float position_z;
  int32_t reserved[2];
  char area_id[32];
  char area_name[96];
  char dialogue[256];
} EternalSonataOverworldEvent;

// Host ABI version, so a mod can check compatibility.
typedef uint32_t (*EternalSonataOverworldAbiVersionFn)(void);

// True when area loading can be requested: field active and no battle.
typedef int (*EternalSonataIsAreaLoadingAvailableFn)(void);

// Queues a field area id (e.g. "hno01") to be force-loaded on the next guest tick.
typedef int (*EternalSonataForceLoadAreaFn)(const char* area_id);

// Number of known areas in the area table.
typedef int (*EternalSonataGetAreaCountFn)(void);

// Copies area id and display name for area `index` (0-based) into the buffers.
typedef int (*EternalSonataGetAreaInfoFn)(int index, char* id_out, int id_len,
                                         char* name_out, int name_len);

#ifdef __cplusplus
}
#endif
