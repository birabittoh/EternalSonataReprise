// eternalsonata - Battle state: reading it, and forcing a win.
//
// The public C ABI mods call is eternalsonata_battle_api.h; this header is the
// small internal surface the rest of the exe needs. The guest layout behind it
// is battle_layout.h.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the battle system to the runtime. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable".
void BindBattleSystem(rex::Runtime* runtime);

void NotifyBattleAbility(uint32_t ability, uint32_t action);
void NotifyBattleItem(uint32_t item);

}  // namespace eternalsonata
