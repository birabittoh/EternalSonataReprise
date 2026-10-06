// eternalsonata - The shop's equip panel, twelve characters wide. See
// party_shop_menu.cpp.
#pragma once

#include <cstdint>

namespace party_shop_menu {

// Called from the sub_821F2F38 hook in eternalsonata_options.cpp, which owns
// that hook. Returns an edited copy of the shop's display list, or 0 for any
// other list.
std::uint32_t MaybeSwapShopList(std::uint8_t* base, std::uint32_t list);

}  // namespace party_shop_menu
