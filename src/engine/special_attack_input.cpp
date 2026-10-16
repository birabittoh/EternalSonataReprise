// eternalsonata - Which special attack Y performs, and what the right trigger
// does in battle.
//
// A character can have two specials on each of the light and dark sides.
// sub_821C4DB0 picks the one Y performs: the second once the hold timer at
// object + 116 passes 150 (1/300 s units, so half a second), else the first.
// sub_821C4E30 answers the other one, for the HUD. sub_821C5900 runs that
// timer: it counts while Y is held and fires the special on release.
//
// The PS3 (sub_1C3FE8, sub_1C3CF0) dropped the timer: Y fires on the press and
// R2 held at that moment picks the second special. Alt Special mode does the
// same with RT, on top of the 360 code: the press arms the timer for one frame,
// then fires with the timer set to one side of the threshold or the other, so
// the game's own resets carry the choice through the special.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/mod_plugin.h>

#include "eternalsonata_battle_api.h"
#include "room_presence.h"
#include "target.h"

REXCVAR_DEFINE_STRING(right_trigger_behavior, "auto", "Eternal Sonata",
                      "What the right trigger does in battle: auto (the PS3 data picks "
                      "special, the 360 data camera), camera or special")
    .allowed({"auto", "camera", "special"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(special_hold_time, 0.5, "Eternal Sonata",
                      "Seconds Y is held before it switches to the second special attack")
    .range(0.1, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr u32 kPadRecords = 0x824BB418u;
constexpr u32 kPadStride = 464u;
constexpr u32 kPadCount = 4u;
constexpr u32 kPadConnected = 2u;  // byte
constexpr u32 kPadPrevious = 8u;
constexpr u32 kPadHeld = 0x1A8u;
constexpr u32 kPadPressed = 0x1ACu;
constexpr u32 kPadRepeat = 0x1B0u;
constexpr u32 kPadReleased = 0x1B4u;
constexpr u32 kPadRightTrigger = 0x1C8u;  // float
constexpr u32 kRightTriggerBit = 0x2000000u;

constexpr u32 kObjectOwner = 32u;
constexpr u32 kObjectTimer = 116u;  // float
constexpr u32 kOwnerPad = 81697u;   // byte
constexpr u32 kOwnerSide = 0x13FB4u;
constexpr u32 kOwnerSpecials = 0x13EA4u;  // u16 light 1, dark 1, light 2, dark 2

constexpr double kTimerUnitsPerSecond = 300.0;

bool g_right_trigger[kPadCount] = {};

// Per battle object: Y fired this hold, so the rest of the hold is ignored.
std::unordered_map<u32, bool> g_fired;

int Behavior() {
  const std::string& value = REXCVAR_GET(right_trigger_behavior);
  if (value == "camera")
    return ETERNALSONATA_BATTLE_RIGHT_TRIGGER_CAMERA;
  if (value == "special")
    return ETERNALSONATA_BATTLE_RIGHT_TRIGGER_ALT_SPECIAL;
  return ETERNALSONATA_BATTLE_RIGHT_TRIGGER_AUTO;
}

bool AltSpecial() {
  const int behavior = Behavior();
  if (behavior == ETERNALSONATA_BATTLE_RIGHT_TRIGGER_AUTO)
    return eternalsonata::IsPs3Target();
  return behavior == ETERNALSONATA_BATTLE_RIGHT_TRIGGER_ALT_SPECIAL;
}

float Threshold() {
  return static_cast<float>(REXCVAR_GET(special_hold_time) * kTimerUnitsPerSecond);
}

float LoadFloat(u8* base, u32 addr) { return std::bit_cast<float>(REX_LOAD_U32(addr)); }

void StoreFloat(u8* base, u32 addr, float value) {
  REX_STORE_U32(addr, std::bit_cast<u32>(value));
}

// sub_82199500's pad choice: the owner's pad, or the first connected one.
bool RightTriggerHeld(u8* base, u32 object) {
  const u32 owner = REX_LOAD_U32(object + kObjectOwner);
  u32 pad = REX_LOAD_U8(owner + kOwnerPad);
  if (pad >= kPadCount || !REX_LOAD_U8(kPadRecords + kPadStride * pad + kPadConnected)) {
    pad = 0;
    while (pad < kPadCount && !REX_LOAD_U8(kPadRecords + kPadStride * pad + kPadConnected))
      ++pad;
    if (pad == kPadCount)
      return false;
  }
  return g_right_trigger[pad];
}

bool UseSecond(u8* base, u32 object) {
  const float timer = LoadFloat(base, object + kObjectTimer);
  if (timer > 0.0f)
    return timer > Threshold();
  return AltSpecial() && RightTriggerHeld(base, object);
}

void PickSpecial(PPCContext& ctx, u8* base, bool other) {
  const u32 object = ctx.r3.u32;
  const u32 owner = REX_LOAD_U32(object + kObjectOwner);
  const u32 side = REX_LOAD_U32(owner + kOwnerSide);
  if (side > 1u) {
    ctx.r3.s64 = -1;
    return;
  }
  const bool second = UseSecond(base, object) != other;
  ctx.r3.u64 = REX_LOAD_U16(owner + kOwnerSpecials + 2u * side + (second ? 4u : 0u));
}

}  // namespace

// The pad fill. RT is read here, then hidden from the guest while it picks
// specials, so it stops switching the camera.
REX_EXTERN(__imp__sub_82128310);

REX_HOOK_RAW(sub_82128310) {
  const u32 record = ctx.r3.u32;
  __imp__sub_82128310(ctx, base);
  const u32 pad = (record - kPadRecords) / kPadStride;
  if (record < kPadRecords || pad >= kPadCount)
    return;
  g_right_trigger[pad] = (REX_LOAD_U32(record + kPadHeld) & kRightTriggerBit) != 0;

  if (!eternalsonata::GetRoomPresence().IsBattleActive()) {
    g_fired.clear();
    return;
  }
  if (!AltSpecial())
    return;
  for (u32 offset : {kPadPrevious, kPadHeld, kPadPressed, kPadRepeat, kPadReleased})
    REX_STORE_U32(record + offset, REX_LOAD_U32(record + offset) & ~kRightTriggerBit);
  StoreFloat(base, record + kPadRightTrigger, 0.0f);
}

REX_HOOK_RAW(sub_821C4DB0) { PickSpecial(ctx, base, false); }

REX_HOOK_RAW(sub_821C4E30) { PickSpecial(ctx, base, true); }

// r4 is Y held. Alt Special passes it for one frame per press, which arms the
// timer, then withholds it, which fires the special as a release would.
REX_EXTERN(__imp__sub_821C5900);

REX_HOOK_RAW(sub_821C5900) {
  const u32 object = ctx.r3.u32;
  if (!AltSpecial()) {
    __imp__sub_821C5900(ctx, base);
    return;
  }
  bool& fired = g_fired[object];
  const float timer = LoadFloat(base, object + kObjectTimer);
  if (!(ctx.r4.u32 & 0xFFu)) {
    fired = false;
  } else if (fired) {
    ctx.r4.u64 = 0;
  } else if (timer > 0.0f) {
    const float threshold = Threshold();
    StoreFloat(base, object + kObjectTimer,
               RightTriggerHeld(base, object) ? threshold + 1.0f
                                              : std::min(timer, threshold * 0.5f));
    ctx.r4.u64 = 0;
    fired = true;
  }
  __imp__sub_821C5900(ctx, base);
}

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_battle_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetRightTriggerBehavior(void) {
  return Behavior();
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEffectiveRightTriggerBehavior(void) {
  return AltSpecial() ? ETERNALSONATA_BATTLE_RIGHT_TRIGGER_ALT_SPECIAL
                      : ETERNALSONATA_BATTLE_RIGHT_TRIGGER_CAMERA;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetRightTriggerBehavior(int behavior) {
  static constexpr const char* kValues[] = {"auto", "camera", "special"};
  if (behavior < 0 || behavior > 2)
    return ETERNALSONATA_BATTLE_ERR_INVALID_ARGUMENT;
  rex::cvar::SetFlagByName("right_trigger_behavior", kValues[behavior], /*persist=*/false);
  return ETERNALSONATA_BATTLE_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT float EternalSonataGetSpecialHoldTime(void) {
  return static_cast<float>(REXCVAR_GET(special_hold_time));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetSpecialHoldTime(float seconds) {
  if (!std::isfinite(seconds) || seconds < 0.1f || seconds > 5.0f)
    return ETERNALSONATA_BATTLE_ERR_INVALID_ARGUMENT;
  rex::cvar::SetFlagByName("special_hold_time", std::to_string(seconds), /*persist=*/false);
  return ETERNALSONATA_BATTLE_OK;
}
