// eternalsonata - The PS3's item and magic tables.
#pragma once

#include <cstdint>

namespace eternalsonata {

// Puts the PS3's item master and magic records over the 360's, in place.
// Call once the xex image is mapped and before the guest runs.
void ApplyPs3ItemTables();

// The guest copy of the PS3 text block that replaces a 360 item or magic
// block, or 0. Guest thread only.
uint32_t Ps3TextBlockFor(uint32_t block);

// The guest copy of the PS3's twelve row magic display order, or 0.
uint32_t Ps3MagicOrderAddress();

// Where an item icon reader at `pc` should read `address` from, or 0. The
// icons the PS3 adds sit where the 360's table runs into the camp portraits.
uint32_t Ps3ItemIconAddress(uint32_t address, uint32_t pc);

// The highest item id the loaded master table names.
int BaseItemIdMax();

}  // namespace eternalsonata
