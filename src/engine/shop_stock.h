// eternalsonata - The shop's stock list, thirty five items long.
#pragma once

#include <cstdint>

namespace eternalsonata {

// Where an access to the stock list (word_82560114, its count and shop id)
// made by the instruction at `pc` should go. Called for every address in the
// list's [address_remap] range.
uint32_t ShopStockAddress(uint32_t address, uint32_t pc);

}  // namespace eternalsonata
