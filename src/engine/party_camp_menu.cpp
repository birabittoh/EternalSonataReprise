// eternalsonata - Characters 11 and 12 in the camp menu's member panels, and
// the worn costume's panel art for everyone.
//
// The panel builders pick a name id and a portrait per character from tables
// and switches sized for ten. config/party.toml hooks them with these, which
// follow the PS3's own builders (sub_1E0B88 is sub_821DDD00's twin).

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "costume_system.h"
#include "eternalsonata_costume_api.h"
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

// One per live layout: the item target list opens over the camp grid.
std::array<ExtraHealthBars, 4> g_extra_health_bars;
uint32_t g_extra_health_bar_teardown_end = 0;
uint32_t g_pending_extra_health_bars = 0;
// Set while a failed screen heap allocation may come from its parent heap.
thread_local bool g_screen_heap_fallback = false;

// The worn costume's one to three member panel art, 0 for the character's own.
// From Viola on the panel shows the status page's art, so a costume without
// panel art shows its status portrait there.
uint32_t CostumePanel(uint32_t c) {
  const int character = static_cast<int>(c);
  uint32_t id = eternalsonata::CostumePortrait(character, ETERNALSONATA_COSTUME_PORTRAIT_PANEL);
  if (!id && c > 4)
    id = eternalsonata::CostumePortrait(character, ETERNALSONATA_COSTUME_PORTRAIT_STATUS);
  return id;
}

uint32_t CurrentLayout(uint8_t* base, uint32_t owner) {
  const uint32_t menu = REX_LOAD_U8(owner + kMenuIndexOffset);
  return REX_LOAD_U32(owner + 4 * (menu + kLayoutTableOffset));
}

ExtraHealthBars* FindExtraHealthBars(uint32_t layout) {
  for (auto& bars : g_extra_health_bars)
    if (bars.count && bars.layout == layout)
      return &bars;
  return nullptr;
}

void RememberExtraHealthBar(uint8_t* base, uint32_t owner, uint32_t id) {
  const uint32_t layout = CurrentLayout(base, owner);
  ExtraHealthBars* bars = FindExtraHealthBars(layout);
  for (auto& free : g_extra_health_bars)
    if (!bars && !free.count)
      bars = &free;
  if (!bars)
    bars = &g_extra_health_bars[0];
  if (bars->layout != layout)
    *bars = {.layout = layout};
  if (bars->count < 2)
    bars->ids[bars->count++] = id;
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

// After the last `cmpwi cr, i, n` choosing a ruby name by 0 based index (the
// item target list's): 11 and 12 take the ruby one, as in the camp panels.
extern "C++" void PartyRubyNameIndexCompare(PPCCRRegister& cr, PPCRegister& i) {
  if (i.u32 >= 10 && i.u32 < 12) {
    cr.lt = false;
    cr.gt = false;
    cr.eq = true;
  }
}

// The item target list's one to three member panel: like sub_821DDD00, but
// Salsa's image id is built in the register holding c.
extern "C++" bool PartyTargetPortraitSrn(PPCRegister& c) {
  if (c.u32 != 12 || !eternalsonata::IsPs3Target())
    return false;
  c.u64 = kPortraitId + 1;
  return true;
}

extern "C++" bool PartyPanelPortraitSrn(PPCRegister& c, PPCRegister& image,
                                        PPCRegister& height) {
  if (c.u32 != 12 || !eternalsonata::IsPs3Target())
    return false;
  image.u64 = kPortraitId + 1;
  height.u64 = 12;
  return true;
}

// sub_82237A68, the one to three member swap, picks each panel's portrait by
// a switch on the character; as in sub_821DDD00, Crescendo takes Viola's case
// and Serenade Salsa's, then their own image after the case loads its id.
extern "C++" bool PartySwapAsViola(PPCRegister& c) {
  return c.u32 == 11 && eternalsonata::IsPs3Target();
}

extern "C++" bool PartySwapAsSalsa(PPCRegister& c) {
  return c.u32 == 12 && eternalsonata::IsPs3Target();
}

extern "C++" void PartySwapPortrait(PPCRegister& c, PPCRegister& image) {
  if (const uint32_t costume = CostumePanel(c.u32))
    image.u64 = costume;
  else if ((c.u32 == 11 || c.u32 == 12) && eternalsonata::IsPs3Target())
    image.u64 = kPortraitId + c.u32 - 11;
}

// The item target list's one to three member panel, once a case has stored
// the image record at `record`.
extern "C++" void PartyTargetPanelCostume(PPCRegister& index, PPCRegister& record) {
  if (const uint32_t costume = CostumePanel(index.u32 + 1)) {
    uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
    REX_STORE_U32(record.u32 + 4, costume);
  }
}

REX_EXTERN(__imp__sub_821DDD00);

// sub_821DDD00(list, c, ...): every case leaves the panel art in the record at
// list + 48, {100, image, ...}.
REX_HOOK_RAW(sub_821DDD00) {
  const uint32_t list = ctx.r3.u32;
  const uint32_t c = ctx.r4.u32;
  __imp__sub_821DDD00(ctx, base);
  if (const uint32_t costume = CostumePanel(c))
    REX_STORE_U32(list + 52, costume);
}

REX_EXTERN(__imp__sub_821E3408);
REX_EXTERN(__imp__sub_821DD808);
REX_EXTERN(__imp__sub_8221B5A0);
REX_EXTERN(__imp__sub_820C0000);
REX_EXTERN(__imp__sub_821F09A0);
REX_EXTERN(__imp__sub_821EED00);
REX_EXTERN(sub_821125A0);

// The screen heap is sized for ten members. Allocations for 11 and 12 that it
// cannot hold come from its parent heap, whose allocator header still gives
// destruction its owner.
REX_HOOK_RAW(sub_820C0000) {
  const uint32_t size = ctx.r3.u32;
  __imp__sub_820C0000(ctx, base);
  if (!g_screen_heap_fallback || ctx.r3.u32)
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
  g_screen_heap_fallback = true;
  __imp__sub_821F09A0(ctx, base);
  g_screen_heap_fallback = false;
  REX_STORE_U32(layout + kElementCountOffset, following_fields);
  if (ctx.r3.u32 != UINT32_MAX) {
    RememberExtraHealthBar(base, owner, ctx.r3.u32);
    --g_pending_extra_health_bars;
  }
}

// Put the saved ids after the retail list once its following field is gone.
extern "C++" void PartyAppendExtraHealthBars(PPCRegister& layout) {
  ExtraHealthBars* bars = FindExtraHealthBars(layout.u32);
  if (!bars)
    return;

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (uint32_t i = 0; i < bars->count; ++i)
    REX_STORE_U32(layout.u32 + kElementCountOffset + 4 * i, bars->ids[i]);
  g_extra_health_bar_teardown_end = kElementCountOffset + 4 * bars->count;
  *bars = {};
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

uint32_t PartyMemberCount(uint8_t* base) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < eternalsonata::kPartyCharacterCount; ++i)
    n += REX_LOAD_U32(eternalsonata::PartyArrayAddress(eternalsonata::PartyArray::kPosition, i)) != 0;
  return n;
}

// sub_8221B5A0, the item target list, has the camp grid's layout: rows of
// three, and position ten alone on the last row, which has room for 11 and 12.
// Their health fills go to the same sidecar as the camp grid's.
REX_HOOK_RAW(sub_8221B5A0) {
  const uint32_t count = ctx.r4.u32;
  __imp__sub_8221B5A0(ctx, base);
  const uint32_t members = PartyMemberCount(base);
  if (count == eternalsonata::kRetailCharacterCount &&
      members > eternalsonata::kRetailCharacterCount)
    g_pending_extra_health_bars = members - eternalsonata::kRetailCharacterCount;
}

// After the ten member loop's `cmpwi cr, i, 10`: run to the member count.
extern "C++" void PartyCompareMemberCount(PPCCRRegister& cr, PPCRegister& i) {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const int32_t members = static_cast<int32_t>(
      std::min(PartyMemberCount(base), eternalsonata::kPartyCharacterCount));
  cr.lt = i.s32 < members;
  cr.gt = i.s32 > members;
  cr.eq = i.s32 == members;
}

REX_EXTERN(__imp__sub_821F8F78);
REX_EXTERN(__imp__sub_821F9600);
REX_EXTERN(sub_821E8C78);
REX_EXTERN(sub_821DC238);
REX_EXTERN(sub_821D3890);
REX_EXTERN(sub_821F6BF0);
REX_EXTERN(sub_82112550);
REX_EXTERN(OBJECT__ctor);
REX_EXTERN(sub_821E91B8);

// The camp's element registry: 256 records of 20 bytes, {index, ?, handle, ?,
// element}, at *(dword_824400E4 + 1504) (sub_821F64E0, sub_821F6580).
uint32_t ElementRecord(uint8_t* base, uint32_t key, uint32_t field) {
  const uint32_t registry = REX_LOAD_U32(REX_LOAD_U32(0x824400E4u) + 1504);
  for (uint32_t i = 0; registry && i < 256; ++i)
    if (REX_LOAD_U32(registry + 20 * i + field) == key)
      return registry + 20 * i;
  return 0;
}

// The heal-all use struct (dword_824400F8) keeps its heap at +144 and the
// popup tasks of positions 1..10; those of 11 and 12 are kept here.
constexpr uint32_t kHealPopupVtable = 0x82087468u;
uint32_t g_heal_popup_use = 0;
std::array<uint32_t, 2> g_heal_popups{};
// Characters 11 and 12 healed by the last heal-all item, as bits (1 << c).
uint32_t g_heal_extra_healed = 0;

void PopHeap(uint8_t* base, uint32_t heap) {
  const uint32_t depth = REX_LOAD_U32(heap + kHeapDepthOffset);
  if (!depth)
    return;
  REX_STORE_U32(heap + kHeapDepthOffset, depth - 1);
  REX_STORE_U32(kCurrentHeap, REX_LOAD_U32(heap + 24 + 4 * (depth - 1)));
}

void StoreF32(uint8_t* base, uint32_t address, float value) {
  REX_STORE_U32(address, std::bit_cast<uint32_t>(value));
}

// sub_821F9600's popup task: counts the HP text from old to new while it
// flashes, over 150 frames.
uint32_t NewHealPopup(PPCContext& ctx, uint8_t* base, uint32_t use) {
  if (!REX_LOAD_U8(use + 256))
    return 0;
  ctx.r3.u64 = use + 144;
  sub_82112550(ctx, base);
  ctx.r3.u64 = 80;
  sub_820C0000(ctx, base);
  const uint32_t task = ctx.r3.u32;
  if (task) {
    ctx.r4.u64 = 7;
    ctx.r5.u64 = 0;
    OBJECT__ctor(ctx, base);
    REX_STORE_U32(task, kHealPopupVtable);
    REX_STORE_U32(task + 48, UINT32_MAX);
    REX_STORE_U32(task + 52, 0);
    StoreF32(base, task + 56, 0.0f);
    StoreF32(base, task + 60, 0.0f);
    StoreF32(base, task + 64, 0.0f);
    REX_STORE_U32(task + 72, 0);
    REX_STORE_U8(task + 76, 0);
  }
  PopHeap(base, use + 144);
  return task;
}

// sub_821F8F78 picks a heal-all item's targets into a struct with ten slots,
// so sub_821F9600 heals positions 1..10. Positions 11 and 12 are healed here
// once the item is spent, as it does: HP, a popup task on the panel's HP text
// (child 7 of the ten member panel) and the health fill.
REX_HOOK_RAW(sub_821F8F78) {
  __imp__sub_821F8F78(ctx, base);
  const uint32_t use = REX_LOAD_U32(0x824400F8u);
  if (!(ctx.r3.u32 & 0xFF) || !use || REX_LOAD_U32(use + 24) != 2)
    return;
  const int32_t item = static_cast<int16_t>(REX_LOAD_U16(use + 28));
  const uint32_t record = 0x82017630u + 100 * (item - 1);
  const int32_t flat = static_cast<int32_t>(REX_LOAD_U32(record + 0x3C));
  const int32_t percent = static_cast<int32_t>(REX_LOAD_U32(record + 0x40));
  const uint32_t layout = REX_LOAD_U32(REX_LOAD_U32(0x824400E4u) + 2840);
  const ExtraHealthBars* bars = layout ? FindExtraHealthBars(layout) : nullptr;

  const PPCContext saved = ctx;
  ctx.r1.u64 = ctx.r1.u32 - 0x100;
  const uint32_t text = ctx.r1.u32 + 0x80;
  g_heal_popup_use = use;
  g_heal_popups = {};
  g_heal_extra_healed = 0;
  using eternalsonata::PartyArray;
  using eternalsonata::PartyArrayAddress;
  for (uint32_t pos = eternalsonata::kRetailCharacterCount + 1;
       pos <= eternalsonata::kPartyCharacterCount; ++pos) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < eternalsonata::kPartyCharacterCount && !c; ++i)
      if (REX_LOAD_U32(PartyArrayAddress(PartyArray::kPosition, i)) == pos)
        c = i + 1;
    if (!c)
      continue;
    const uint32_t live = PartyArrayAddress(PartyArray::kStatsLive, c - 1);
    const int32_t old_hp = static_cast<int32_t>(REX_LOAD_U32(live + 0x0C));
    const int32_t max = static_cast<int32_t>(REX_LOAD_U32(live + 0x10));
    if (old_hp >= max)
      continue;
    const int32_t amount =
        static_cast<int32_t>(static_cast<float>(max) * 0.0099999998f * static_cast<float>(percent)) +
        flat;
    const uint32_t k = pos - eternalsonata::kRetailCharacterCount - 1;
    const uint32_t panel = ElementRecord(base, REX_LOAD_U32(layout + 4 * (pos + 2)), 8);
    const uint32_t child = panel ? ElementRecord(base, REX_LOAD_U32(panel) + 7, 0) : 0;
    const uint32_t task = child ? NewHealPopup(ctx, base, use) : 0;

    ctx.r3.u64 = c;
    ctx.r4.s64 = amount;
    sub_821E8C78(ctx, base);
    const int32_t hp = ctx.r3.s32;
    g_heal_extra_healed |= 1u << c;

    if (task) {
      g_heal_popups[k] = task;
      REX_STORE_U32(task + 48, REX_LOAD_U32(child + 16));
      REX_STORE_U32(task + 52, old_hp);
      REX_STORE_U32(task + 72, 6);
      REX_STORE_U8(task + 76, 0);
      StoreF32(base, task + 56, static_cast<float>(old_hp));
      StoreF32(base, task + 64, static_cast<float>(hp));
      StoreF32(base, task + 60,
               (static_cast<float>(hp) - static_cast<float>(old_hp)) * 0.0066666668f);
      REX_STORE_U8(task + 68, hp > old_hp);
      if (!REX_LOAD_U8(task + 36))
        REX_STORE_U8(task + 36, 1);
    } else if (child) {
      ctx.r3.s64 = hp;
      ctx.r4.u64 = text;
      ctx.r5.u64 = 6;
      ctx.r6.s64 = -1;
      sub_821DC238(ctx, base);
      ctx.r3.u64 = 0x82555690u;
      ctx.r4.u64 = REX_LOAD_U32(child + 16);
      ctx.r5.u64 = text;
      ctx.r6.s64 = -1;
      sub_821D3890(ctx, base);
    }
    const uint32_t fill =
        bars && k < bars->count ? ElementRecord(base, bars->ids[k], 8) : 0;
    if (fill) {
      ctx.r3.u64 = REX_LOAD_U32(fill + 16);
      ctx.r4.s64 = amount;
      ctx.r5.u64 = 150;
      sub_821F6BF0(ctx, base);
    }
  }
  ctx = saved;
}

// When sub_821F9600 is done it deletes its popup tasks; delete ours with them,
// before the struct's heap goes.
REX_HOOK_RAW(sub_821F9600) {
  const uint32_t use = ctx.r3.u32;
  __imp__sub_821F9600(ctx, base);
  if (!(ctx.r3.u32 & 0xFF) || use != g_heal_popup_use)
    return;
  const PPCContext saved = ctx;
  for (uint32_t& task : g_heal_popups) {
    if (!task)
      continue;
    ctx.r3.u64 = use + 144;
    sub_82112550(ctx, base);
    ctx.r3.u64 = task;
    ctx.r4.u64 = 1;
    sub_821E91B8(ctx, base);
    PopHeap(base, use + 144);
    task = 0;
  }
  g_heal_popup_use = 0;
  ctx = saved;
}

// sub_821EED00 creates a menu element, such as the flash a heal-all item puts
// over every member panel, on the screen heap. The panels for 11 and 12 use
// up its room for the last flashes, so those fall back to the parent heap too.
REX_HOOK_RAW(sub_821EED00) {
  const bool fallback = g_screen_heap_fallback;
  g_screen_heap_fallback = true;
  __imp__sub_821EED00(ctx, base);
  g_screen_heap_fallback = fallback;
}

// sub_82222438 flashes every member panel after a heal-all item; skip the
// members it did not heal (before `bl sub_821F6580`, r30 the 0 based position).
extern "C++" bool PartyHealFlashSkip(PPCRegister& index) {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t use = REX_LOAD_U32(0x824400F8u);
  if (!use || REX_LOAD_U32(use + 24) != 2)
    return false;
  uint32_t c = 0;
  for (uint32_t i = 0; i < eternalsonata::kPartyCharacterCount && !c; ++i)
    if (REX_LOAD_U32(eternalsonata::PartyArrayAddress(eternalsonata::PartyArray::kPosition, i)) ==
        index.u32 + 1)
      c = i + 1;
  if (!c || g_heal_extra_healed & (1u << c))
    return false;
  for (uint32_t i = 0; i < eternalsonata::kRetailCharacterCount; ++i)
    if (REX_LOAD_U16(use + 2 * i) == c)
      return false;
  return true;
}
