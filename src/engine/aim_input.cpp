// eternalsonata - Optional axis inversion while aiming a long range attack,
// plus right stick aiming and mouse look.
//
// sub_821C55A8 is the aim reticle update, shared by Beat's camera
// (sub_82193B48) and Viola's bow (sub_821CEB78). It reads the left stick out
// of the pad table sub_82128310 fills: flt_824BB5D0[116 * pad] is X, [+1] is
// Y, pad index at *(u32*)(this + 32) + 81697. The right stick is the next two
// floats of the same record (the fill writes 0x1B8..0x1C4) and the game reads
// it nowhere else. Rewriting the left slots around the call keeps both the
// inversion and the right stick confined to aiming; the rest of the frame,
// movement included, still sees the real sticks.
//
// Retail aims direct horizontally but inverted vertically, so each axis is
// flipped only when its cvar disagrees with that.

#include "generated/eternalsonata_init.h"

#include "aim_input.h"

#include <bit>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DECLARE(bool, aim_invert_x);
REXCVAR_DECLARE(bool, aim_invert_y);
REXCVAR_DECLARE(bool, mnk_mouse);

namespace {

constexpr u32 kPadStickX = 0x824BB5D0u;
constexpr u32 kPadStride = 464u;
constexpr u32 kPadIndexOffset = 81697u;
constexpr u32 kSignBit = 0x80000000u;

bool g_aiming = false;
bool g_forced_mouse = false;

float Magnitude2(u32 x, u32 y) {
  const float fx = std::bit_cast<float>(x);
  const float fy = std::bit_cast<float>(y);
  return fx * fx + fy * fy;
}

} // namespace

namespace eternalsonata {

void AimInputTick() {
  if (!g_aiming && g_forced_mouse) {
    REXCVAR_SET(mnk_mouse, false);
    g_forced_mouse = false;
  }
  g_aiming = false;
}

}  // namespace eternalsonata

REX_EXTERN(__imp__sub_821C55A8);

REX_HOOK_RAW(sub_821C55A8) {
  g_aiming = true;
  if (!REXCVAR_GET(mnk_mouse)) {
    REXCVAR_SET(mnk_mouse, true);
    g_forced_mouse = true;
  }

  const bool flip_x = REXCVAR_GET(aim_invert_x);
  const bool flip_y = !REXCVAR_GET(aim_invert_y);

  const u32 owner = REX_LOAD_U32(ctx.r3.u32 + 32u);
  u32 pad = REX_LOAD_U8(owner + kPadIndexOffset);
  if (pad > 4u) {
    pad = 0u;
  }

  const u32 x_addr = kPadStickX + kPadStride * pad;
  const u32 y_addr = x_addr + 4u;
  const u32 x = REX_LOAD_U32(x_addr);
  const u32 y = REX_LOAD_U32(y_addr);

  // Whichever stick is pushed further wins, so neither one fights the other
  // when both are held.
  u32 aim_x = x;
  u32 aim_y = y;
  const u32 rx = REX_LOAD_U32(x_addr + 8u);
  const u32 ry = REX_LOAD_U32(y_addr + 8u);
  if (Magnitude2(rx, ry) > Magnitude2(x, y)) {
    aim_x = rx;
    aim_y = ry;
  }

  if (flip_x) {
    aim_x ^= kSignBit;
  }
  if (flip_y) {
    aim_y ^= kSignBit;
  }

  REX_STORE_U32(x_addr, aim_x);
  REX_STORE_U32(y_addr, aim_y);

  __imp__sub_821C55A8(ctx, base);

  REX_STORE_U32(x_addr, x);
  REX_STORE_U32(y_addr, y);
}
