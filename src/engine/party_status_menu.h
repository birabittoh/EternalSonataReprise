// eternalsonata - status page additions. See party_status_menu.cpp.
#pragma once

#include <cstdint>

namespace party_status_menu {

// Called from the sub_821F2F38 hook in eternalsonata_options.cpp, which owns
// that hook. Returns an edited copy of the status page's display list, or 0 for
// any other list.
std::uint32_t MaybeSwapStatusList(std::uint8_t* base, std::uint32_t list);

// Called from the sub_8223B780 hook, which owns that hook. Returns the guest
// string for the costume prompt's label, or 0 to fall through.
std::uint32_t CostumePromptText(std::uint8_t* base, std::uint32_t blob,
                                std::uint32_t sid);

}  // namespace party_status_menu
