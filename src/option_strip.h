// eternalsonata - status/pause menu option strip. See option_strip.cpp.
#pragma once

#include <cstdint>

namespace option_strip {

// Called from the sub_821F2F38 hook in eternalsonata_options.cpp, which owns
// that hook because a guest function can only be hooked once. `list` is the
// display list the interpreter is about to walk: for the status menu that is a
// writable stack copy, not the shipped list. Does nothing for other screens.
void MaybeEditOptionStrip(std::uint8_t* base, std::uint32_t list);

// Called from the sub_8223B780 hook in eternalsonata_options.cpp, which owns
// that hook. Returns a guest string address for the Achievements hover label
// while the strip is asking for the moved slot, or 0 to fall through.
std::uint32_t AchievementsLabelOverride(std::uint8_t* base, std::uint32_t sid);

// The entry's own name, in the language the menu is drawn in, so the screen it
// opens can head itself with the same word.
const char* AchievementsWord(const std::uint8_t* base);

}  // namespace option_strip
