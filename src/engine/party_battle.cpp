// eternalsonata: Characters 11 and 12 in battle.
//
// The battle reads a character's stats through the twelve wide arrays, so
// what remains are its own ten character switches. The model and cloth cases
// follow the PS3's sub_163010 (its twin of sub_821A2B38): Crescendo is model
// -20 named bCRS with a manto01_sp chain, Serenade -21 named bSRN. A modded
// character takes its own model and files, and its base's case in every
// switch (docs/modded-characters.md).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "generated/eternalsonata_init.h"
#include "character_roster.h"
#include "costume_system.h"
#include "party_system.h"
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

// The pc%03d.bop number sub_821A03D0 loads, r6 = character.
extern "C++" void PartyBattleFile(PPCRegister& r6) {
  r6.u64 = static_cast<uint32_t>(eternalsonata::BattleFileNumber(static_cast<int>(r6.u32)));
}

// The pc%03d(_usa).csf number sub_821BD0D0 loads, r6 = character.
extern "C++" void PartyVoiceFile(PPCRegister& r6) {
  r6.u64 = static_cast<uint32_t>(eternalsonata::VoiceFileNumber(static_cast<int>(r6.u32)));
}

// sub_821A2B38 before its model switch, r11 = character - 1: the model and
// the scene name the cases leave in r28 and r4.
extern "C++" bool PartyBattleModel(PPCRegister& r11, PPCRegister& r4, PPCRegister& r28) {
  const int character = static_cast<int>(r11.u32) + 1;
  if (character <= eternalsonata::kRetailCast || !eternalsonata::CharacterExists(character))
    return false;
  const uint32_t model = eternalsonata::CostumeModel(character);
  std::string scene = eternalsonata::Builtin(character).scene;
  eternalsonata::ModdedCharacter modded;
  if (eternalsonata::ModdedDefinition(character, modded))
    scene = modded.scene.empty() ? eternalsonata::Builtin(modded.base).scene : modded.scene;
  static uint32_t names[eternalsonata::kRosterSize - eternalsonata::kRetailCast];
  static std::string named[std::size(names)];
  const size_t index = static_cast<size_t>(character - eternalsonata::kFirstAddedSlot);
  if (named[index] != scene) {
    names[index] = 0;
    named[index] = scene;
  }
  const uint32_t name = GuestString(names[index], scene.c_str());
  if (!model || !name)
    return false;
  r28.u64 = model;
  r4.u64 = name;
  return true;
}

// sub_821A2B38 before its cloth chains by character, r11 = character: a
// modded character wearing its base's body takes the base's chains; one with
// a model of its own has none yet.
extern "C++" void PartyBattleClothCharacter(PPCRegister& r11) {
  const int character = static_cast<int>(r11.u32);
  eternalsonata::ModdedCharacter modded;
  if (eternalsonata::ModdedDefinition(character, modded))
    r11.u64 = modded.model.empty() ? static_cast<uint32_t>(modded.base) : 0u;
}

// Crescendo's mantle takes the one chain path with its name in r4.
extern "C++" bool PartyBattleCloth(PPCRegister& r11, PPCRegister& r4) {
  if (!eternalsonata::IsPs3Target() || r11.u32 != kCrescendo)
    return false;
  static uint32_t name;
  if (!GuestString(name, "manto01_sp"))
    return false;
  r4.u64 = name;
  return true;
}

// sub_82190028(character, motion): whether the character's bop has its own
// hit motion, else sub_821C8C98 plays the generic flinch. The PS3's twin
// (sub_139068) gives Crescendo and Serenade Allegretto's motion set; a
// modded character takes its base's.
extern "C++" void PartyHitMotionCharacter(PPCRegister& r3) {
  const int character = static_cast<int>(r3.u32);
  if (eternalsonata::IsPs3Target() && (r3.u32 == kCrescendo || r3.u32 == kSerenade))
    r3.u64 = 1;
  else if (eternalsonata::IsModdedCharacter(character))
    r3.u64 = static_cast<uint32_t>(eternalsonata::CharacterBase(character));
}

// sub_821E7358 ORs bits 13..21 of each equipped item's flags into the
// member's status word at +0x3C. No retail item sets them, but the wear bits
// of 11 and 12 (4 << c) are 13 and 14, which gave whoever wore their gear the
// light and dark auras of status bits 0 and 1. The PS3 keeps its wear bits
// at 22 and 23, out of reach. r10 = the item's shifted status bits.
constexpr uint32_t kStatusShift = 13;
constexpr uint32_t kCrescendoStatus = (4u << kCrescendo) >> kStatusShift;
constexpr uint32_t kSerenadeStatus = (4u << kSerenade) >> kStatusShift;

extern "C++" void PartyEquipStatus(PPCRegister& r10) {
  r10.u64 = r10.u32 & ~(kCrescendoStatus | kSerenadeStatus);
}

// After sub_821A2B38's member copy, r23 = the member's character, r28 = the
// copy: on the PS3 Crescendo shines with light and Serenade with dark.
extern "C++" void PartyMemberStatus(PPCRegister& r23, PPCRegister& r28) {
  if (!eternalsonata::IsPs3Target())
    return;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory || !r23.u32 || !r28.u32)
    return;
  const uint32_t character =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(r23.u32));
  const uint32_t status = character == kCrescendo  ? kCrescendoStatus
                          : character == kSerenade ? kSerenadeStatus
                                                   : 0u;
  if (!status)
    return;
  auto* word = memory->TranslateVirtual<uint8_t*>(r28.u32 + 0x3C);
  rex::memory::store_and_swap<uint32_t>(word, rex::memory::load_and_swap<uint32_t>(word) | status);
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
    const int character = static_cast<int>(r.u32 - kPortraitBase);
    if (const uint32_t group = g_lending.group[r.u32 - kCrescendo - kPortraitBase])
      r.u64 = group;
    else if (const int base = eternalsonata::CharacterBase(character))
      r.u64 = static_cast<uint32_t>(base) + kPortraitBase;  // the base's face for now
  }
}

namespace {

// Particle rotation: integrated angles at +0x88, drawn angles at +0x94.
constexpr uint32_t kParticleRotations[] = {0x88, 0x8C, 0x90, 0x94, 0x98, 0x9C};

std::string HexBytes(const uint8_t* p, uint32_t n) {
  std::string s;
  char b[4];
  for (uint32_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%02x", p[i]);
    s += b;
    if (i % 4 == 3)
      s += ' ';
  }
  return s;
}

// An angle the wrap below cannot bring into range: NaN, infinite, or so large
// a 2 pi step no longer changes it. Tested on the bits, not with isfinite.
bool BadAngle(float angle) {
  uint32_t bits;
  std::memcpy(&bits, &angle, sizeof(bits));
  return (bits & 0x7F800000u) == 0x7F800000u || (bits & 0x7FFFFFFFu) >= 0x461C4000u;  // 1e4
}

float SafeAngle(float angle) {
  uint32_t bits;
  std::memcpy(&bits, &angle, sizeof(bits));
  return (bits & 0x7F800000u) == 0x7F800000u ? 0.0f : std::fmod(angle, 6.2831855f);
}

}  // namespace

REX_EXTERN(__imp__sub_820C1238);

// sub_820C1238, f1 = angle. Its wrap loops are fcmpu + blt / bge, and bge is
// taken on an unordered compare, so a NaN angle never leaves them.
REX_HOOK_RAW(sub_820C1238) {
  const float angle = static_cast<float>(ctx.f1.f64);
  if (BadAngle(angle)) {
    static int reports = 0;
    if (reports < 8) {
      ++reports;
      REXLOG_WARN("effect guard: sub_820C1238 angle {} from {:08X}", angle, static_cast<uint32_t>(ctx.lr));
    }
    ctx.f1.f64 = SafeAngle(angle);
  }
  __imp__sub_820C1238(ctx, base);
}

// The drawn Z angle at particle +0x9C is loaded in two places from an
// fctiwz scratch slot whose upper word nothing writes, which comes out as
// 0xFFFFFFFF here. Elsewhere it is the integrated Z at +0x90.
namespace {
void FixDrawnZ(uint32_t particle, PPCRegister& angle, bool integrated) {
  if (!BadAngle(static_cast<float>(angle.f64)))
    return;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  float z = 0.0f;
  if (memory && particle && integrated) {
    z = rex::memory::load_and_swap<float>(memory->TranslateVirtual<uint8_t*>(particle) + 0x90);
    if (BadAngle(z))
      z = 0.0f;
  }
  angle.f64 = z;
}
}  // namespace

// sub_820C8378 at 0x820C9040.
extern "C++" void PartyParticleDrawnZ(PPCRegister& r31, PPCRegister& f31) {
  FixDrawnZ(r31.u32, f31, true);
}

// sub_820C9550 at 0x820C9BC0, before the store. The particle is not being
// integrated here, so there is no Z to fall back on.
extern "C++" void PartyParticleDrawZ(PPCRegister& r31, PPCRegister& f0) {
  FixDrawnZ(r31.u32, f0, false);
}

// sub_820C9550 before it reads the particle's emitter, r31 = particle: logs
// the first particles that arrive with a bad rotation.
extern "C++" void PartyEffectAngleGuard(PPCRegister& r31) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory || !r31.u32)
    return;
  auto* particle = memory->TranslateVirtual<uint8_t*>(r31.u32);
  static int reports = 0;
  for (uint32_t offset : kParticleRotations) {
    const float angle = rex::memory::load_and_swap<float>(particle + offset);
    if (!BadAngle(angle))
      continue;
    if (reports < 4) {
      ++reports;
      const uint32_t emitter = rex::memory::load_and_swap<uint32_t>(particle + 8);
      REXLOG_WARN("effect guard: particle {:08X} +{:X} angle {} emitter {:08X}", r31.u32, offset,
                  angle, emitter);
      REXLOG_WARN("effect guard: particle {}", HexBytes(particle, 0x180));
      if (emitter)
        REXLOG_WARN("effect guard: emitter {}",
                    HexBytes(memory->TranslateVirtual<uint8_t*>(emitter), 0x80));
    }
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


namespace {

// Per character motion tables of sub_821C91F0 and sub_821C9E78, ten rows on
// the 360. The PS3's rows 11 and 12 repeat row 10 in the first and hold 40
// in the second.
constexpr uint32_t kMotionBlendTable = 0x82074BA0;  // u16[10][38]
constexpr uint32_t kMotionBlendRow = 38 * 2;
constexpr uint32_t kMotionStopTable = 0x82074E98;   // u32[10]
constexpr uint32_t kMotionStop = 40;

// Ten retail rows and two more: the PS3's (`tail_value` set, else row 10)
// or a modded character's base row, refreshed on every use since a slot can
// change hands.
uint32_t TwelveRowCopy(uint32_t& copy, uint32_t source, uint32_t row, uint32_t tail_value) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return 0;
  if (!copy) {
    copy = memory->SystemHeapAlloc(row * kSerenade, 0x10);
    if (!copy)
      return 0;
    std::memcpy(memory->TranslateVirtual<uint8_t*>(copy), memory->TranslateVirtual<uint8_t*>(source),
                row * 10);
  }
  auto* out = memory->TranslateVirtual<uint8_t*>(copy);
  for (uint32_t c = kCrescendo; c <= kSerenade; ++c) {
    uint8_t* dest = out + row * (c - 1);
    const int base = eternalsonata::CharacterBase(static_cast<int>(c));
    if (base)
      std::memcpy(dest, out + row * (base - 1), row);
    else if (tail_value)
      rex::memory::store_and_swap<uint32_t>(dest, tail_value);
    else
      std::memcpy(dest, out + row * 9, row);
  }
  return copy;
}

}  // namespace

// After sub_821C91F0 forms word_82074BA0, indexed by character: the blend
// time of the motion it starts. Past row 10 it read float data, thousands of
// frames, so Crescendo's specials never left his previous pose.
extern "C++" void PartyMotionBlendTable(PPCRegister& r11) {
  static uint32_t copy = 0;
  if (TwelveRowCopy(copy, kMotionBlendTable, kMotionBlendRow, 0))
    r11.u64 = copy;
}

// After sub_821C9E78 forms its per character u32 table.
extern "C++" void PartyMotionStopTable(PPCRegister& r11) {
  static uint32_t copy = 0;
  if (TwelveRowCopy(copy, kMotionStopTable, 4, kMotionStop))
    r11.u64 = copy;
}
