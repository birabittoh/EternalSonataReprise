// eternalsonata - the PS3's Scrapbook camp page. See scrapbook_menu.cpp.
#pragma once

#include <cstdint>
#include <string>

#include "generated/eternalsonata_init.h"

namespace scrapbook_menu {

// The strip tag the entry carries; past the twelve sub_82236CD0 switches on.
inline constexpr std::uint32_t kTag = 13;

// PS3 data with the pictures, and Bel01.e has turned the entry on.
bool StripShowsScrapbook();

// The entry's label as the strip and the heading print it, without markup.
std::string EntryLabel(const std::uint8_t* base);

// True while the Music gallery is standing in for the Scrapbook.
bool Active();

// sub_82236CD0 was asked for `tag`. Opens the page and returns true for ours;
// any other tag takes the mode off.
bool Dispatch(std::uint8_t* base, std::uint32_t tag);

// After the gallery's init and every tab draw, which reset the row count.
void FixRowCount(std::uint8_t* base);
std::uint32_t RowCount();

// Around one row build, so its title lookup is answered.
void BeginRow(int index);
void EndRow();

// After the gallery's per frame tick.
void Tick(PPCContext& ctx, std::uint8_t* base);

// Stands in for the gallery's input handler while the page is up.
void Input(PPCContext& ctx, std::uint8_t* base);

// From the sub_8223B780 hook: the page's own strings, or 0.
std::uint32_t TextOverride(std::uint8_t* base, std::uint32_t blob, std::uint32_t sid);

// From the sub_821F2F38 hook, before and after the interpreter: the Music list
// with the playback prompts gone and the picture panel added.
std::uint32_t MaybeSwapScreenList(std::uint8_t* base, std::uint32_t list);
void ScreenBuilt(std::uint8_t* base, std::uint32_t root);

}  // namespace scrapbook_menu
