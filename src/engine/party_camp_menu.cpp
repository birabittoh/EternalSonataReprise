// eternalsonata - Characters 11 and 12 in the camp menu's member panels.
//
// The panel builders pick a name id and a portrait per character from tables
// and switches sized for ten. config/party.toml hooks them with these, which
// follow the PS3's own builders (sub_1E0B88 is sub_821DDD00's twin).

#include <bit>
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "party_arrays.h"
#include "party_system.h"
#include "ps3_appkeep.h"
#include "target.h"

namespace {

// AppKeep image ids are the slot plus one.
constexpr uint32_t kPortraitId = eternalsonata::kPs3MenuPortraitSlot + 1;

constexpr uint32_t kMenuIndexOffset = 2833;
constexpr uint32_t kLayoutTableOffset = 709;
constexpr uint32_t kElementCountOffset = 880;
constexpr uint32_t kCurrentHeap = 0x824408D8u;
constexpr uint32_t kHeapDepthOffset = 56;

struct ExtraHealthBars {
  uint32_t layout = 0;
  uint32_t ids[2] = {0, 0};
  uint32_t count = 0;
};

ExtraHealthBars g_extra_health_bars;
uint32_t g_extra_health_bar_teardown_end = 0;
uint32_t g_pending_extra_health_bars = 0;
thread_local bool g_allocating_extra_health_bar = false;

uint32_t CurrentLayout(uint8_t* base, uint32_t owner) {
  const uint32_t menu = REX_LOAD_U8(owner + kMenuIndexOffset);
  return REX_LOAD_U32(owner + 4 * (menu + kLayoutTableOffset));
}

void RememberExtraHealthBar(uint8_t* base, uint32_t owner, uint32_t id) {
  const uint32_t layout = CurrentLayout(base, owner);
  if (g_extra_health_bars.layout != layout)
    g_extra_health_bars = {.layout = layout};
  if (g_extra_health_bars.count < 2)
    g_extra_health_bars.ids[g_extra_health_bars.count++] = id;
}

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

// Save rows draw the party from the ten entry face table at 0x822FF530, read
// at `c << 2`. The PS3 appends CRS and SRN's faces to that set (its 290, 291);
// a vacant 360 slot gets Allegretto's.
extern "C++" void PartySaveRowPortrait(PPCRegister& offset, PPCRegister& image) {
  const uint32_t c = offset.u32 >> 2;
  if (c <= 10 || c > 12)
    return;
  uint32_t id = 0;
  if (eternalsonata::IsPs3Target())
    id = eternalsonata::Ps3AppKeepImageId(291 + c - 11);
  image.u64 = id ? id : 0x112u;
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
REX_EXTERN(__imp__sub_820C0000);
REX_EXTERN(__imp__sub_821F09A0);
REX_EXTERN(sub_821125A0);

// The screen heap is also sized for ten fills. Allocate the two extras from
// its parent heap, whose allocator header still gives destruction its owner.
REX_HOOK_RAW(sub_820C0000) {
  const uint32_t size = ctx.r3.u32;
  __imp__sub_820C0000(ctx, base);
  if (!g_allocating_extra_health_bar || ctx.r3.u32)
    return;

  const uint32_t heap = REX_LOAD_U32(kCurrentHeap);
  const uint32_t depth = heap ? REX_LOAD_U32(heap + kHeapDepthOffset) : 0;
  const uint32_t parent = depth ? REX_LOAD_U32(heap + 4 * (depth + 5)) : 0;
  if (!parent)
    return;
  ctx.r3.u64 = parent;
  ctx.r4.u64 = size;
  ctx.r5.u64 = 16;
  sub_821125A0(ctx, base);
}

// The retail layout has ten ownership slots followed immediately by other
// fields. Let the creator use that boundary as a temporary slot, then keep the
// returned id in sidecar storage while restoring every byte it touched there.
REX_HOOK_RAW(sub_821F09A0) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t layout = CurrentLayout(base, owner);
  if (!layout || !g_pending_extra_health_bars ||
      REX_LOAD_U8(layout + kElementCountOffset) != 10) {
    __imp__sub_821F09A0(ctx, base);
    return;
  }

  const uint32_t following_fields = REX_LOAD_U32(layout + kElementCountOffset);
  REX_STORE_U32(layout + kElementCountOffset, UINT32_MAX);
  g_allocating_extra_health_bar = true;
  __imp__sub_821F09A0(ctx, base);
  g_allocating_extra_health_bar = false;
  REX_STORE_U32(layout + kElementCountOffset, following_fields);
  if (ctx.r3.u32 != UINT32_MAX) {
    RememberExtraHealthBar(base, owner, ctx.r3.u32);
    --g_pending_extra_health_bars;
  }
}

// Put the saved ids after the retail list once its following field is gone.
extern "C++" void PartyAppendExtraHealthBars(PPCRegister& layout) {
  if (!g_extra_health_bars.count || layout.u32 != g_extra_health_bars.layout)
    return;

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (uint32_t i = 0; i < g_extra_health_bars.count; ++i)
    REX_STORE_U32(layout.u32 + kElementCountOffset + 4 * i,
                  g_extra_health_bars.ids[i]);
  g_extra_health_bar_teardown_end =
      kElementCountOffset + 4 * g_extra_health_bars.count;
  g_extra_health_bars = {};
}

// Extend the retail cleanup loop only while it owns the saved ids.
extern "C++" void PartyExtraHealthBarTeardownLimit(PPCCRRegister& cr,
                                                    PPCRegister& offset) {
  if (!g_extra_health_bar_teardown_end)
    return;
  cr.lt = offset.u32 < g_extra_health_bar_teardown_end;
  cr.gt = offset.u32 > g_extra_health_bar_teardown_end;
  cr.eq = offset.u32 == g_extra_health_bar_teardown_end;
  if (!cr.lt)
    g_extra_health_bar_teardown_end = 0;
}

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
// grid's last row has room for two more, at the row's x step.
REX_HOOK_RAW(sub_821DD808) {
  const uint32_t list = ctx.r3.u32;
  g_pending_extra_health_bars = 0;
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
    ++g_pending_extra_health_bars;
    end = ctx.r3.u32;
  }
  ctx = saved;
}
