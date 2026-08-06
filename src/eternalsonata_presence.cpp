#include "generated/eternalsonata_init.h"

#include "room_presence.h"

// ---------------------------------------------------------------------------
// Field area tracking (Discord Rich Presence)
// ---------------------------------------------------------------------------

// sub_820FAFB0 and sub_820FB420 are the map manager's field-area loaders:
// each receives the area id (a "cfdata\<id>.e" filename like "ktm01.e") in r4
// and loads the area's cfdata + maptex, caching only a 4-byte code
// (dword_8244B934 / the map object). The generic cfdata loader that writes
// byte_8244B500 (sub_820FCC80) is used only by the menu/event path, so during
// normal field play the id never reaches guest static data (which is why the
// old byte_8244B500-only presence showed "Title Screen" in-field). Hook both
// loaders to forward the id to RoomPresence, which drives the Discord overlay;
// see room_presence.cpp for the precedence rules.
namespace {

// sub_820FAFB0/sub_820FB420's r5 (a3) argument carries a flags word whose
// 0xA000 bits distinguish a real transition from a speculative gate-proximity
// preload. Session-log-verified (walking up to, waiting at, then entering a
// house): the real village load and the real house entry both had r5 =
// 0xA100 / 0xA001 (0xA000 bits set); every proximity-only preload call --
// including the reverse preload of the exterior the game issues right after
// the real entry, so you can step back out seamlessly -- had r5 = 0x10 (bits
// clear). Caller address alone doesn't work: the same trigger-table wrapper
// (sub_820ECC30) fires for both real and speculative calls.
constexpr uint32_t kRealTransitionFlagMask = 0xA000u;

void CaptureFieldArea(PPCContext& ctx, uint8_t* base) {
  const u32 id_addr = ctx.r4.u32;
  if (!id_addr) {
    return;
  }
  char id[16] = {};
  for (u32 i = 0; i < sizeof(id) - 1; ++i) {
    const u8 c = REX_LOAD_U8(id_addr + i);
    if (!c) {
      break;
    }
    id[i] = static_cast<char>(c);
  }
  if ((ctx.r5.u32 & kRealTransitionFlagMask) != kRealTransitionFlagMask) {
    return;
  }
  eternalsonata::GetRoomPresence().NotifyAreaLoad(id);
}

}  // namespace

REX_EXTERN(__imp__sub_820FAFB0);

REX_HOOK_RAW(sub_820FAFB0) {
  CaptureFieldArea(ctx, base);
  __imp__sub_820FAFB0(ctx, base);
}

REX_EXTERN(__imp__sub_820FB420);

REX_HOOK_RAW(sub_820FB420) {
  CaptureFieldArea(ctx, base);
  __imp__sub_820FB420(ctx, base);
}

// sub_820FDB80 is battle entry: its last act, on every path (no early return
// before it), is to request scene mode 4 --
//   0x820FDF94  li  r10, 4
//   0x820FDF9C  stw r10, dword_824C74C4(r11)
// -- so reaching it means a battle is starting. The request itself cannot be
// polled: it is consumed and cleared faster than one rendered frame (see
// room_presence.cpp), which is why this is a hook and not a memory read.
REX_EXTERN(__imp__sub_820FDB80);

REX_HOOK_RAW(sub_820FDB80) {
  eternalsonata::GetRoomPresence().NotifyBattleStart();
  __imp__sub_820FDB80(ctx, base);
}

// There is deliberately no battle-*exit* hook. The obvious candidates
// (sub_821AB1A0, sub_821A61F0, sub_82229098, sub_8223EA48, sub_8223F3C0,
// sub_8223F898, sub_821DD4D0) all request scene mode 3 conditionally, and
// hooking them was tried: sub_821AB1A0 is the one that fires, but it fires
// ~2.8s *into* the battle -- the length of the entry transition -- not at the
// end. Exit is detected in Tick() instead; see room_presence.cpp.

// sub_820FD998 is the single map-transition funnel: every map load and unload
// goes through it. Called with a null name (r4 == 0) it takes the full
// teardown path, freeing all four map slots -- i.e. "no field map is loaded any more". That is the edge that
// takes the state row back out of "Exploring..."; without it the flag
// NotifyAreaLoad sets would never clear and every menu after the first field
// load would still read as "Exploring...".
REX_EXTERN(__imp__sub_820FD998);

REX_HOOK_RAW(sub_820FD998) {
  if (ctx.r4.u32 == 0) {
    eternalsonata::GetRoomPresence().NotifyFieldTeardown();
  }
  __imp__sub_820FD998(ctx, base);
}
