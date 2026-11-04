// eternalsonata - The PS3's scattered battle start positions.
//
// Both releases share the eight formations at 0x82023DE8 (EBOOT 0x46CD24) and
// pick one the same way (sub_821A26C0, EBOOT sub_160AC0). The PS3's placement
// (sub_1610D8 party, sub_160460 enemies) then drops the table position for a
// random one unless the battle is a tutorial or its encounter record has the
// story bit set, the same bit that makes the 360's intro unskippable.
//
// A random spot is on the unit's half of a disc around the field centre (the
// whole disc for an ambushed party), at a random distance up to a share of the
// field half width, facing diagonally. It is rerolled until the battle stage's
// ground is within reach and it stands clear of the field's props and of the
// party members already placed.

#include "generated/eternalsonata_init.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <random>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "target.h"

REXCVAR_DEFINE_STRING(battle_start_positions, "auto", "Eternal Sonata",
                      "Where units stand when a battle starts: 360 (fixed formation), ps3 "
                      "(scattered, except story and tutorial battles) or auto (follows the "
                      "game data)")
    .allowed({"auto", "360", "ps3"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_821A28C0);
REX_EXTERN(__imp__sub_821A29C0);
REX_EXTERN(__imp__sub_821A26C0);
REX_EXTERN(__imp__sub_821A2518);
REX_EXTERN(__imp__sub_8217BED0);
REX_EXTERN(__imp__sub_821101A8);
REX_EXTERN(__imp__sub_8217BF28);
REX_EXTERN(__imp__sub_82178A88);
REX_EXTERN(__imp__sub_82178BB0);
REX_EXTERN(__imp__sub_8217A8C0);

namespace {

constexpr u32 kSceneManager = 0x824CF500u;
constexpr u32 kTutorialArmed = 0x824D0440u + 0x83238u;  // u8
constexpr u32 kEncounterRecord = 0x8253923Cu;           // pointer
constexpr u32 kEncounterFlags = 0x4Cu;                  // bit 0: story battle

// Off the placement object (manager + 64) every hook here receives in r3.
constexpr u32 kStageHandle = 664u;
constexpr u32 kPartyCount = 672u;
constexpr u32 kEnemyCount = 673u;
constexpr u32 kPartyNode = 265272u;
constexpr u32 kPartyStride = 81972u;
constexpr u32 kEnemyNode = 445496u;
constexpr u32 kEnemyStride = 32456u;
constexpr u32 kEnemyNodeToFlags = 15652u;  // node - this = record + 340
constexpr u32 kFlyingBit = 1u << 17;       // picks the stage's second ground layer
constexpr u32 kFieldHalfWidth = 528176u;   // float, the PS3's +527560
constexpr u32 kProps = 531648u;            // 12 byte records, handle first
constexpr u32 kPropStride = 12u;
constexpr u32 kPropCount = 531748u;        // u8
constexpr u32 kAmbush = 532840u;           // u8: 1 the party's, 2 the enemies'
constexpr u32 kSidePending = 8840u;        // u8, sub_821A26C0's inputs below
constexpr u32 kEnemyFieldX = 8892u;
constexpr u32 kLeaderFieldX = 8856u;

// EBOOT TOC floats.
constexpr float kPartyReach = 0.7f;
constexpr float kEnemyReach = 0.95f;
constexpr float kMinGapSquared = 4.0f;
constexpr double kGroundReach = 0.5;
constexpr float kQuarterTurn = std::numbers::pi_v<float> / 4.0f;
constexpr float kHalfTurn = std::numbers::pi_v<float>;

// The PS3 rerolls forever; give up and keep the table spot instead.
constexpr int kMaxAttempts = 256;

// The PS3's bit 2: which pair of formations sub_821A26C0 took.
bool g_far_side = true;
u32 g_scratch = 0;
std::mt19937 g_rng{std::random_device{}()};

float Random() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng); }

float LoadFloat(u8* base, u32 address) { return std::bit_cast<float>(REX_LOAD_U32(address)); }
void StoreFloat(u8* base, u32 address, float value) { REX_STORE_U32(address, std::bit_cast<u32>(value)); }

u32 Scratch() {
  if (!g_scratch) {
    auto* kernel = rex::system::kernel_state();
    if (kernel && kernel->memory())
      g_scratch = kernel->memory()->SystemHeapAlloc(128, 0x20);
  }
  return g_scratch;
}
u32 PositionVec() { return Scratch(); }
u32 GroundHit() { return Scratch() + 32; }
u32 ReadVec() { return Scratch() + 96; }

// Called on the hook's own ctx, since the scene calls can reach other hooks.
u32 Call(PPCContext& ctx, u8* base, void (*fn)(PPCContext&, u8*), std::initializer_list<u32> args,
         double f1 = 0.0, double f2 = 0.0) {
  const PPCContext saved = ctx;
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8};
  size_t i = 0;
  for (u32 arg : args)
    regs[i++]->u64 = arg;
  ctx.f1.f64 = f1;
  ctx.f2.f64 = f2;
  fn(ctx, base);
  const u32 result = ctx.r3.u32;
  ctx = saved;
  return result;
}

bool Scattered(u8* base) {
  const std::string& value = REXCVAR_GET(battle_start_positions);
  if (value != "ps3" && !(value == "auto" && eternalsonata::IsPs3Target()))
    return false;
  if (REX_LOAD_U8(kTutorialArmed))
    return false;
  const u32 record = REX_LOAD_U32(kEncounterRecord);
  return record && !(REX_LOAD_U32(record + kEncounterFlags) & 1u) && Scratch();
}

// sub_2C5070's layer lookup on the battle stage's collision.
u32 GroundLayer(PPCContext& ctx, u8* base, u32 placement, u32 layer) {
  const u32 stage = REX_LOAD_U32(placement + kStageHandle);
  if (stage == 0xFFFFFFFFu)
    return 0;
  const u32 object = Call(ctx, base, __imp__sub_8217BED0, {kSceneManager, stage});
  const u32 collision = object ? REX_LOAD_U32(object + 320) : 0;
  if (!collision)
    return 0;
  const u32 layers = REX_LOAD_U32(collision + 84);
  const u32 header = REX_LOAD_U32(collision + 24);
  if (!layers || !header || layer >= REX_LOAD_U8(header + 48))
    return 0;
  return layers + 28 * layer;
}

bool OnGround(PPCContext& ctx, u8* base, u32 layer) {
  return Call(ctx, base, __imp__sub_821101A8, {layer, PositionVec(), GroundHit()}, kGroundReach,
              1.0) &
         0xFF;
}

bool Clear(PPCContext& ctx, u8* base, u32 node) {
  Call(ctx, base, __imp__sub_8217BF28, {ReadVec(), kSceneManager, node});
  float gap = 0.0f;
  for (u32 i = 0; i < 3; ++i) {
    const float d = LoadFloat(base, PositionVec() + 4 * i) - LoadFloat(base, ReadVec() + 4 * i);
    gap += d * d;
  }
  return gap >= kMinGapSquared;
}

bool ClearOfProps(PPCContext& ctx, u8* base, u32 placement) {
  const u32 count = REX_LOAD_U8(placement + kPropCount);
  for (u32 i = 0; i < count; ++i) {
    if (!Clear(ctx, base, REX_LOAD_U32(placement + kProps + kPropStride * i)))
      return false;
  }
  return true;
}

bool ClearOfParty(PPCContext& ctx, u8* base, u32 placement, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    if (!Clear(ctx, base, REX_LOAD_U32(placement + kPartyNode + kPartyStride * i)))
      return false;
  }
  return true;
}

// x leans towards `side` (-1, 1), or anywhere when 0; z is free.
void PickSpot(u8* base, float side, float reach) {
  float x = side == 0.0f ? 2.0f * Random() - 1.0f : side * Random();
  float z = 2.0f * Random() - 1.0f;
  const float length = std::sqrt(x * x + z * z);
  if (length > 0.0f) {
    x /= length;
    z /= length;
  }
  const float distance = Random() * reach;
  StoreFloat(base, PositionVec(), x * distance);
  StoreFloat(base, PositionVec() + 4, 0.0f);
  StoreFloat(base, PositionVec() + 8, z * distance);
}

void Place(PPCContext& ctx, u8* base, u32 node, float yaw) {
  Call(ctx, base, __imp__sub_82178A88, {kSceneManager, node, PositionVec(), 0, 0, 0xFFFFFFFFu});
  StoreFloat(base, PositionVec(), 0.0f);
  StoreFloat(base, PositionVec() + 4, yaw);
  StoreFloat(base, PositionVec() + 8, 0.0f);
  Call(ctx, base, __imp__sub_82178BB0, {kSceneManager, node, PositionVec(), 0, 0, 0xFFFFFFFFu});
}

void ScatterPartyMember(PPCContext& ctx, u8* base, u32 placement, u32 slot) {
  const u32 layer = GroundLayer(ctx, base, placement, 0);
  if (!layer)
    return;
  const bool ambushed = REX_LOAD_U8(placement + kAmbush) == 2;
  const float reach = LoadFloat(base, placement + kFieldHalfWidth) * kPartyReach;
  const float side = ambushed ? 0.0f : (g_far_side ? 1.0f : -1.0f);
  const float yaw = (g_far_side ? 5.0f * kQuarterTurn : kQuarterTurn) + (ambushed ? kHalfTurn : 0.0f);
  const u32 node = REX_LOAD_U32(placement + kPartyNode + kPartyStride * slot);
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    PickSpot(base, side, reach);
    if (!OnGround(ctx, base, layer) || !ClearOfProps(ctx, base, placement) ||
        !ClearOfParty(ctx, base, placement, slot))
      continue;
    Place(ctx, base, node, yaw);
    Call(ctx, base, __imp__sub_8217A8C0, {kSceneManager, node, 0, 0, 0xFFFFFFFFu});
    return;
  }
}

void ScatterEnemies(PPCContext& ctx, u8* base, u32 placement) {
  const bool party_ambush = REX_LOAD_U8(placement + kAmbush) == 1;
  const float reach = LoadFloat(base, placement + kFieldHalfWidth) * kEnemyReach;
  const float side = g_far_side ? -1.0f : 1.0f;
  const float yaw =
      (g_far_side ? kQuarterTurn : 5.0f * kQuarterTurn) + (party_ambush ? kHalfTurn : 0.0f);
  const u32 party = REX_LOAD_U8(placement + kPartyCount);
  const u32 count = REX_LOAD_U8(placement + kEnemyCount);
  for (u32 slot = 0; slot < count; ++slot) {
    const u32 node_address = placement + kEnemyNode + kEnemyStride * slot;
    const bool flying = REX_LOAD_U32(node_address - kEnemyNodeToFlags) & kFlyingBit;
    const u32 layer = GroundLayer(ctx, base, placement, flying ? 1 : 0);
    if (!layer)
      continue;
    const u32 node = REX_LOAD_U32(node_address);
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
      PickSpot(base, side, reach);
      if (!OnGround(ctx, base, layer) || !ClearOfProps(ctx, base, placement) ||
          !ClearOfParty(ctx, base, placement, party))
        continue;
      Place(ctx, base, node, yaw);
      break;
    }
  }
}

}  // namespace

// Battles without a field leader or enemy take the second pair, like the PS3.
REX_HOOK_RAW(sub_821A2518) {
  if (ctx.r4.u32 == 0xFFFFFFFFu || ctx.r5.u32 == 0xFFFFFFFFu)
    g_far_side = true;
  __imp__sub_821A2518(ctx, base);
}

REX_HOOK_RAW(sub_821A26C0) {
  const u32 placement = ctx.r3.u32;
  if (REX_LOAD_U8(kTutorialArmed))
    g_far_side = true;
  else if (REX_LOAD_U8(placement + kSidePending))
    g_far_side = LoadFloat(base, placement + kEnemyFieldX) < LoadFloat(base, placement + kLeaderFieldX);
  __imp__sub_821A26C0(ctx, base);
}

REX_HOOK_RAW(sub_821A28C0) {
  const u32 placement = ctx.r3.u32;
  const u32 slot = ctx.r4.u32;
  const PPCContext saved = ctx;
  __imp__sub_821A28C0(ctx, base);
  if (!Scattered(base))
    return;
  const PPCContext result = ctx;
  ctx = saved;
  ScatterPartyMember(ctx, base, placement, slot);
  ctx = result;
}

REX_HOOK_RAW(sub_821A29C0) {
  const u32 placement = ctx.r3.u32;
  const PPCContext saved = ctx;
  __imp__sub_821A29C0(ctx, base);
  if (!Scattered(base))
    return;
  const PPCContext result = ctx;
  ctx = saved;
  ScatterEnemies(ctx, base, placement);
  ctx = result;
}
