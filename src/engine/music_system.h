// eternalsonata - Music menu (OST gallery) unlock state for mods.
//
// The public C ABI mods call is eternalsonata_music_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the music system to the runtime and starts the per-frame poll that
// publishes the unlock events. Call once the runtime is live (OnPostSetup).
// Until then every API entry point answers "unavailable" and no event is
// published (the events need the runtime's mod registry).
void BindMusicSystem(rex::Runtime* runtime);

// Called after the guest has restored its globals from a loaded save, so the
// unlock state that comes back is adopted rather than reported as a batch of
// unlock events. The hook on sub_82240AF8 lives in photo_system.cpp - a guest
// routine can only be hooked once - and forwards here.
void NotifyMusicSaveLoaded();

}  // namespace eternalsonata
