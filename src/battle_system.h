// eternalsonata - Battle state: reading it, and forcing a win.
//
// The public C ABI mods call is eternalsonata_battle_api.h; this header is the
// small internal surface the rest of the exe needs. The guest layout behind it
// is battle_layout.h.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the battle system to the runtime. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable".
void BindBattleSystem(rex::Runtime* runtime);

}  // namespace eternalsonata
