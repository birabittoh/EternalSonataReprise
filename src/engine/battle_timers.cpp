// eternalsonata - Multipliers on the two battle timers.
//
// Tactical time (timer object + 6440/6444, 1/300 s) must run out before the
// action gauge starts draining; the party level table gives it 3 s, 1 s or
// none, and levels 1 and 2 set the no limit flag at + 6452 instead. Enemies
// never get any. sub_821ABF70 and sub_8218A618 write it at turn setup.
//
// The action gauge (+ 8476 max, + 8480 left) is sized by sub_821AFCA0, which
// every reader of its full length also calls, so scaling its result keeps
// them in step. "No limit" is a gauge too long to ever drain.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "battle_layout.h"

REXCVAR_DEFINE_DOUBLE(party_tactical_time_multiplier, 1.0, "Eternal Sonata",
                      "Multiplier on the party's tactical time (0..10, above 10 no limit)")
    .range(0.0, 11.0);

REXCVAR_DEFINE_DOUBLE(party_action_time_multiplier, 1.0, "Eternal Sonata",
                      "Multiplier on the party's action time (0.25..10, above 10 no limit)")
    .range(0.25, 11.0);

REXCVAR_DEFINE_DOUBLE(enemy_action_time_multiplier, 1.0, "Eternal Sonata",
                      "Multiplier on the enemies' action time")
    .range(0.25, 10.0);

namespace {

namespace battle = eternalsonata::battle;

constexpr u32 kTacticalDuration = battle::kCommandTimerDurationOffset;
constexpr u32 kTacticalRemaining = battle::kCommandTimerRemainingOffset;
constexpr u32 kTacticalUnlimited = 6452u;  // byte
constexpr double kUnlimitedAbove = 10.0;
constexpr double kUnlimitedTicks = 1 << 30;

u32 CurrentActorKind(u8* base) {
  const u32 holder = REX_LOAD_U32(battle::kManager + battle::kActorHolderOffset);
  if (!holder)
    return ~0u;
  const u32 descriptor = REX_LOAD_U32(holder + battle::kActorDescriptorOffset);
  return descriptor ? REX_LOAD_U32(descriptor) : ~0u;
}

void ScaleTacticalTime(u8* base, u32 manager, u32 kind) {
  if (kind != battle::kActorKindParty)
    return;
  const double scale = REXCVAR_GET(party_tactical_time_multiplier);
  const u32 timer = REX_LOAD_U32(manager + battle::kActionTimerObjectPtrOffset);
  if (!timer || scale == 1.0 || REX_LOAD_U8(timer + kTacticalUnlimited))
    return;
  if (scale > kUnlimitedAbove) {
    REX_STORE_U8(timer + kTacticalUnlimited, 1);
    return;
  }
  for (u32 offset : {kTacticalDuration, kTacticalRemaining}) {
    const double ticks = static_cast<int32_t>(REX_LOAD_U32(timer + offset)) * scale;
    REX_STORE_U32(timer + offset, static_cast<u32>(std::lround(ticks)));
  }
}

}  // namespace

REX_EXTERN(__imp__sub_821AFCA0);

REX_HOOK_RAW(sub_821AFCA0) {
  __imp__sub_821AFCA0(ctx, base);
  const u32 kind = CurrentActorKind(base);
  double scale = 1.0;
  if (kind == battle::kActorKindParty)
    scale = REXCVAR_GET(party_action_time_multiplier);
  else if (kind == battle::kActorKindEnemy)
    scale = REXCVAR_GET(enemy_action_time_multiplier);
  if (scale > kUnlimitedAbove)
    ctx.f1.f64 = kUnlimitedTicks;
  else
    ctx.f1.f64 = std::min(ctx.f1.f64 * scale, kUnlimitedTicks);
}

// r4 is the acting unit's {kind, slot}.
REX_EXTERN(__imp__sub_821ABF70);

REX_HOOK_RAW(sub_821ABF70) {
  const u32 manager = ctx.r3.u32;
  const u32 kind = REX_LOAD_U32(ctx.r4.u32);
  __imp__sub_821ABF70(ctx, base);
  ScaleTacticalTime(base, manager, kind);
}

// Above party level 1, moving clears the no limit flag and the tactical
// window drops to 0.00. Level 1 keeps it and drains the gauge only while the
// unit acts, so an unlimited setting gets that rule for the call.
constexpr u32 kPartyLevelByte = 0x8243F3EBu;  // low byte of dword_8243F3E8

REX_EXTERN(__imp__sub_8218A7C0);

REX_HOOK_RAW(sub_8218A7C0) {
  if (REXCVAR_GET(party_tactical_time_multiplier) <= kUnlimitedAbove ||
      CurrentActorKind(base) != battle::kActorKindParty) {
    __imp__sub_8218A7C0(ctx, base);
    return;
  }
  const u8 level = REX_LOAD_U8(kPartyLevelByte);
  REX_STORE_U8(kPartyLevelByte, 1);
  __imp__sub_8218A7C0(ctx, base);
  REX_STORE_U8(kPartyLevelByte, level);
}

REX_EXTERN(__imp__sub_8218A618);

REX_HOOK_RAW(sub_8218A618) {
  const u32 manager = ctx.r3.u32;
  __imp__sub_8218A618(ctx, base);
  ScaleTacticalTime(base, manager, CurrentActorKind(base));
}
