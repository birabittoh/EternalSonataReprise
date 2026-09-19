// eternalsonata - Cutscene state: observing the game's events, and skipping.
//
// The public C ABI mods call is eternalsonata_cutscene_api.h; this header is
// the small internal surface the rest of the exe needs.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the cutscene system to the runtime. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable" and
// no event is published.
void BindCutsceneSystem(rex::Runtime* runtime);

// True while an event script is running. The overworld camera API uses it
// to keep serving the cutscene's camera.
bool IsCutsceneActive();

}  // namespace eternalsonata
