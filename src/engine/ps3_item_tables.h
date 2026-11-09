// eternalsonata - The PS3's item and magic tables.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace eternalsonata {

// Puts the PS3's item master and magic records over the 360's, in place.
// Call once the xex image is mapped and before the guest runs.
void ApplyPs3ItemTables();

// The guest copy of the PS3 text block that replaces a 360 item or magic
// block, or 0. Guest thread only.
uint32_t Ps3TextBlockFor(uint32_t block);

// The Scrapbook's block, which stands in for no 360 block.
inline constexpr uint32_t kPs3ScrapbookBlock = 0;

// The PS3 text block that stands in for the 360 block at `block` in PS3 mode,
// or empty. Mod text edits for that block are made to it and handed back with
// SetPs3TextBlockSource, before the guest runs.
std::span<const uint8_t> Ps3TextBlockSource(uint32_t block);
void SetPs3TextBlockSource(uint32_t block, std::vector<uint8_t> data);

// The guest copy of the PS3's twelve row magic display order, or 0.
uint32_t Ps3MagicOrderAddress();

// Where an item icon reader at `pc` should read `address` from, or 0. The
// icons the PS3 adds sit where the 360's table runs into the camp portraits.
uint32_t Ps3ItemIconAddress(uint32_t address, uint32_t pc);

// The highest item id the loaded master table names.
int BaseItemIdMax();

// A Scrapbook string from the PS3 EBOOT, as the game's text bytes: ids 0..9
// picture titles, 10..19 their captions, 20 the entry name, 21 its heading
// and 22 the open prompt. `language` is dword_8243D370's numbering.
const char* Ps3ScrapbookText(uint32_t language, uint32_t id);

}  // namespace eternalsonata
