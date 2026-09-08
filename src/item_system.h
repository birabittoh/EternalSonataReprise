// eternalsonata - Items and the Item Set, for mods.
//
// The public C ABI mods call is eternalsonata_item_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace eternalsonata {

// Binds the item system to the runtime and starts the per-frame poll that
// publishes the inventory and Item Set events. Call once the runtime is live
// (OnPostSetup). Until then every API entry point answers "unavailable" and no
// event is published (the events need the runtime's mod registry).
void BindItemSystem(rex::Runtime* runtime);

// Called after the guest has restored its globals from a loaded save, so the
// inventory and set that come back are adopted rather than reported as a batch
// of changes. The hook on sub_82240AF8 lives in photo_system.cpp, because a
// guest routine can only be hooked once, and forwards here.
void NotifyItemSaveLoaded();

}  // namespace eternalsonata
