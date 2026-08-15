// eternalsonata - The id range checks that reject characters 11 and 12.
//
// The last piece of the ten-to-twelve conversion, and the easy one. Three
// functions gate on the character id before doing anything, and all three
// reject anything outside 1..10 outright:
//
//   sub_821E7898  the stat recompute every status and equipment screen depends
//                 on. Returns immediately unless 1 <= id <= 10.
//   sub_821A03D0  the battle-party model loader, which formats
//                 "btldata\player\pc%03d.bop". Rejects `id - 1 > 9` before it
//                 gets as far as the name. An asset mod can supply pc011.bop
//                 and pc012.bop; this is what stops the game asking for them.
//   sub_820E78B8  the party menu's "give a character the next free display
//                 position" routine, which the mod API calls to add a member.
//                 Gates the 0-based index on `*a1 <= 9`, so a placement for
//                 indices 10 and 11 fell through silently - the character was
//                 rostered but got no display position, which read as "nothing
//                 happened" in the overlay.
//
// All three are `cmpwi`/`cmplwi` against a literal, which a mid-ASM hook cannot
// rewrite. So all three hooks fire *before* the compare and take the accept
// path themselves via jump_address_on_true when the id is one of the two new
// ones, leaving the original instruction to handle the first ten exactly as
// before. Jumping past the compare leaves cr6 holding whatever it held; in all
// three cases the next reader of cr6 on the accept path is preceded by its own
// compare (or by a call, which makes cr6 volatile anyway), so nothing observes
// the skip.
//
// A whole-function override was considered for sub_821E7898, since it is fully
// understood, but it is a thousand-odd bytes of stat and equipment maths that
// would then have to be maintained by hand for no gain over a two-instruction
// hook.
//
// The matching [[midasm_hook]] blocks are in eternalsonata_config.toml, in the
// "src/party_bounds.cpp" section. Addresses and options:
//
//   address     hook                          jump_address_on_true
//   0x821E78AC  PartyBounds_ExtendedId(r11)   0x821E78B4   sub_821E7898 stat recompute
//   0x821A0528  PartyBounds_ExtendedIndex(r11) 0x821A0530  sub_821A03D0 model loader
//   0x820E78D0  PartyBounds_ExtendedIndex(r11) 0x820E78D8  sub_820E78B8 position place
//
// All three fire before the instruction at the listed address.

#include "party_relocation.h"

#include <rex/ppc/context.h>

namespace {

using eternalsonata::kRelocatedCharacterCount;

constexpr uint32_t kRetailCharacterCount = 10;

}  // namespace

// The hook bodies are looked up by name by the recompiler, so they keep
// external linkage.

// `cmpwi cr6, r11, 0xA` / `bgt` in sub_821E7898, on a 1-based character id that
// the `cmpwi r11, 1` above has already floored. Only the new ids need the jump;
// 1..10 fall through to the original branch, which does not take it.
bool PartyBounds_ExtendedId(PPCRegister& id) {
  return id.u32 > kRetailCharacterCount && id.u32 <= kRelocatedCharacterCount;
}

// `cmplwi cr6, r11, 9` / `bgt` in sub_821A03D0, on `id - 1` rather than on the
// id. The compare is unsigned, so id 0 has already wrapped to a huge number and
// is rejected by the same branch; keeping the test in terms of the index
// preserves that. The same accept set covers sub_820E78B8's gate on the 0-based
// index of the character being placed into the next free display position:
// there the compare is `cmplwi cr6, r11, 9` / `bgt` over the whole body, and
// jumping straight in for indices 10 and 11 is what lets characters 11 and 12
// into the party at all.
bool PartyBounds_ExtendedIndex(PPCRegister& index) {
  return index.u32 >= kRetailCharacterCount &&
         index.u32 < kRelocatedCharacterCount;
}
