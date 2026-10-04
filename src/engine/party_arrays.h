// eternalsonata - The per-character party arrays, twelve entries wide.
//
// The game keeps its per-character state in arrays exactly ten entries wide,
// packed back to back (docs/party-system.md). They live in guest memory
// allocated here instead, sized for twelve; [address_remap] in
// config/party.toml sends every guest access to the old arrays to the new
// ones, by the address the access lands on.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}

namespace eternalsonata {

inline constexpr uint32_t kPartyCharacterCount = 12;
inline constexpr uint32_t kRetailCharacterCount = 10;

enum class PartyArray : uint8_t {
  kPosition,   // u32 display position, 0 when not in the party
  kSlotBytes,  // u8, kept equal to the position while one is assigned
  kCharFlags,  // u8, pending award queue
  kStatsLive,  // 48 byte stats with equipment folded in
  kStatsBase,  // 48 byte stats, what a save holds
  kCharWords,  // u16, pending award queue
  kTemplate,   // 136 byte starting stats, read only image data
  kCount,
};

// Guest address of character `index`'s (0 based) entry in the relocated array,
// or of the retail array before the relocation exists. Host code that reads
// or writes party state goes through this, never the retail addresses.
uint32_t PartyArrayAddress(PartyArray array, uint32_t index = 0);

// Makes the native memcpy, memmove and memset family remap the same ranges.
// Call before the guest runs (OnPostSetup).
void InitPartyArrays(rex::Runtime* runtime);

}  // namespace eternalsonata
