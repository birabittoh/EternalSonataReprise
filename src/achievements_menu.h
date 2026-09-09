// eternalsonata - the status menu's Achievements screen. See achievements_menu.cpp.
#pragma once

#include <cstdint>

namespace achievements_menu {

// True while the Music gallery is standing in for the Achievements screen.
bool Active();

// Replaces the strip's icon art with the trophy icon in src/images/. Registers
// an asset patch, so it must run before BindAssetSystem builds the cache.
void RegisterTrophyIcon();

// Called from the sub_8223B780 hook in eternalsonata_options.cpp, which owns
// that hook. While a row of the Achievements screen is being built, answers the
// row title lookup with the achievement's own name. Returns a guest string
// address, or 0 to fall through.
std::uint32_t RowTitleOverride(std::uint8_t* base, std::uint32_t blob,
                               std::uint32_t sid);

// True while this screen is up and `blob` is the one its heading comes out of,
// meaning the caller should resolve the reference string for TitleOverrideFor.
bool WantsTitleSwap(std::uint32_t blob);

// The heading is inherited from the Music gallery and its string id was never
// found, so it is recognised by content instead: `music_text` is what the same
// blob returns for the Music label, and any lookup that resolves to it becomes
// Achievements. Returns a guest string address, or 0 to leave `result` alone.
std::uint32_t TitleOverrideFor(std::uint8_t* base, std::uint32_t result,
                               std::uint32_t music_text);

// Called from the sub_821F2F38 hook in eternalsonata_options.cpp, which owns
// that hook. While the Achievements screen is up, hands the interpreter an
// edited copy of the Music gallery's display list with the playback button
// prompts taken out. Returns the replacement list address, or 0 to leave the
// incoming one alone.
std::uint32_t MaybeSwapScreenList(std::uint8_t* base, std::uint32_t list);

}  // namespace achievements_menu
