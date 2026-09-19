#pragma once

#include <cstdint>

#include "eternalsonata_overworld_api.h"

namespace rex {
class Runtime;
}

namespace eternalsonata {

void BindOverworldSystem(rex::Runtime* runtime);
void NotifyOverworldAreaEntered(const char* area_id);
void NotifyOverworldFieldAction(uint32_t object, int32_t animation);
void NotifyOverworldDialogue(uint32_t window, const char* text);
void NotifyOverworldBattleStarted(uint32_t encounter, uint32_t music,
                                  uint32_t rule, uint32_t target);

// Active camera control shared with the cutscene API. Status codes are the
// overworld ABI's. A cutscene request is honoured only while an event runs
// and is dropped when it ends; the target transform survives shot changes.
int ReadActiveCamera(EternalSonataFieldCamera* out);
int QueueActiveCamera(const EternalSonataFieldCamera* camera);
int RequestCutsceneCameraControl(int enabled);
bool IsCutsceneCameraControlled();

}  // namespace eternalsonata
