// eternalsonata - BattleKeep.bop from the PS3.
//
// sub_821A0F18 copies BattleKeep.bop's entry pointers into a slot array that
// executable code and battle records index by 360 slot. The PS3 dropped the
// effects at 360 slots 26..33 and moved every later slot down by eight, yet
// its battle files keep the 360's ids, so in PS3 mode the array is reordered
// into the 360's layout. The converter appends the eight dropped effects
// after the PS3's entries (docs/ps3-assets.md section 4).
//
// The voice table (360 slot 38) is the PS3's: twelve character rows before
// the enemies and two more categories after 40, which the PS3 engine asks
// for as the 360's plus 2 (EBOOT sub_175248 against sub_821AB7E0).

#include "button_glyphs.h"
#include "character_roster.h"
#include "generated/eternalsonata_init.h"
#include "target.h"

#include <array>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

constexpr uint32_t kSlotArray = 0x2C9B0u;
constexpr uint32_t kSlotCount = 0x2C9ACu;
constexpr uint32_t kSlots360 = 106;
constexpr uint32_t kSlotsPs3 = 98;
constexpr uint32_t kDroppedFirst = 26;
constexpr uint32_t kDroppedEnd = 34;
constexpr uint32_t kVoiceTable = 38;
constexpr uint32_t kTagBmd = 0x424D4420u;  // "BMD "

// sub_821ABC68 rows: character id - 1 on both, enemy id + 9 on the 360 and
// + 11 on the PS3 (EBOOT sub_1A6A98).
constexpr int32_t kPs3EnemyRows = 2;
constexpr int32_t kFirstShiftedCategory = 41;
constexpr uint32_t kPs3NewCategories = 2;

// The file slot that holds a 360 slot.
uint32_t FileSlot(uint32_t slot) {
  if (slot < kDroppedFirst)
    return slot;
  if (slot < kDroppedEnd)
    return kSlotsPs3 + slot - kDroppedFirst;
  return slot - (kDroppedEnd - kDroppedFirst);
}

}  // namespace

REX_EXTERN(__imp__sub_821A0F18);

// BOP parser (object).
REX_HOOK_RAW(sub_821A0F18) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_821A0F18(ctx, base);
  if (!ctx.r3.u32)
    return;
  if (!eternalsonata::IsPs3Target()) {
    button_glyphs::OnBattleKeepLoaded(object + kSlotArray);
    return;
  }
  const uint32_t slots = object + kSlotArray;
  const uint32_t count = REX_LOAD_U32(object + kSlotCount);
  std::array<uint32_t, kSlots360> file{};
  for (uint32_t i = 0; i < kSlots360 && i < count; ++i)
    file[i] = REX_LOAD_U32(slots + 4 * i);
  const uint32_t voice = file[FileSlot(kVoiceTable)];
  if (count != kSlots360 || !voice || REX_LOAD_U32(voice) != kTagBmd) {
    REXLOG_ERROR("ps3 battlekeep: {} entries, not the PS3's plus the 360's eight; "
                 "unpack the PS3 files again",
                 count);
    return;
  }
  for (uint32_t slot = 0; slot < kSlots360; ++slot)
    REX_STORE_U32(slots + 4 * slot, file[FileSlot(slot)]);
  button_glyphs::OnBattleKeepLoaded(slots);
}

REX_EXTERN(__imp__sub_821ABC68);

// Voice table row of a battle unit (r4: u32 kind, u8 index). On 360 data the
// rows past ten are enemies', so a modded character speaks with its base's
// row; a line from an unloaded bank never ends and holds the battle intro.
REX_HOOK_RAW(sub_821ABC68) {
  const uint32_t unit = ctx.r4.u32;
  __imp__sub_821ABC68(ctx, base);
  const uint32_t kind = REX_LOAD_U32(unit);
  if (eternalsonata::IsPs3Target()) {
    if (kind == 1 && ctx.r3.s32 >= 0)
      ctx.r3.s64 = ctx.r3.s32 + kPs3EnemyRows;
  } else if (kind == 0 && ctx.r3.s32 >= 10) {
    ctx.r3.s64 = eternalsonata::CharacterBase(ctx.r3.s32 + 1) - 1;
  }
}

namespace {

// Marks a category already in the PS3's numbering.
constexpr uint32_t kPs3Category = 0x100u;

constexpr uint32_t kBattleRecord = 0x8253923Cu;
constexpr uint32_t kPartyCount = 0x824D0720u;
constexpr uint32_t kPartyArray = 0x824FD1A0u;
constexpr uint32_t kPartyStride = 81972u;

// The pre-battle reply by a fixed member: encounter, first and fallback
// character, category in PS3 numbering (EBOOT sub_19FC80). The 360's table
// (sub_821BAB70) asks Jazz for 38 against Waltz, which the PS3 left empty,
// and the line never ends.
struct FixedReply {
  int16_t encounter;
  uint32_t character, fallback, category;
};
constexpr FixedReply kPs3FixedReplies[] = {
    {151, 8, 0, 38}, {157, 2, 0, 38}, {161, 1, 0, 38}, {162, 3, 0, 38}, {168, 8, 0, 38},
    {169, 7, 1, 39}, {170, 7, 1, 41}, {171, 2, 1, 42}, {313, 2, 0, 41},
};

int32_t PartySlot(uint8_t* base, uint32_t character) {
  const uint32_t count = REX_LOAD_U8(kPartyCount);
  for (uint32_t i = 0; character && i < count; ++i)
    if (REX_LOAD_U32(kPartyArray + kPartyStride * i) == character)
      return int32_t(i);
  return -1;
}

}  // namespace

REX_EXTERN(__imp__sub_821BAB70);

// Battle intro setup (intro): +8 the replying member, +12 its category.
REX_HOOK_RAW(sub_821BAB70) {
  const uint32_t intro = ctx.r3.u32;
  __imp__sub_821BAB70(ctx, base);
  const uint32_t record = REX_LOAD_U32(kBattleRecord);
  if (!eternalsonata::IsPs3Target() || !record)
    return;
  const auto encounter = static_cast<int16_t>(REX_LOAD_U16(record));
  for (const FixedReply& reply : kPs3FixedReplies) {
    if (reply.encounter != encounter)
      continue;
    int32_t slot = PartySlot(base, reply.character);
    if (slot < 0)
      slot = PartySlot(base, reply.fallback);
    if (slot >= 0) {
      REX_STORE_U32(intro + 8, uint32_t(slot));
      REX_STORE_U32(intro + 12, reply.category < kFirstShiftedCategory
                                    ? reply.category
                                    : reply.category | kPs3Category);
    }
    return;
  }
}

REX_EXTERN(__imp__sub_821BCC40);

// Voice pick (table, row, category, ...).
REX_HOOK_RAW(sub_821BCC40) {
  if (eternalsonata::IsPs3Target()) {
    if (ctx.r5.u32 & kPs3Category)
      ctx.r5.u64 = ctx.r5.u32 & ~kPs3Category;
    else if (ctx.r5.s32 >= kFirstShiftedCategory)
      ctx.r5.u64 = ctx.r5.u32 + kPs3NewCategories;
  }
  __imp__sub_821BCC40(ctx, base);
}
