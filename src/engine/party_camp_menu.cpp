// eternalsonata - Characters 11 and 12 in the camp menu's member panels.
//
// The panel builders pick a name id and a portrait per character from tables
// and switches sized for ten. config/party.toml hooks them with these, which
// follow the PS3's own builders (sub_1E0B88 is sub_821DDD00's twin).

#include <bit>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "party_arrays.h"
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


REX_EXTERN(__imp__sub_821E3408);
REX_EXTERN(__imp__sub_821DD808);

// sub_821E3408, the ten member grid panel, keeps the character in registers
// it reuses for other fields, so its name record is fixed in the output: the
// first type 200 record naming `c`.
REX_HOOK_RAW(sub_821E3408) {
  const uint32_t begin = ctx.r3.u32;
  const uint32_t c = ctx.r4.u32;
  __imp__sub_821E3408(ctx, base);
  if (c != 11 && c != 12)
    return;
  for (uint32_t at = begin; at + 4 < ctx.r3.u32; at += 4) {
    if (REX_LOAD_U32(at) == 200 && REX_LOAD_U32(at + 4) == c) {
      REX_STORE_U32(at + 4, eternalsonata::PartyNameSid(static_cast<int>(c), false));
      break;
    }
  }
}

// sub_821DD808 builds the camp's member panels and stops at position ten. The
// grid's last row has room for two more, at the row's x step. Their 1100
// record is dropped: sub_821F09A0 keeps those elements in a ten entry list
// inside the screen's layout object and fails the eleventh, leaving an
// element the scene draw then crashes on.
REX_HOOK_RAW(sub_821DD808) {
  const uint32_t list = ctx.r3.u32;
  __imp__sub_821DD808(ctx, base);
  const uint32_t count = ctx.r3.u32;
  if (count <= eternalsonata::kRetailCharacterCount)
    return;
  const PPCContext saved = ctx;
  uint32_t end = list;
  while (REX_LOAD_U32(end) != 0xFFFF)
    end += 4;
  const auto f32 = [&](uint32_t address) { return std::bit_cast<float>(REX_LOAD_U32(address)); };
  const float x0 = f32(0x820AA764u);
  const float step = f32(0x820AA760u);
  const float y = f32(0x820AA750u);
  for (uint32_t pos = eternalsonata::kRetailCharacterCount + 1;
       pos <= count && pos <= eternalsonata::kPartyCharacterCount; ++pos) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < eternalsonata::kPartyCharacterCount && !c; ++i) {
      if (REX_LOAD_U32(eternalsonata::PartyArrayAddress(eternalsonata::PartyArray::kPosition, i)) ==
          pos)
        c = i + 1;
    }
    if (!c)
      continue;
    ctx.r3.u64 = end;
    ctx.r4.u64 = c;
    ctx.r7.u64 = pos - 1;
    ctx.f1.f64 = x0 + step * static_cast<float>(pos - eternalsonata::kRetailCharacterCount);
    ctx.f2.f64 = y;
    sub_821E3408(ctx, base);
    const uint32_t panel_end = ctx.r3.u32;  // its 0xFFFF
    for (uint32_t at = end; at + 4 < panel_end; at += 4) {
      if (REX_LOAD_U32(at) == 1100 && REX_LOAD_U32(at + 4) == c) {
        constexpr uint32_t kRecord = 6 * 4;
        for (uint32_t from = at + kRecord; from <= panel_end; from += 4)
          REX_STORE_U32(from - kRecord, REX_LOAD_U32(from));
        ctx.r3.u64 = panel_end - kRecord;
        break;
      }
    }
    end = ctx.r3.u32;
  }
  ctx = saved;
}

