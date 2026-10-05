// eternalsonata: Characters 11 and 12 in battle.
//
// The battle reads a character's stats through the twelve wide arrays, so
// what remains are its own ten character switches. The model and cloth cases
// follow the PS3's sub_163010 (its twin of sub_821A2B38): Crescendo is model
// -20 named bCRS with a manto01_sp chain, Serenade -21 named bSRN.

#include <cstring>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "generated/eternalsonata_init.h"
#include "party_system.h"
#include "ps3_appkeep.h"
#include "target.h"

namespace {

constexpr uint32_t kCrescendo = 11;
constexpr uint32_t kSerenade = 12;

// A guest copy of a host string, made once.
uint32_t GuestString(uint32_t& at, const char* text) {
  if (at)
    return at;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return 0;
  const uint32_t size = static_cast<uint32_t>(std::strlen(text)) + 1;
  const uint32_t guest = memory->SystemHeapAlloc(size, 0x10);
  if (guest)
    std::memcpy(memory->TranslateVirtual(guest), text, size);
  at = guest;
  return at;
}

}  // namespace

// sub_821A2B38 before its model switch, r11 = character - 1: the model and
// the scene name the cases leave in r28 and r4.
extern "C++" bool PartyBattleModel(PPCRegister& r11, PPCRegister& r4, PPCRegister& r28) {
  if (!eternalsonata::IsPs3Target() || (r11.u32 != kCrescendo - 1 && r11.u32 != kSerenade - 1))
    return false;
  const uint32_t index = r11.u32 - (kCrescendo - 1);
  const uint32_t model = eternalsonata::Ps3AppKeep2Model(index);
  static uint32_t names[2];
  const uint32_t name = GuestString(names[index], index ? "bSRN" : "bCRS");
  if (!model || !name)
    return false;
  r28.u64 = model;
  r4.u64 = name;
  return true;
}

// sub_821A2B38's cloth chains by character, r11 = character: Crescendo's
// mantle takes the one chain path with its name in r4.
extern "C++" bool PartyBattleCloth(PPCRegister& r11, PPCRegister& r4) {
  if (!eternalsonata::IsPs3Target() || r11.u32 != kCrescendo)
    return false;
  static uint32_t name;
  if (!GuestString(name, "manto01_sp"))
    return false;
  r4.u64 = name;
  return true;
}

// The battle HUD's name, r4 = character - 1 into a ten entry text block.
extern "C++" void PartyBattleNameSid(PPCRegister& r4) {
  if (r4.u32 == kCrescendo - 1 || r4.u32 == kSerenade - 1)
    r4.u64 = eternalsonata::PartyNameSid(static_cast<int>(r4.u32 + 1), false);
}

// The battle HUD's portraits. Its layout (BattleKeep slot 41) draws character
// c from group c + 3. The PS3's adds Crescendo and Serenade as groups 16 and
// 17, but the layout object (sub_820CD1F0) holds sixteen groups, so their
// portraits are built into the group of a character not on the field.
namespace {

constexpr uint32_t kHudLoaded = 2004;      // byte, set once the layout is read
constexpr uint32_t kHudLayoutData = 2000;  // the layout file
constexpr uint32_t kHudSlots = 2032;
constexpr uint32_t kHudSlotSize = 15408;
constexpr uint32_t kSlotCharacter = 15368;
constexpr uint32_t kSlotPortraitGroup = 0x322C;
constexpr uint32_t kGroupSize = 120;
constexpr uint32_t kPortraitBase = 3;      // group of character c is c + 3
constexpr uint32_t kPs3PortraitGroup = 16; // Crescendo, then Serenade

struct Lending {
  uint32_t hud = 0;
  uint32_t group[2] = {};  // borrowed group per character, 0 for none
};
Lending g_lending;

// Group `index` of the layout file at `data`: its texture, its image list
// and the number of images.
bool LayoutGroup(uint8_t* base, uint32_t data, uint32_t index, uint32_t& texture,
                 uint32_t& images, uint32_t& count) {
  if (!data || REX_LOAD_U32(data) != 0x03339010u)
    return false;
  const uint32_t rows = REX_LOAD_U16(data + 4);
  uint32_t textures[32] = {};
  uint32_t texture_count = 0;
  uint32_t group = 0;
  for (uint32_t i = 0; i < rows; ++i) {
    const uint32_t row = data + 8 + 8 * i;
    const uint8_t type = REX_LOAD_U8(row);
    const uint32_t offset = REX_LOAD_U32(row + 4);
    if (type == 1 && texture_count < 32) {
      textures[texture_count++] = data + offset;
    } else if (type == 2 && group++ == index) {
      const uint8_t tex = REX_LOAD_U8(row + 1);
      if (tex >= texture_count)
        return false;
      texture = textures[tex];
      images = data + offset;
      count = REX_LOAD_U8(row + 3);
      return true;
    }
  }
  return false;
}

// sub_820CF310(record, texture, images, count) rebuilds a group record in
// place, releasing what it held.
void BuildGroup(PPCContext& ctx, uint8_t* base, uint32_t hud, uint32_t group,
                uint32_t source) {
  uint32_t texture, images, count;
  if (!LayoutGroup(base, REX_LOAD_U32(hud + kHudLayoutData), source, texture, images, count))
    return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = hud + kGroupSize * group;
  ctx.r4.u64 = texture;
  ctx.r5.u64 = images;
  ctx.r6.u64 = count;
  sub_820CF310(ctx, base);
  ctx = saved;
}

uint32_t SlotCharacter(uint8_t* base, uint32_t hud, uint32_t slot) {
  return REX_LOAD_U32(hud + kHudSlots + kHudSlotSize * slot + kSlotCharacter);
}

void LendPortraits(PPCContext& ctx, uint8_t* base, uint32_t hud) {
  if (g_lending.hud != hud) {
    g_lending = {};
    g_lending.hud = hud;
  }
  bool on_field[kSerenade + 1] = {};
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint32_t c = SlotCharacter(base, hud, slot);
    if (c <= kSerenade)
      on_field[c] = true;
  }
  for (uint32_t i = 0; i < 2; ++i) {
    uint32_t& group = g_lending.group[i];
    // Give a group back once its own character comes on.
    if (group && on_field[group - kPortraitBase]) {
      BuildGroup(ctx, base, hud, group, group);
      group = 0;
    }
    if (group || !on_field[kCrescendo + i])
      continue;
    for (uint32_t c = 1; c <= 10 && !group; ++c) {
      const uint32_t candidate = c + kPortraitBase;
      if (!on_field[c] && candidate != g_lending.group[1 - i])
        group = candidate;
    }
    if (group)
      BuildGroup(ctx, base, hud, group, kPs3PortraitGroup + i);
  }
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint32_t c = SlotCharacter(base, hud, slot);
    if ((c == kCrescendo || c == kSerenade) && g_lending.group[c - kCrescendo])
      REX_STORE_U32(hud + kHudSlots + kHudSlotSize * slot + kSlotPortraitGroup,
                    g_lending.group[c - kCrescendo]);
  }
}

}  // namespace

// Before a slot stores its portrait group (character + 3): 14 and 15 are
// Crescendo and Serenade, drawn from their lent group.
extern "C++" void PartyBattlePortraitGroup(PPCRegister& r) {
  if (r.u32 == kCrescendo + kPortraitBase || r.u32 == kSerenade + kPortraitBase) {
    if (const uint32_t group = g_lending.group[r.u32 - kCrescendo - kPortraitBase])
      r.u64 = group;
  }
}

REX_EXTERN(__imp__sub_820DEC20);

// The battle HUD's per frame update; its first call reads the layout.
REX_HOOK_RAW(sub_820DEC20) {
  const uint32_t hud = ctx.r3.u32;
  if (!REX_LOAD_U8(hud + kHudLoaded))
    g_lending = {};
  __imp__sub_820DEC20(ctx, base);
  if (eternalsonata::IsPs3Target() && REX_LOAD_U8(hud + kHudLoaded))
    LendPortraits(ctx, base, hud);
}
