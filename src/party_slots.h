// eternalsonata - The registry behind the two vacant character slots.
//
// The port widens every per-character table from ten entries to twelve (see
// party_relocation.h) but puts nothing in the two new ones. They are vacant:
// the host holds the storage, a mod supplies the content. This file is the one
// place that knows which of them a mod has claimed, and everything else asks
// here rather than testing an id against 11 or 12.
//
// Three kinds of caller:
//
//   - the mid-ASM id gates in party_bounds.cpp, on the guest thread, once per
//     stat recompute and once per battle model load. They read the defined mask
//     and nothing else, which is why it is a plain atomic bitmask rather than
//     something behind the registry mutex.
//   - party_system.cpp, for names and for refusing party edits on a vacant
//     slot.
//   - the public API in party_system.cpp, which defines and undefines.
#pragma once

#include <cstdint>
#include <string>

#include "eternalsonata_party_api.h"

namespace eternalsonata {

// Whether `character` has content: true for the whole retail cast, true for an
// added slot only once a mod has defined it. Safe from any thread, and cheap
// enough for the guest-thread id gates.
bool IsCharacterDefined(int character);

// The pcNNN.bop model number to load for `character`: the definition's
// model_id, or the character's own id when it did not set one. Guest thread.
int ModelIdForCharacter(int character);

// The name a defined added slot answers to, or an empty string. The retail cast
// is not in here; party_system.cpp holds its names.
std::string AddedCharacterName(int character);

// The starting own-stats a definition asked to be applied on first join, or
// false if it did not ask. Cleared once applied, so a character that levels up
// and leaves the party is not reset when it rejoins.
bool TakeStartingStats(int character, EternalSonataCharacterStats* out);

// Define / undefine. `definition` is copied. See the API header for what the
// results mean.
int DefineCharacter(int character, const EternalSonataCharacterDefinition* definition);
int DefineNextCharacter(const EternalSonataCharacterDefinition* definition);
int UndefineCharacter(int character);

// The added slots, as ids: ETERNALSONATA_CHAR_FIRST_ADDED upwards.
int AddedSlotCount();
int AddedSlotAt(int index);

}  // namespace eternalsonata
