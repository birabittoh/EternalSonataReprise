// eternalsonata: Characters 11 and 12 in battle.
//
// The battle reads a character's stats through the twelve wide arrays, so
// what remains are its own ten character switches. The model and cloth cases
// follow the PS3's sub_163010 (its twin of sub_821A2B38): Crescendo is model
// -20 named bCRS with a manto01_sp chain, Serenade -21 named bSRN.

#include <cstring>

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

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
