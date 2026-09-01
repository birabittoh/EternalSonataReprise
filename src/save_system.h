// eternalsonata - Save state: observing the game's save flow, for mods.
//
// The public C ABI mods call is eternalsonata_save_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the save system to the runtime. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable" and
// no save event is published (the events need the runtime's mod registry).
void BindSaveSystem(rex::Runtime* runtime);

}  // namespace eternalsonata
