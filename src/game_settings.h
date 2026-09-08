// eternalsonata - The game's own Options settings, for mods.
//
// The public C ABI mods call is eternalsonata_settings_api.h; this header is
// the small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the settings system to the runtime and starts the per-frame poll that
// publishes the change events. Call once the runtime is live (OnPostSetup).
// Until then every API entry point answers "unavailable".
void BindGameSettings(rex::Runtime* runtime);

// Called after the guest has restored its globals from a loaded save, so the
// settings that come back are adopted rather than reported as a batch of
// changes. The hook on sub_82240AF8 lives in photo_system.cpp and forwards
// here; a guest routine can only be hooked once.
void NotifyGameSettingsSaveLoaded();

}  // namespace eternalsonata
