#pragma once

#include <cstdint>

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

}  // namespace eternalsonata
