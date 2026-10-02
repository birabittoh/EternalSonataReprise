// eternalsonata - PS3 controller button prompts.
//
// PS3 text names buttons with icons the 360 markup lacks: <ibN> is icon
// N + 54 and <ib> the decide button (EBOOT 0x28B5AC). Its icon table
// (0x46B1C8) gives circle, cross and triangle for <ib1>..<ib3>. The 360 parses
// only <iN>, so in PS3 mode the tags become <i55>.. and <i0> before the
// preprocessor.
//
// Both releases turn <iN> into control code 25, but only the PS3 adds the
// icon to the line's glyphs; the 360's code 25 places a sprite that never
// shows, and no 360 text uses it. The 360's glyph icons are code 24, the
// arrows, so PS3 icons are laid out by that case instead.
//
// The PS3 battle tutorial asks native 45039 whether attack is on circle and
// then highlights circle, where the 360 asks whether it is on A. On a 360 pad
// that question is "attack is on B".

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <cstdint>
#include <mutex>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

namespace {

constexpr uint32_t kAttackOnB = 0x8243FC01u;
constexpr uint32_t kAppKeep = 0x82420AFCu;

// AppKeep keeps the 360 layout with the PS3's glyphs over A, B and Y
// (docs/ps3-assets.md section 1).
constexpr uint32_t kCross = 235;
constexpr uint32_t kCircle = 236;
constexpr uint32_t kTriangle = 238;

// <ib> is icon 0, the decide button: A, so cross.
constexpr uint32_t kFirstPs3Icon = 55;
constexpr uint32_t kPs3Icons[] = {kCircle, kCross, kTriangle};
constexpr uint32_t kPs3IconCount = sizeof(kPs3Icons) / sizeof(kPs3Icons[0]);

int32_t AppKeepSlot(uint32_t icon) {
  if (icon == 0)
    return kCross;
  if (icon >= kFirstPs3Icon && icon < kFirstPs3Icon + kPs3IconCount)
    return kPs3Icons[icon - kFirstPs3Icon];
  return -1;
}

// After its first frame an icon's argument holds its sprite instead, so the
// record's icons are remembered by (record, argument index).
std::mutex g_mutex;
std::unordered_set<uint64_t> g_icons;

uint64_t Key(uint32_t record, uint32_t index) {
  return (uint64_t{record} << 8) | (index & 0xFF);
}

bool IsIcon(uint32_t record, uint32_t index) {
  std::lock_guard lock(g_mutex);
  return g_icons.count(Key(record, index)) != 0;
}

void Forget(uint32_t record) {
  std::lock_guard lock(g_mutex);
  for (uint32_t index = 0; index < 256; ++index)
    g_icons.erase(Key(record, index));
}

template <typename T>
T Load(uint32_t address) {
  auto* memory = rex::Runtime::instance()->memory();
  return rex::memory::load_and_swap<T>(memory->TranslateVirtual<uint8_t*>(address));
}

}  // namespace

// Layout pass sub_821D5CC0, code 25 case: r25 is the argument index, r31 the
// record. True sends the icon to the code 24 case.
extern "C++" bool EternalSonataPs3IconCase(PPCRegister& r25, PPCRegister& r31);

bool EternalSonataPs3IconCase(PPCRegister& r25, PPCRegister& r31) {
  if (!eternalsonata::IsPs3Target())
    return false;
  const uint32_t index = r25.u32 & 0xFF;
  const uint32_t record = r31.u32;
  if (Load<uint8_t>(record + 0x20C + 8 * index))
    return IsIcon(record, index);
  const uint32_t icon = Load<uint32_t>(record + 8 * (index + 65)) & 0xFFFF;
  if (AppKeepSlot(icon) < 0)
    return false;
  std::lock_guard lock(g_mutex);
  g_icons.insert(Key(record, index));
  return true;
}

// Code 24 loads the arrow's texture into r4 from its argument, r10 = 4 * arg.
extern "C++" void EternalSonataPs3IconTexture(PPCRegister& r4, PPCRegister& r10);

void EternalSonataPs3IconTexture(PPCRegister& r4, PPCRegister& r10) {
  if (!eternalsonata::IsPs3Target())
    return;
  const int32_t slot = AppKeepSlot(r10.u32 / 4);
  if (slot >= 0)
    r4.u64 = Load<uint32_t>(kAppKeep + 4 * slot);
}

// Code 24 tints the arrow with the text colour in r10; the PS3 draws its
// icons untinted.
extern "C++" void EternalSonataPs3IconColour(PPCRegister& r10, PPCRegister& r27,
                                             PPCRegister& r31);

void EternalSonataPs3IconColour(PPCRegister& r10, PPCRegister& r27, PPCRegister& r31) {
  if (eternalsonata::IsPs3Target() && IsIcon(r31.u32, r27.u32 & 0xFF))
    r10.u64 = (r10.u32 & 0xFF000000u) | 0x00FFFFFFu;
}

// The markup preprocessor (docs/script-vm-notes.md section 3) expands the
// record's text in place. The rewrite keeps every length.
REX_EXTERN(__imp__sub_821D50A8);

REX_HOOK_RAW(sub_821D50A8) {
  const uint32_t mgr = ctx.r3.u32;
  const uint32_t record = ctx.r4.u32;
  if (eternalsonata::IsPs3Target() && record) {
    Forget(record);
    const uint32_t buffer =
        REX_LOAD_U32(record) == REX_LOAD_U32(mgr + 4 * 9037) ? mgr + 35122 : record + 8;
    uint32_t at = buffer + REX_LOAD_U16(record + 420);
    for (uint8_t c; (c = REX_LOAD_U8(at)) != 0; ++at) {
      if (c != '<' || REX_LOAD_U8(at + 1) != 'i' || REX_LOAD_U8(at + 2) != 'b')
        continue;
      const uint8_t n = REX_LOAD_U8(at + 3);
      if (n == '>') {
        REX_STORE_U8(at + 2, '0');
        REX_STORE_U8(at + 3, '>');
      } else if (n >= '1' && n < '1' + kPs3IconCount && REX_LOAD_U8(at + 4) == '>') {
        const uint32_t icon = kFirstPs3Icon + (n - '1');
        REX_STORE_U8(at + 2, '0' + icon / 10);
        REX_STORE_U8(at + 3, '0' + icon % 10);
      }
    }
  }
  __imp__sub_821D50A8(ctx, base);
}

// Native 45039: whether attack is on A on the 360, on circle on the PS3.
REX_EXTERN(__imp__sub_820E7640);

REX_HOOK_RAW(sub_820E7640) {
  if (!eternalsonata::IsPs3Target())
    return __imp__sub_820E7640(ctx, base);
  ctx.r3.u64 = REX_LOAD_U8(kAttackOnB) != 0;
}
