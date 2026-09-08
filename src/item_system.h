// eternalsonata - Items and the Item Set, for mods.
//
// The public C ABI mods call is eternalsonata_item_api.h; this header is the
// small internal surface the rest of the exe needs.
#pragma once

#include <cstdint>

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

// Resolves `text_id` in the BTX block at guest address `block`, in the language
// the game is running in, and caches the result for the life of the process.
// Never null; a block that has nothing for the id answers "". This is the item
// system's own text reader, shared so that other systems on other text blocks
// (the magic half of equipment_system.cpp) do not have to duplicate it or
// queue sub_8223B780 onto the guest thread.
const char* LookupBtxString(uint32_t block, int text_id);

}  // namespace eternalsonata
