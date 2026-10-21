// eternalsonata - The game's flag system, for mods.
//
// The public C ABI mods call is eternalsonata_flag_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the flag system to the runtime and starts the per-frame poll that
// publishes the change events. Call once the runtime is live (OnPostSetup).
// Until then every API entry point answers "unavailable".
void BindFlagSystem(rex::Runtime* runtime);

// Called after the guest has restored its globals from a loaded save, so the
// bank that comes back is adopted rather than reported as a few thousand
// changes. The hook on sub_82240AF8 lives in photo_system.cpp and forwards
// here; a guest routine can only be hooked once.
void NotifyFlagsSaveLoaded();

// Makes the guest see a nonzero Encore counter until EndEncoreOverride, for a
// hook that wants one routine to act as in Encore. The API and the change
// events keep reporting the real value meanwhile. No effect if the counter is
// already nonzero. Calls must pair up on one thread.
void BeginEncoreOverride();
void EndEncoreOverride();

}  // namespace eternalsonata
