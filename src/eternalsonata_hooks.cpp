#include "generated/eternalsonata_init.h"

#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "room_presence.h"

// frame_rate cvar: "30" / "60" / "unlocked". Defined (and persisted) in
// settings.cpp; declared here so the frame-driver hook can read it cheaply.
REXCVAR_DECLARE(std::string, frame_rate);


// ---------------------------------------------------------------------------
// Debug hooks
// ---------------------------------------------------------------------------

// sub_82254060 is the devkit-privilege gate called early in xstart (the title
// entry point).  It probes XexCheckExecutablePrivilege(0xA), XGetAVPack, and
// two ExGetXConfigSetting calls; if any check indicates a non-dev retail
// environment it returns 1, which makes xstart call XamLoaderTerminateTitle
// and kill the process.  In the recompiled port we are always "dev-capable"
// so the simplest fix is to override the whole function and return 0 (pass).
REX_EXTERN(__imp__sub_82254060);
REX_HOOK_RAW(sub_82254060) { ctx.r3.u64 = 0; }

// The debug-console (sub_822DFA88) and ConsoleSetting (sub_822E5BE8) init
// hooks used to live here.  Both forced "console active" state bytes after
// the original init ran, and both were removed as dead code: the retail build
// keeps only the allocation and teardown of those objects.  Their state bytes
// (dword_8244DDE0, byte_8244DDE8, ...) have zero reads after init, the command
// buffer at 0x8244C188 is only ever zeroed, and the console vtable
// off_82082DDC holds just a destructor and a nullsub - no render, input, or
// command-dispatch method survives in the binary.  An on-screen console has to
// be built host-side.

// ---------------------------------------------------------------------------
// Skippable message waits
// ---------------------------------------------------------------------------

// sub_821D50A8 is the dialogue markup preprocessor.  It walks a raw text entry
// out of a .e file, expands the `<...>` tags into single-byte control codes in
// a scratch buffer at a1+19104, stores each tag's numeric argument into the
// parallel slot array at a2 + 8*(argidx+65) (argidx counter at a2+840), and
// finally copies the scratch buffer to a2+8 (or a1+35122) + *(u16*)(a2+420).
//
// The three "end of message" tags and the control codes they emit
// (jump table word_820821B8, base loc_821D5740, chars 'n'..'z'; the `w` case
// is at 0x821D5804):
//
//   <w>        -> 2    wait for player input, no timeout   (29473 uses)
//   <wNNNN>    -> 1    auto-advance after NNNN ms, arg=NNNN (26829 uses)
//   <wv>       -> 13   wait for the voice clip to finish    (568 uses)
//
// `<wv>` has no player-skip path at all, so a long voice line (e.g. the battle
// tutorial narration in btldata/script/tutorial/t0001.e) blocks for the full
// clip.  Rewriting the emitted code to 2 puts those messages on the ordinary,
// well-travelled "press a button to advance" path without touching the assets.
//
// This is safe with respect to the argument array: the <w> and <wv> paths both
// advance the arg index by exactly 1, and <w> never reads its slot, so no
// re-indexing is needed - only the control byte changes.

// Make <wv> (wait-for-voice) player-skippable.  This is the one that motivated
// the hook.
//
// Fallback if the code-2 path turns out not to draw an advance prompt during
// battle-tutorial narration: rewrite 13 -> 1 (kWaitTimed) instead.  The <wv>
// handler at 0x821D5858 already stores 0 into that message's argument slot, so
// a timed wait of 0 ms advances immediately rather than waiting for input.
static constexpr bool kSkippableVoiceWaits = true;

// Also convert <wNNNN> (timed auto-advance) into a player wait.  Off by
// default: it would make ~26k normally self-advancing messages - including
// non-dialogue things like title cards - demand a button press.
static constexpr bool kSkippableTimedWaits = false;

static constexpr u8 kWaitForInput = 2;
static constexpr u8 kWaitTimed = 1;
static constexpr u8 kWaitForVoice = 13;

// Guard against a missing terminator in a malformed entry.
static constexpr u32 kMaxMessageBytes = 8192;

REX_EXTERN(__imp__sub_821D50A8);
REX_HOOK_RAW(sub_821D50A8) {
    // The original clobbers r3/r4, so capture the arguments up front.
    const u32 a1 = ctx.r3.u32;
    const u32 a2 = ctx.r4.u32;

    __imp__sub_821D50A8(ctx, base);

    if (!a2 || (!kSkippableVoiceWaits && !kSkippableTimedWaits)) {
        return;
    }

    // Recompute the destination exactly as the tail of sub_821D50A8 does.
    const u32 dest = (REX_LOAD_U32(a2) == REX_LOAD_U32(a1 + 36148)) ? (a1 + 35122) : (a2 + 8);
    const u32 start = dest + REX_LOAD_U16(a2 + 420);

    for (u32 p = start; p < start + kMaxMessageBytes; ++p) {
        const u8 c = REX_LOAD_U8(p);
        if (!c) {
            break;
        }
        if ((kSkippableVoiceWaits && c == kWaitForVoice) ||
            (kSkippableTimedWaits && c == kWaitTimed)) {
            REX_STORE_U8(p, kWaitForInput);
        }
    }
}

// ---------------------------------------------------------------------------
// Frame-rate cap
// ---------------------------------------------------------------------------

// sub_8210A6B8 is the guest's *only* setter for the D3D presentation interval,
// and the presentation interval is the actual frame cap:
//
//   void sub_8210A6B8(int /*unused*/, u8 fps) {
//     if (fps) { u32 v = 60 / fps - 1;                 // 60 -> 0, 30 -> 1, 20 -> 2
//                if (v <= 2) { D3D_SetPresentationInterval(dev, 1 << v);
//                              byte_82465F90 = fps; } }
//     else     { D3D_SetPresentationInterval(dev, 0x80000000 /*IMMEDIATE*/);
//                byte_82465F90 = 60; }                 // dword_8243D374 == 60
//   }
//
// sub_8225A9F0 is D3DDevice_SetPresentationInterval; 1/2/4 are
// D3DPRESENT_INTERVAL_ONE/TWO/FOUR, 0x80000000 is IMMEDIATE. byte_82465F90 is
// the game's own "current fps" value, used by the frame-time accumulator in the
// present path (sub_8210AAD8 does `obj[280] += 300 / byte_82465F90`), i.e. the
// game's clock is expressed in 1/300 s units and *is* frame-rate aware — which
// is why changing the interval scales fps rather than game speed.
//
// Every caller (sub_820EDEF8, sub_82133130, sub_821BA630, sub_821E51B0,
// sub_821E5470, plus the vtable slot at 0x820AD068) funnels through here, so
// overriding the requested rate here overrides it everywhere. The stock game
// asks for 30.
//
// NOTE on vsync: interval N means "present every Nth vblank", so a real
// interval ties the frame rate to the SDK vblank pump, whose rate the `vsync`
// cvar changes drastically. We therefore never use a real interval — see
// ApplyFrameRate — and pace with a host limiter instead, which keeps the frame
// rate independent of `vsync`. (The `vsync` cvar does not give host-side vsync
// in any case: the D3D12 presenter always calls `Present(0, ...)`.)
namespace {

REX_IMPORT(__imp__sub_8225A9F0, g_sub_8225A9F0, void(u32, u32));

// Maps the frame_rate cvar onto a target fps. 0 means "no limit".
//
// IMPORTANT — game speed is (actual fps / declared fps). The sim advances
// `300 / byte_82465F90` clock units per presented frame (sub_8210AAD8), so the
// rate we declare to the game must equal the rate we actually achieve, or the
// whole game runs fast/slow in proportion. That is what the host limiter below
// is for; the presentation interval alone cannot do it with vsync=false,
// because the SDK's vblank pump fires in bursts and the interval wait is
// satisfied immediately.
//
// Exactness caveat: `300 / rate` is integer division, so only divisors of 300
// keep game speed exact — 20, 25, 30, 50, 60, 75, 100, 150. 120 is NOT one
// (300/120 truncates 2.5 to 2), and was confirmed in-game to run ~20% slow
// motion even when paced perfectly. Don't add a rate here that doesn't
// divide 300.
// The rate the guest last asked for. The stock game does not run at a single
// rate: sub_82133130 (title) and sub_821E51B0 (the save menu, which saves the
// old rate into byte_8243F232 to restore later) ask for 60, most of the game
// asks for 30. Screens that ask for 60 have logic written for 60 presents per
// second, so pinning them to 30 halves the ticks that logic gets while anything
// driven by the wall clock is unaffected — that is what made the save slots
// finish their slide-in while the player was still choosing.
u8 g_guest_rate = 30;

// "30" means stock: follow the guest's own request, which is 30 for gameplay and
// 60 where the game asks for it. There is deliberately no option that pins every
// screen to 30. "stock" is accepted as an alias for settings written while that
// was the option's name.
u8 RequestedFrameRate(u8 stock) {
  const std::string& mode = REXCVAR_GET(frame_rate);
  if (mode == "60")
    return 60;
  if (mode == "unlocked" || mode == "0")
    return 0;
  return stock;
}

}  // namespace

namespace {

// Applies the fps value by doing exactly what sub_8210A6B8 does. Must be called
// with a live guest context (it calls into guest code).
//
// The call to sub_8225A9F0 goes through a rex::CallFrame, NOT the caller's ctx.
// sub_8225A9F0 itself is a one-line field store (`device[13444] = interval`) and
// cannot fault — but calling it with the caller's context leaves r3 holding its
// return value (the device pointer). When this ran from the sub_8210AAD8 hook
// below, that clobbered sub_8210AAD8's own r3 argument, and the original then
// ran against the device pointer as if it were its object, faulting deep in
// sub_82261AB0 on a wild `stw r9,12580(r31)`. CallFrame copies only r1/r13/
// fpscr, so the callee's register writes don't leak back into ctx.
void ApplyFrameRate(PPCContext& ctx, u8* base, u8 fps) {
  const u32 device = REX_LOAD_U32(0x82465F68);
  if (!device) {
    return;  // Device not created yet; the init-time call will cover us.
  }
  rex::CallFrame frame{ctx};
  if (fps) {
    // Use a real presentation interval whenever the rate can express one, and
    // fall back to IMMEDIATE otherwise. The interval is NOT inert: sub_8225E818
    // reads device[13444], maps it to 0/1/2/3, and packs it into the swap
    // scheduler argument, where sub_8225E680 (vblank ISR, spinlocked on the
    // kernel device object) adds it to the flip schedule:
    //
    //   v17 = last_flip_target + interval;
    //   if (v17 <= vblank_count) { v17 = vblank_count; ... }
    //   if (v17 == vblank_count) flip now; else queue in the pending-flip ring;
    //
    // With IMMEDIATE the interval term is 0, so every flip takes the "flip now"
    // branch and the pending-flip ring goes unused — a different front-buffer
    // publication pattern than stock, which matters because the title runs two
    // front buffers (dword_82466100 / dword_82466104). Host pacing is the
    // limiter's job either way; this only restores the guest's stock flip
    // scheduling.
    const u32 interval = (fps <= 60 && 60u % fps == 0) ? (60u / fps - 1u) : 3u;
    g_sub_8225A9F0(frame, base, device, interval <= 2 ? (1u << interval) : 0x80000000u);
    REX_STORE_U8(0x82465F90, fps);
  } else {
    // Unlocked: no limiter, so we cannot make declared == actual. Leave the
    // sim's rate at the stock 60; the game will run fast in proportion to how
    // far above 60 fps it renders. That is inherent to a frame-clocked engine.
    g_sub_8225A9F0(frame, base, device, 0x80000000u);
    REX_STORE_U8(0x82465F90, static_cast<u8>(REX_LOAD_U32(0x8243D374)));
  }
}

// Last fps value we pushed into the device, so the per-frame re-apply below
// only touches D3D state when the cvar actually changed.
u8 g_applied_fps = 0xFF;

// Sleep for `d` without the ~15 ms granularity of a default Windows timer.
// Nothing in the SDK raises the global timer resolution (no timeBeginPeriod
// anywhere in the tree), and raising it process-wide is a heavy-handed thing to
// do from a hook, so use a high-resolution waitable timer instead: it is
// per-wait, needs no extra link library, and degrades to a normal sleep if the
// OS is too old to support the flag.
void PreciseSleep(std::chrono::steady_clock::duration d) {
  if (d <= std::chrono::steady_clock::duration::zero()) {
    return;
  }
#ifdef _WIN32
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;  // negative = relative, in 100 ns units
    due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);
    if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(d);
}

// Host frame limiter. The guest present thread waits here until the frame's
// deadline, so the achieved rate matches the rate declared to the sim above.
// Sleeps to ~1.5 ms short of the deadline (Windows timer granularity is coarse
// and nothing in the SDK raises it), then spins for the remainder.
void LimitFrame(u8 fps) {
  using clock = std::chrono::steady_clock;
  static clock::time_point next_deadline{};

  if (!fps) {
    next_deadline = {};  // Unlocked: drop any stale deadline.
    return;
  }

  const auto period = std::chrono::duration_cast<clock::duration>(
      std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
  const auto now = clock::now();

  // First frame, a rate change, or a long stall (loading screen, alt-tab):
  // restart the cadence instead of trying to catch up on missed frames.
  if (next_deadline == clock::time_point{} || now > next_deadline + period * 4) {
    next_deadline = now + period;
    return;
  }

  constexpr auto kSpinMargin = std::chrono::microseconds(1500);
  if (next_deadline - now > kSpinMargin) {
    PreciseSleep((next_deadline - now) - kSpinMargin);
  }
  while (clock::now() < next_deadline) {
    std::this_thread::yield();
  }

  // Never accumulate debt. If the wait overshot (coarse timer, a slow frame,
  // anything), schedule the next deadline from now rather than from the missed
  // one. Catching up would fire a burst of zero-wait presents, and because the
  // game advances its sim clock by a fixed 300/rate units per present — not by
  // elapsed time — such a burst runs the game's animation clock forward in a
  // few milliseconds of wall time. That is what made save-slot slide-ins finish
  // while the player was still choosing. Running a hair slow is harmless; a
  // burst is not.
  const auto after = clock::now();
  next_deadline += period;
  if (next_deadline <= after) {
    next_deadline = after + period;
  }
}

}  // namespace

REX_HOOK_RAW(sub_8210A6B8) {
  const u8 requested = static_cast<u8>(ctx.r4.u32);
  g_guest_rate = requested ? requested : 60;
  const u8 fps = RequestedFrameRate(g_guest_rate);
  ApplyFrameRate(ctx, base, fps);
  g_applied_fps = fps;
}

// The game only calls sub_8210A6B8 at init and on a few scene transitions, so
// overriding it alone means the settings slider does nothing until the next
// such call. Re-apply from the present path instead, which runs every frame:
// sub_8210AAD8 is the render pump's present (verified under a live breakpoint:
// it hits every frame). The check is a cvar string compare against the last
// applied value, and it only touches D3D when they differ.
REX_EXTERN(__imp__sub_8210AAD8);

REX_HOOK_RAW(sub_8210AAD8) {
  // The original clobbers r3, so capture the render-pump object up front.
  const u32 a1 = ctx.r3.u32;
  const u8 fps = RequestedFrameRate(g_guest_rate);
  if (fps != g_applied_fps) {
    ApplyFrameRate(ctx, base, fps);
    g_applied_fps = fps;
    REXLOG_INFO("[fps-cap] applied frame_rate={} (interval arg {})", REXCVAR_GET(frame_rate), fps);
  }
  __imp__sub_8210AAD8(ctx, base);

  // Pace after the present, so the wait covers the whole frame.
  LimitFrame(fps);
}

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
// takes the state row back out of "Overworld"; without it the flag
// NotifyAreaLoad sets would never clear and every menu after the first field
// load would still read as "Overworld".
REX_EXTERN(__imp__sub_820FD998);

REX_HOOK_RAW(sub_820FD998) {
  if (ctx.r4.u32 == 0) {
    eternalsonata::GetRoomPresence().NotifyFieldTeardown();
  }
  __imp__sub_820FD998(ctx, base);
}

// ---------------------------------------------------------------------------
// Dead lead, kept as a note: sub_8223FB78
// ---------------------------------------------------------------------------
//
// An earlier attempt hooked sub_8223FB78, believed to hold "the single FPS cap
// in the whole image" (an `elapsed_us >= 30000` gate). That was wrong: IDA
// reports exactly one xref to 0x8223FB78, at 0x820B2448, and that is a .pdata
// unwind entry, not a dispatch-table slot. Nothing in the image references the
// function as code or data, and a live breakpoint on it never hit. It is dead
// code in this build. Do not resurrect that hook.
