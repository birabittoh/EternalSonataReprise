// eternalsonata - Optional axis inversion while aiming a long range attack.
//
// sub_821C55A8 is the aim reticle update, shared by Beat's camera
// (sub_82193B48) and Viola's bow (sub_821CEB78). It reads the left stick out
// of the pad table sub_82128310 fills: flt_824BB5D0[116 * pad] is X, [+1] is
// Y, pad index at *(u32*)(this + 32) + 81697. Flipping the sign around the
// call keeps the inversion to aiming; the rest of the frame sees the real
// stick.
//
// Retail aims direct horizontally but inverted vertically, so each axis is
// flipped only when its cvar disagrees with that.

#include "generated/eternalsonata_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DECLARE(bool, aim_invert_x);
REXCVAR_DECLARE(bool, aim_invert_y);

namespace {

constexpr u32 kPadStickX = 0x824BB5D0u;
constexpr u32 kPadStride = 464u;
constexpr u32 kPadIndexOffset = 81697u;
constexpr u32 kSignBit = 0x80000000u;

} // namespace

REX_EXTERN(__imp__sub_821C55A8);

REX_HOOK_RAW(sub_821C55A8) {
  const bool flip_x = REXCVAR_GET(aim_invert_x);
  const bool flip_y = !REXCVAR_GET(aim_invert_y);
  if (!flip_x && !flip_y) {
    __imp__sub_821C55A8(ctx, base);
    return;
  }

  const u32 owner = REX_LOAD_U32(ctx.r3.u32 + 32u);
  u32 pad = REX_LOAD_U8(owner + kPadIndexOffset);
  if (pad > 4u) {
    pad = 0u;
  }

  const u32 x_addr = kPadStickX + kPadStride * pad;
  const u32 y_addr = x_addr + 4u;
  const u32 x = REX_LOAD_U32(x_addr);
  const u32 y = REX_LOAD_U32(y_addr);

  if (flip_x) {
    REX_STORE_U32(x_addr, x ^ kSignBit);
  }
  if (flip_y) {
    REX_STORE_U32(y_addr, y ^ kSignBit);
  }

  __imp__sub_821C55A8(ctx, base);

  REX_STORE_U32(x_addr, x);
  REX_STORE_U32(y_addr, y);
}
