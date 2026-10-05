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

// The highest item id the loaded master table names.
int BaseItemIdMax();

}  // namespace eternalsonata
