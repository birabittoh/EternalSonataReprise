// eternalsonata - Characters 11 and 12 in the camp menu's member panels.
//
// The panel builders pick a name id and a portrait per character from tables
// and switches sized for ten. config/party.toml hooks them with these, which
// follow the PS3's own builders (sub_1E0B88 is sub_821DDD00's twin).

#include <rex/ppc/context.h>

#include "party_system.h"
#include "ps3_appkeep.h"
#include "target.h"

namespace {

// AppKeep image ids are the slot plus one.
constexpr uint32_t kPortraitId = eternalsonata::kPs3MenuPortraitSlot + 1;

}  // namespace

// After `addi r, c, 10` forms a ruby name id: past the ten names it would
// read the menu labels that follow them.
extern "C++" void PartyRubyNameSid(PPCRegister& sid) {
  if (sid.u32 > 20 && sid.u32 <= 22)
    sid.u64 = eternalsonata::PartyNameSid(static_cast<int>(sid.u32) - 10, true);
}

// Before a plain name id `c` is stored: past ten it would read the ruby
// names that follow.
extern "C++" void PartyPlainNameSid(PPCRegister& sid) {
  if (sid.u32 > 10 && sid.u32 <= 12)
    sid.u64 = eternalsonata::PartyNameSid(static_cast<int>(sid.u32), false);
}

// After `cmpwi cr, c, 8` choosing between a plain and a ruby name: the PS3
// gives 11 and 12 the ruby one, with its offset.
extern "C++" void PartyRubyNameCompare(PPCCRRegister& cr, PPCRegister& c) {
  if (c.u32 > 10 && c.u32 <= 12)
    cr.eq = true;
}

// sub_821DDD00, the one to three member panel. The PS3 lays out Crescendo
// like Viola and Serenade like Salsa, with their own portraits.
extern "C++" bool PartyPanelPortraitCrs(PPCRegister& c, PPCRegister& image) {
  if (c.u32 != 11 || !eternalsonata::IsPs3Target())
    return false;
  image.u64 = kPortraitId;
  return true;
}

// After Viola's path stores the image: the register is also every text
// record's type, 200, from here on.
extern "C++" void PartyPanelTextType(PPCRegister& type) {
  type.u64 = 200;
}

extern "C++" bool PartyPanelPortraitSrn(PPCRegister& c, PPCRegister& image,
                                        PPCRegister& height) {
  if (c.u32 != 12 || !eternalsonata::IsPs3Target())
    return false;
  image.u64 = kPortraitId + 1;
  height.u64 = 12;
  return true;
}

