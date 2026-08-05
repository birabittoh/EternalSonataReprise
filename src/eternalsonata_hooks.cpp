#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <imgui.h>
#include <rex/cvar.h>

#include "room_presence.h"
#include "settings.h"

// frame_rate cvar: "30" / "60" / "unlocked". Defined (and persisted) in
// settings.cpp; declared here so the frame-driver hook can read it cheaply.
REXCVAR_DECLARE(std::string, frame_rate);
REXCVAR_DECLARE(bool, frame_debug);
REXCVAR_DECLARE(bool, adaptive_framerate);
REXCVAR_DECLARE(bool, menu_scan);


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
// Exactness caveat: `300 / rate` is integer division, so a rate must divide 300
// to keep game speed exact — 120 does not (2.5 -> 2) and was confirmed in-game
// to run ~20% slow motion even when paced perfectly. Dividing 300 is necessary
// but NOT sufficient: the rate must also divide 60 to stay on the content's
// authored grid, which rules out 25/50/75/100/150 even though they are exact.
// See the ladder comment below. In practice 60 is the ceiling and 30 the floor.
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

// ---------------------------------------------------------------------------
// Adaptive frame skipping
//
// There is no way to skip *rendering* a frame from here: the guest draws and
// then presents, and by the time the present hook runs the work is already
// done. The only lever is byte_82465F90, the rate declared to the sim — and it
// turns out to be a sufficient one. Verified in IDA: ~100 call sites read that
// byte and each computes `300 / byte_82465F90` as "clock units this frame is
// worth" (sub_820EA758, sub_8212D350, and the present path itself). Declaring
// a lower rate therefore makes every frame count for proportionally more sim
// time, which is exactly frame skipping: fewer frames drawn, same game speed.
//
// (Note the per-frame accumulator at dword_82465F98 — obj+280, the field the
// present path bumps — has no readers anywhere in the image. Topping it up to
// compensate for a slow frame does nothing; the declared rate is the whole
// mechanism.)
//
// Game speed = actual fps / declared fps, so the goal is to declare a rate the
// host can actually hit. Two constraints on the fallback rungs:
//
//   - The rung must divide 300, or `300 / rate` truncates and the game runs at
//     the wrong speed (120 -> 2.5 -> 2 was measured at ~20% slow motion).
//   - The rung must divide 60. This is the constraint that matters and the one
//     two earlier attempts got wrong. `300 / rate` is the amount added per
//     frame, and the game's content is authored around the stock cadence of 30
//     (step 10) and 60 (step 5). Rates that divide 60 keep the step a multiple
//     of 5 and stay on that grid: 20 -> 15, 15 -> 20. Rates that don't come off
//     it — 50 -> 6, 75 -> 4, 100 -> 3 — and the models visibly twitch even
//     though game speed is arithmetically exact.
//
// The first attempt used a fixed ladder containing 100/75/50 and parked on 50.
// The second derived rungs as whole-number divisions of the *target*, which
// gives 150 -> 75 -> 50 and parks on 50 again. A third derived them from the
// display refresh rate; that happens to give the right answer on a 60 Hz panel
// (60/30/20 divide 60) but makes the game's behaviour depend on the user's
// monitor, which is wrong — the authored cadence is a property of the content,
// not of the screen.
//
// So the ladders are fixed. The selected target is always the first rung even
// when it is off-grid (the user asked for it, and it is the ceiling we try
// first), but every fallback beneath it divides 60.
//
// LimitFrame measures the per-frame work time — guest logic plus present, with
// our own pacing wait excluded. Sustained work over the current rung's budget
// steps down; sustained headroom against the *next higher* rung's (tighter)
// budget steps back up. Comparing headroom against the current rung's budget
// instead is a trap: once at 30 fps, "comfortable" against a 33 ms budget is a
// far lower bar than sustaining 60 fps's 16.7 ms, so it would climb straight
// back into the rate it just failed at and oscillate.
// ---------------------------------------------------------------------------

// Fallback ladder. Every rung divides 60, so the per-frame step stays on the
// content's 5-unit grid (60 -> 5, 30 -> 10). 30 is the floor: it's the game's
// own stock gameplay rate, which the content is built for; 20 was dropped
// because the game isn't built for it.
//
// 60 is the only adaptive target, and it is also the highest rate this engine
// can express at all. Three independent constraints agree on that ceiling: the
// declared rate is stored in a single byte (byte_82465F90), it must divide 300
// or `300 / rate` truncates and the game runs at the wrong speed, and it must
// divide 60 to stay on the authored grid. Nothing above 60 satisfies all
// three — 75/100/150 are off-grid, 180 doesn't even divide 300 (step 1.67 -> 1,
// i.e. 0.6x speed), and 300 doesn't fit in a byte. Fast-forward is offered as
// a held key instead; see TurboHeld.
const u8* g_rungs = nullptr;
size_t g_rung_count = 0;

bool BuildLadder(u8 target) {
  static constexpr u8 k60[] = {60, 30};
  if (target == 60) {
    g_rungs = k60;
    g_rung_count = std::size(k60);
    return true;
  }
  g_rungs = nullptr;  // "30"/stock and "unlocked" are not adaptive.
  g_rung_count = 0;
  return false;
}

// Scores are leaky buckets, not consecutive-frame streaks: a bucket that
// decays rides out isolated hitches while still requiring sustained evidence.
//
// Both rates are deliberately conservative about *leaving* the target rate.
// Stepping down is a visible, second-order-of-magnitude change, and a machine
// that can nearly hold the target is far better off holding it than dropping
// to half. An earlier tuning used a 95%-of-budget "late" test with gain 2 /
// trip 40, which fires on a machine hovering at 15-17 ms against a 16.67 ms
// budget — i.e. on a machine that can actually sustain 60. And with an ahead
// decay of 3 against a trip of 600, any run where a quarter of the frames miss
// the headroom bar can never accumulate, so the ladder could not climb back at
// all. Keep decay <= gain on the way up, or the ladder is one-way.
// Step-down can afford to be brisk now that "behind" means a genuinely missed
// deadline rather than a guess at one. Gain 4 against decay 1 gives a useful
// curve: constant overruns trip in ~15 frames (a quarter second), a 50% overrun
// rate in ~40, and a 25% rate still gets there in a few seconds, while an
// isolated hitch decays away without ever accumulating.
constexpr int kBehindGain = 4;   // per overrun frame
constexpr int kBehindDecay = 1;  // per frame that met its deadline
constexpr int kBehindTrip = 60;
constexpr int kBehindMax = 120;

// Climbing is deliberately aggressive. It is safe to be, because step-down is
// now both fast and trustworthy: a climb that turns out to be wrong is undone
// in a quarter second of overruns, so the cost of guessing high is a brief
// stumble, whereas the cost of guessing low is sitting at half rate for
// seconds at a time. Errors in the two directions are not symmetrical.
constexpr int kAheadGain = 1;   // per frame that would have met the higher rung
constexpr int kAheadDecay = 1;  // per frame that would not
constexpr int kAheadTrip = 60;  // a sustained majority of good frames, ~2 s
constexpr int kAheadMax = 120;

// After stepping down, climbing is blocked outright for a while, and the block
// doubles each further step down. Without it the ladder flaps: a demanding
// area is exactly where headroom briefly appears (a fade, a menu) and would
// otherwise buy an immediate climb back into a rate the area can't sustain.
constexpr int kUpBlockBase = 45;  // frames
constexpr int kUpBlockMax = 480;

u8 g_ladder_target = 0;  // which ladder is loaded; 0 = none
size_t g_rung = 0;
int g_behind_score = 0;
int g_ahead_score = 0;
int g_up_block = 0;
int g_up_block_len = kUpBlockBase;

// Frames since the last step up, saturating at kClimbHoldFrames. Distinguishes
// a climb that held from one that collapsed immediately — measured logs showed
// failed climbs falling back within 0.35-0.52 s while genuine ones lasted
// 3-11 s, so the two are cleanly separable. Starts saturated so the first step
// down of a session isn't blamed on a climb that never happened.
constexpr int kClimbHoldFrames = 120;  // ~2 s at 60
int g_frames_since_up = kClimbHoldFrames;

// Set by LimitFrame, consumed once by the present hook below. Not a valid
// measurement on the first frame of a new cadence (rate change, long stall).
std::chrono::microseconds g_frame_work{0};
bool g_frame_measured = false;

constexpr std::chrono::microseconds PeriodFor(u8 fps) {
  return std::chrono::microseconds(1'000'000 / fps);
}

// `measure` gates whether this call may advance the ladder: only the per-frame
// present hook (sub_8210AAD8) should. The scene-transition hook (sub_8210A6B8)
// calls this too, but only wants the rung currently in force for the rate it
// is applying.
//
// `stock` is what the guest itself asked for (g_guest_rate), independent of
// the frame_rate cvar. Screens the stock game paces at 60 (title, save menu)
// are not what the ladder exists to protect. It exists to soften gameplay,
// which stock paces at 30 and the "60" option boosts to 60. So when stock is
// already 60, act vanilla and hand back 60 untouched, leaving the ladder's
// state (built for the gameplay rate) alone rather than dragging those
// screens down to whatever rung gameplay perf last settled on.
u8 AdaptiveFrameRate(u8 requested, u8 stock, bool measure) {
  if (!REXCVAR_GET(adaptive_framerate)) {
    g_ladder_target = 0;  // Rebuild from the top if it is switched back on.
    return requested;
  }
  if (stock == 60) {
    return 60;
  }
  if (g_ladder_target != requested) {
    // First use, or the user changed the setting: rebuild and start at the top.
    if (!BuildLadder(requested)) {
      // Not an adaptive target ("30"/stock, "unlocked"): pass through
      // untouched, and forget the ladder so a later switch back restarts it.
      g_ladder_target = 0;
      return requested;
    }
    g_ladder_target = requested;
    g_rung = 0;
    g_behind_score = g_ahead_score = 0;
    g_up_block = 0;
    g_up_block_len = kUpBlockBase;
    std::string ladder_desc;
    for (size_t i = 0; i < g_rung_count; ++i) {
      ladder_desc += (i ? " -> " : "") + std::to_string(g_rungs[i]);
    }
    REXLOG_INFO("[fps-cap] target {} fps; ladder = {}", requested, ladder_desc);
  } else if (!g_rung_count) {
    return requested;
  }

  if (measure && g_frame_measured) {
    g_frame_measured = false;  // one measurement, one update
    if (g_frames_since_up < kClimbHoldFrames) {
      ++g_frames_since_up;
    }

    // "Behind" means missing the target by a *meaningful* margin, not missing
    // it at all. The two outcomes are not symmetrical: declaring 60 and
    // achieving 55 runs the game at 92% speed, which is barely perceptible,
    // while stepping down to 30 halves the frame rate outright and permanently.
    // So a host that merely grazes the deadline should stay where it is and
    // accept mild slow motion; only one that cannot get near the rate should
    // drop. Measured on a machine sitting right at the 16.67 ms line, treating
    // any overrun as disqualifying pinned it to 30 indefinitely even though it
    // was very nearly sustaining 60.
    //
    // 115% of the period is roughly "can't hold 52 fps at a 60 target".
    if (g_frame_work * 100 > PeriodFor(g_rungs[g_rung]) * 115) {
      g_behind_score = std::min(g_behind_score + kBehindGain, kBehindMax);
    } else {
      g_behind_score = std::max(g_behind_score - kBehindDecay, 0);
    }

    if (g_up_block > 0) {
      --g_up_block;
      g_ahead_score = 0;
    } else if (g_rung > 0) {
      // The mirror of the step-down test: would this frame have met the higher
      // rung's deadline? The 5% is only headroom for the limiter's own spin
      // margin, not a performance bar.
      //
      // This asked for 80% of the higher period before, which is a different
      // question from the one step-down asks, and the mismatch stranded
      // machines at a low rung: a host that holds 60 with 15 ms frames never
      // overruns at 60, yet never gets under 80% of 16.67 ms (13.3 ms) either,
      // so it could not trip either test and sat at 30 indefinitely. Keep these
      // two thresholds describing the same event or the ladder is one-way.
      if (g_frame_work * 100 < PeriodFor(g_rungs[g_rung - 1]) * 95) {
        g_ahead_score = std::min(g_ahead_score + kAheadGain, kAheadMax);
      } else {
        g_ahead_score = std::max(g_ahead_score - kAheadDecay, 0);
      }
    }

    if (g_behind_score >= kBehindTrip && g_rung + 1 < g_rung_count) {
      ++g_rung;
      g_behind_score = g_ahead_score = 0;
      // Only a climb that *held* clears the anti-flap penalty. Resetting it on
      // every step up (which an earlier version did) means a climb that
      // collapses in half a second still wipes the penalty, so the doubling
      // never accumulates and the ladder flaps indefinitely — measured at 20
      // transitions in two minutes. Charging the doubling to failed climbs
      // only makes a flapping sequence back off geometrically and die out,
      // while a rung that genuinely became sustainable still starts fresh.
      if (g_frames_since_up >= kClimbHoldFrames) {
        g_up_block_len = kUpBlockBase;
      }
      g_up_block = g_up_block_len;
      g_up_block_len = std::min(g_up_block_len * 2, kUpBlockMax);
      REXLOG_INFO("[fps-cap] step down to {} fps", g_rungs[g_rung]);
    } else if (g_ahead_score >= kAheadTrip && g_rung > 0) {
      --g_rung;
      g_behind_score = g_ahead_score = 0;
      g_frames_since_up = 0;
      REXLOG_INFO("[fps-cap] step up to {} fps", g_rungs[g_rung]);
    }
  }
  return g_rungs[g_rung];
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

// Fast-forward, held only. Since the sim advances a fixed 300/declared units
// per present and never by elapsed time, simply not waiting makes the game run
// fast in exact proportion to the extra frames — the same mechanism as the
// "unlocked" setting, applied for as long as the key is down. Nothing about the
// declared rate changes, so releasing the key returns to normal speed with no
// state to unwind.
//
// Deliberately momentary rather than a toggle: a stuck fast-forward in a
// cutscene is unrecoverable without noticing what happened.
constexpr int kTurboKey = VK_TAB;

bool TurboHeld() {
#ifdef _WIN32
  if (!(GetAsyncKeyState(kTurboKey) & 0x8000)) {
    return false;
  }
  // GetAsyncKeyState is global, so check we own the foreground window —
  // otherwise Alt-Tabbing away and using Tab in another app fast-forwards the
  // game in the background.
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid != GetCurrentProcessId()) {
    return false;
  }
  // Don't steal Tab from the settings overlay, where it moves between widgets.
  if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard) {
    return false;
  }
  return true;
#else
  return false;
#endif
}

// Host frame limiter. The guest present thread waits here until the frame's
// deadline, so the achieved rate matches the rate declared to the sim above.
// Sleeps to ~1.5 ms short of the deadline (Windows timer granularity is coarse
// and nothing in the SDK raises it), then spins for the remainder.
void LimitFrame(u8 fps) {
  using clock = std::chrono::steady_clock;
  static clock::time_point next_deadline{};
  static clock::time_point wait_end{};

  const auto now = clock::now();

  // Per-frame work time for the ladder above: everything since the previous
  // frame's wait finished, i.e. guest logic plus the present, with our own
  // pacing wait excluded. Deriving it as `period - slack` instead (the obvious
  // shortcut, and what the first version did) is wrong — it is expressed in
  // terms of a deadline that the debt-free rule below keeps moving, so it reads
  // short after any overshoot and under-reports exactly the slow frames the
  // ladder exists to notice.
  if (wait_end != clock::time_point{}) {
    g_frame_work = std::chrono::duration_cast<std::chrono::microseconds>(now - wait_end);
    g_frame_measured = true;
  }
  wait_end = now;

  if (!fps) {
    next_deadline = {};  // Unlocked: drop any stale deadline.
    g_frame_measured = false;
    return;
  }

  const auto period = std::chrono::duration_cast<clock::duration>(
      std::chrono::duration<double>(1.0 / static_cast<double>(fps)));

  // First frame, a rate change, or a long stall (loading screen, alt-tab):
  // restart the cadence instead of trying to catch up on missed frames. The
  // frame that spans a stall says nothing about sustainable performance, so
  // don't let it feed the ladder.
  if (next_deadline == clock::time_point{} || now > next_deadline + period * 4) {
    next_deadline = now + period;
    g_frame_measured = false;
    return;
  }

  constexpr auto kSpinMargin = std::chrono::microseconds(1500);
  const auto slack = next_deadline - now;
  if (slack > kSpinMargin) {
    PreciseSleep(slack - kSpinMargin);
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
  wait_end = after;  // The next frame's work is measured from here.
  next_deadline += period;
  if (next_deadline <= after) {
    next_deadline = after + period;
  }
}

// Per-second pacing summary, behind the frame_debug cvar.
//
// `presents` is counted here in the hook rather than derived from anything, on
// purpose: sub_8210AAD8 has three callers (sub_8210AC38, sub_8210CF90, and the
// vtable slot at 0x820AD068), so "presents per second" and "frames per second
// on screen" are not necessarily the same number. If presents/sec reads about
// double the observed frame rate, the limiter is pacing each *call* rather than
// each displayed frame and the whole cadence is wrong at the source.
void ReportFramePacing(u8* base, u8 fps) {
  if (!REXCVAR_GET(frame_debug)) {
    return;
  }
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start{};
  static int presents = 0;
  static std::chrono::microseconds work_sum{0};
  static std::chrono::microseconds work_max{0};
  static int work_n = 0;

  const auto now = clock::now();
  if (window_start == clock::time_point{}) {
    window_start = now;
  }
  ++presents;
  if (g_frame_work > std::chrono::microseconds::zero()) {
    work_sum += g_frame_work;
    work_max = std::max(work_max, g_frame_work);
    ++work_n;
  }

  const auto elapsed = now - window_start;
  if (elapsed < std::chrono::seconds(1)) {
    return;
  }
  const double secs = std::chrono::duration<double>(elapsed).count();
  const double mean_ms = work_n ? (double(work_sum.count()) / work_n) / 1000.0 : 0.0;
  REXLOG_INFO(
      "[frame-dbg] presents/s={:.1f} declared={} paced_to={} target={} work_mean={:.2f}ms "
      "work_max={:.2f}ms behind={} ahead={} up_block={}",
      presents / secs, REX_LOAD_U8(0x82465F90), fps, g_ladder_target, mean_ms,
      double(work_max.count()) / 1000.0, g_behind_score, g_ahead_score, g_up_block);

  window_start = now;
  presents = 0;
  work_sum = work_max = std::chrono::microseconds::zero();
  work_n = 0;
}

}  // namespace

REX_HOOK_RAW(sub_8210A6B8) {
  const u8 requested = static_cast<u8>(ctx.r4.u32);
  g_guest_rate = requested ? requested : 60;
  const u8 fps = AdaptiveFrameRate(RequestedFrameRate(g_guest_rate), g_guest_rate, /*measure=*/false);
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

// Defined further down with the rest of the memory differ. Polled from the
// present path as well as the menu path so the manual F9-F12 hotkeys work from
// anywhere, not only while a menu is up.
namespace {
void ScanPollKeys(u8* base);
void ScanTick(u8* base);
}  // namespace

REX_HOOK_RAW(sub_8210AAD8) {
  // The original clobbers r3, so capture the render-pump object up front.
  const u32 a1 = ctx.r3.u32;
  const u8 fps = AdaptiveFrameRate(RequestedFrameRate(g_guest_rate), g_guest_rate, /*measure=*/true);
  if (fps != g_applied_fps) {
    ApplyFrameRate(ctx, base, fps);
    g_applied_fps = fps;
  }
  __imp__sub_8210AAD8(ctx, base);
  ScanPollKeys(base);
  ScanTick(base);

  // Pace after the present, so the wait covers the whole frame. Passing 0 while
  // fast-forwarding skips the wait entirely; LimitFrame also drops its stale
  // deadline and flags the frame unmeasured, so unpaced frames can't be read as
  // headroom and talk the ladder into stepping up.
  LimitFrame(TurboHeld() ? 0 : fps);
  ReportFramePacing(base, fps);
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

// ---------------------------------------------------------------------------
// Native option rows in the Options screen
// ---------------------------------------------------------------------------
//
// Screens are static display lists walked by sub_821F2F38 (full derivation in
// docs/debug-hooks.md §14): a stream of variable-length records, each tagged
// at +0 with `type = index * 100`, dispatched through word_82082428 and ended
// by a record whose type is 0xFFFF. A text label is type 200, 0x28 bytes, with
// a BTX string id at +4 and absolute X/Y at +8/+0xC.
//
// Because every record carries its own absolute position, rows can simply be
// appended: we copy the Options list, add a label + N value text records per
// row before a fresh terminator, and point the interpreter at the copy. The
// records use synthetic string ids that no BTX block defines, and the
// sub_8223B780 hook below answers them directly - so no game data is repacked
// and no asset has to be rebuilt.
//
// Rows are table-driven (kOptionRows below): each has a label, an ordered
// list of values (drawn side by side, exactly as the stock two-option rows
// do), and get/set callbacks that resolve to whichever index is active. A
// value can either author its own literal text (for values with no stock
// analogue, e.g. "60 FPS") or reuse a stock BTX id (for "Si"/"NO", which stay
// localised for free). See docs/debug-hooks.md §14 for the underlying RE.

namespace {

// The Options display list is duplicated once per language - same byte
// layout, different address - rather than sharing one list with
// language-independent string ids the way BTX text lookups do. Live-tested
// 2026-08-06 by watching sub_821F2F38's actual argument per language (ids
// found empirically, not derivable from the language byte's own switch below -
// this is a different value entirely, an argument computed by the screen's
// caller). Indexed by LanguageIndex() below: en, de, fr, es, it.
constexpr u32 kOptionsListByLang[5] = {
    0x8202F388u,  // en
    0x820723F8u,  // de
    0x82068648u,  // fr
    0x8206D520u,  // es
    0x8205E880u,  // it
};
constexpr u32 kOptionsListBytes = 0x868u;  // up to, excluding, its terminator

// Splice point: just past the whole Sottotitoli row group - its label
// (0x8205EF64), the "Si" and "NO" value records, and the type-600 underline -
// i.e. guest 0x8205EFEC. Sottotitoli is the closest analogue to what we are
// adding (a boolean row), so inserting here puts our records in exactly the
// drawing state the real rows are in. Appending at the end of the list instead
// made the label render offset from its own cursor position.
constexpr u32 kInsertOffset = 0x76Cu;
constexpr u32 kTextRecord = 200u;          // type 200 = text label
constexpr u32 kTextRecordBytes = 0x28u;
// Type 100 is a whole family: its handler at 0x821F35DC sub-dispatches on
// `type - 100` across 21 shapes (word_820823E0). Plain 100 is the highlight
// bar, {100, sprite, height, y, 1000, 1000, -1}, 0x1C bytes.
//
// `+4` is NOT an X coordinate. The handler at 0x821F3610 loads it with `lwz`
// and passes it to sub_821EF0E8 as an integer, while `+8` and `+0xC` go
// through fcfid into floats - so +4 is a sprite/resource id, and a bar carries
// no authored X at all (the row handler positions it). Writing a coordinate
// there makes sub_821EF0E8 fail, and it stores its -1 into the id array while
// still bumping the count - which crashes the per-frame walk a few seconds
// later. Reuse the game's own sprite id.
constexpr u32 kIconRecord = 100u;
constexpr u32 kIconRecordBytes = 0x1Cu;

// The bar is a **type-110** record - the 100 family, subtype 10 - at list
// offset 0x6C4: {110, 234, 490, 285, 1000, 1000, -1}. Sprite 234, x 490 (the
// left option's column) and y 285 (the row's own display y). It sits *before*
// the row's text, because the bar draws behind it.
//
// Two wrong turns, recorded so they are not repeated:
//   - type 600 ({600, 120, 328, 950} at 0x75C) creates nothing at all - the id
//     count did not even move. It is the underline, as originally named.
//   - type 100 subtype 0 ({100, 350, ...} at 0x76C) is the small subtitle
//     ICON, one per row. Right family, wrong subtype.
// Only the 100 family creates entries in the id array; text (200) and 600 do
// not, which is why our three text records never disturbed the row indices.
// Clone the whole stock block, not just the bar record. A bar on its own comes
// out visibly darker than the stock ones: the {2101, 2100, 1} record before it
// sets the draw state the type-100 family handler branches on (the r27 test at
// 0x821F3610), and the {2} after it restores that state. Emitting the bar
// outside the pair leaves it drawing with whatever blend the preceding icon
// record left behind.
constexpr u32 kBarRecordOffset = 0x6B8u;  // {2101,2100,1} + bar + {2}
constexpr u32 kBarRecordBytes = 0x2Cu;
constexpr int32_t kBarShrinkFixup = 7;    // see below
constexpr u32 kBarBlockYOffset = 0x18u;   // the bar record's y within the block
// +0x10 / +0x14 of a type-100-family record are a size scale in thousandths
// (the stock records carry 1000, and the two at list offset 0x580 carry 949).
constexpr u32 kBarBlockWOffset = 0x1Cu;
constexpr u32 kBarBlockHOffset = 0x20u;
// Bar placement, taken from the game rather than measured.
//
// sub_82200FE8 is the Options screen init, and it places every bar itself with
// sub_82178A88 - an instant set - so the y authored in the display-list record
// never decides where a bar ends up. That is why the record's y could not be
// made to agree with the runtime one: there are two coordinate spaces, and the
// record is in neither.
//
//   slot   init x                         init y   handler y (sub_82201620)
//   +84    base + 200*(1-byte_8243FBFC)      895      155
//   +88    base + 200*dword_8243F368         945      205
//   +92    base + 200*(BYTE2(FC04)^1)       1205      465
//   +120   base + 200*(1-BYTE1(FC04))       1155      415
//
// The two spaces differ by a constant 740 on every row. Rows follow Subtitles
// (415) and Voce (465) on the same 50px pitch, so row r's handler-space y is
// 515 + 50*r and its init-space y is that + 740. The x formula is the same in
// both spaces: base + 200 * value_index.
//
// Bar size: 650 x 800 thousandths of the stock bar, measured against the real
// rows. +0x10 / +0x14 of a type-100-family record are that scale (the stock
// records carry 1000; the two at list offset 0x580 carry 949).
// Dumping the stock Subtitles bar object next to ours (see DumpHighlightBars)
// showed them identical but for two things: y, differing by exactly the 100 of
// two row pitches, and the scale at +0x4C/+0x50 - 1.0/1.0 stock against our
// 0.65/0.80. So the leftover "few pixels too high" is entirely the height
// scale: the sprite is anchored at its top, so shrinking it lifts the bottom
// edge and the bar's centre rises by (1 - scale) * height / 2. The correction
// is 7px, settled against the real rows - close to the 5px a 50px-tall bar
// would predict, so the sprite is a little taller than the row pitch. Applied
// to every y value so they stay in step.
constexpr int32_t kBarHeight = 800;

// Moving the bar. sub_82201620 does exactly this on every value change:
//   sub_82179F78(&dword_824CF500, id, &{x, y, 0}, 0.0, 14.0, 14.0, 14.0)
// with r10 = 15 (the call site at 0x82201B34 sets it; it lands in the params
// block as a mode field, so it is not optional). The two float constants are
// flt_82016120 = 0.0 and flt_820AAC3C = 14.0, read out of the image.
//
// x = base + 200 * value_index, where base is language dependent, and the
// runtime y is the record's display y + 130 (stock: record 285 -> move 415).
constexpr u32 kTextRegistry = 0x824CF500u;
constexpr u32 kBarMoveMode = 15u;
constexpr double kBarMoveSpeed = 14.0;
constexpr int32_t kBarColumnStride = 200;
constexpr u32 kLanguage = 0x8243D370u;

constexpr u32 kListTerminator = 0x0000FFFFu;

// Row layout: 50px pitch starting just below Voce (285, 335), matching the
// stock rows' own pitch.
constexpr int32_t kRowY0 = 385;
constexpr int32_t kRowYStep = 50;
constexpr int32_t kRowXLabel = 120;
constexpr int32_t kRowXValue0 = 490;  // first value column; +200 per index
constexpr u32 kMaxRowValues = 3;

// Synthetic BTX ids, far above any real entry (the xex block defines 211) so
// they can never collide with a genuine lookup. Row r's label is
// kRowSidBase + kRowSidStride*r; its value i is one past that, +i.
constexpr u32 kRowSidBase = 900u;
constexpr u32 kRowSidStride = 10u;

// The Sottotitoli row's own value strings - "Si" (130) and "NO" (131) - reused
// so boolean rows read identically to the stock ones and stay localised.
constexpr u32 kBtxYes = 130u;
constexpr u32 kBtxNo = 131u;

// Row labels are authored text (there is no stock BTX analogue for
// "Resolution"/"Frame Rate"/"Fullscreen"), so unlike the boolean values above
// they cannot ride the game's own localisation for free - each language needs
// its own literal. Index matches LanguageIndex() below: en, de, fr, es, it.
//
// Accents are written as raw CP1252/Latin-1 byte escapes rather than UTF-8
// source characters: the stock EFIGS text in the game's own BTX blocks is
// single-byte, and writing multi-byte UTF-8 into a single-byte text record
// would render as two garbled glyphs instead of one accented one. Verify
// in-game per language - this is the one part of the row that cannot be
// cross-checked against a stock string the way the boolean values are.
struct LocalizedLabel {
  const char* text[5];
};

// Which language is active is derived from *which of the 5 known list
// addresses matched* (see kOptionsListByLang), not from dword_8243D370's own
// switch - that byte's numbering doesn't correspond to XLanguage kernel ids in
// any way that was confirmed reliable (verified Italian, guessed the other
// three, and two of those guesses were wrong - see 2026-08-06 in
// docs/debug-hooks.md). Address matching sidesteps the guess entirely: the
// list address is what sub_821F2F38 actually branches on, so matching against
// it directly is ground truth, not an inference from an unrelated byte.
int OptionsListIndex(u32 list_addr) {
  for (int i = 0; i < static_cast<int>(std::size(kOptionsListByLang)); ++i) {
    if (kOptionsListByLang[i] == list_addr) {
      return i;
    }
  }
  return -1;
}

constexpr LocalizedLabel kLabelResolution = {
    {"Resolution", "Aufl\xF6sung", "R\xE9solution", "Resoluci\xF3n", "Risoluzione"}};
constexpr LocalizedLabel kLabelFrameRate = {
    {"Frame Rate", "Bildrate", "Fr\xE9quence", "Fotogramas", "Framerate"}};
constexpr LocalizedLabel kLabelFullscreen = {
    {"Fullscreen", "Vollbild", "Plein \xE9" "cran", "Pantalla completa", "Schermo intero"}};

// One entry per value a row can hold. `literal` non-null means we author that
// exact text ourselves (used where there is no stock analogue, e.g. "60
// FPS"); `literal == nullptr` means reuse the stock BTX string `btx_id`
// (localised for free) - that is how the boolean rows get "Si"/"NO". Values
// here (FPS figures, resolution names) are conventionally left untranslated
// even in localised menus, so unlike labels they need no per-language table.
struct OptionValue {
  const char* literal;
  u32 btx_id;
};

struct OptionRow {
  const LocalizedLabel* label;
  const OptionValue* values;
  u8 value_count;
  int (*get_index)();                    // active value, 0-based
  void (*set_index)(u8* base, int idx);  // apply a newly selected value
  // Highlight bar width, thousandths of the stock bar (see kBarWidth). Wider
  // literal values (e.g. "1080p") need a wider bar than the stock "Si"/"NO"
  // width to actually cover the text.
  int32_t bar_width;
};

int FullscreenGetIndex();
void FullscreenSetIndex(u8* base, int idx);
int FrameRateGetIndex();
void FrameRateSetIndex(u8* base, int idx);
int ResolutionGetIndex();
void ResolutionSetIndex(u8* base, int idx);

constexpr OptionValue kBoolValues[2] = {{nullptr, kBtxYes}, {nullptr, kBtxNo}};
constexpr OptionValue kFrameRateValues[2] = {{"30 FPS", 0}, {"60 FPS", 0}};
constexpr const char* kFrameRateIds[2] = {"30", "60"};
constexpr OptionValue kResolutionValues[3] = {{"720p", 0}, {"1080p", 0}, {"1440p", 0}};
constexpr const char* kResolutionIds[3] = {"720p", "1080p", "1440p"};

constexpr int32_t kStockBarWidth = 600;
constexpr int32_t kWideBarWidth = 900;         // fits "30 FPS"/"60 FPS"-length text
constexpr int32_t kResolutionBarWidth = 750;   // fits "1080p"/"1440p", narrower than kWideBarWidth

constexpr OptionRow kOptionRows[] = {
    {&kLabelResolution, kResolutionValues, 3, &ResolutionGetIndex, &ResolutionSetIndex,
     kResolutionBarWidth},
    {&kLabelFrameRate, kFrameRateValues, 2, &FrameRateGetIndex, &FrameRateSetIndex,
     kWideBarWidth},
    {&kLabelFullscreen, kBoolValues, 2, &FullscreenGetIndex, &FullscreenSetIndex,
     kStockBarWidth},
};
constexpr u32 kOptionRowCount = static_cast<u32>(std::size(kOptionRows));

u32 g_bar_id[kOptionRowCount] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};  // per-row bar registry id
u32 g_label_addr[kOptionRowCount] = {};                                   // per-row label string
u32 g_value_addr[kOptionRowCount][kMaxRowValues] = {};  // per-row literal value strings
u32 g_bar_vec = 0;  // shared guest scratch for the {x, y, z} move argument

u32 g_fs_list = 0;   // guest address of the extended display list
bool g_fs_failed = false;
bool g_options_active = false;  // Options screen was the last one built
bool g_bar_dumped = false;      // bar objects already dumped for this entry

// Selection geometry, measured at runtime (see docs §14). The Options screen's
// bottom group is id 2, holding two sub-items at screen y 430 and 480; screen
// y is display y + 145, so row r sits at 530 + 50*r.
constexpr u32 kOptGroupId = 2;
constexpr u32 kOptGroupCount = 2;   // stock rows the group ships with
constexpr int32_t kOptRow0Y = 430;  // Sottotitoli, used to identify the group
constexpr int32_t kOptRow1Y = 480;  // Voce
constexpr u8 kSubtitleRowIndex = 0;  // Sottotitoli, the reference two-option row

// Pad state. sub_821281B8 polls an array of 4 pad objects at 0x824BB418,
// stride 464; sub_82128310 fills each one: +424 = buttons held this frame,
// +8 = previous, +428 = newly pressed this frame, +432 = pressed-or-repeat,
// +436 = released. We want the +428 edge so one press is one toggle.
constexpr u32 kPad0 = 0x824BB418u;
constexpr u32 kPadPressed = 428u;
constexpr u32 kBtnDPadLeft = 0x0004u;
constexpr u32 kBtnDPadRight = 0x0008u;
constexpr u32 kBtnA = 0x1000u;
// sub_82128310 also folds the left stick into synthetic bits; confirmed live:
// 0x10000/0x20000 = up/down, 0x40000/0x80000 = left/right. Matching only the
// d-pad meant stick input never reached the handler.
constexpr u32 kBtnStickLeft = 0x40000u;
constexpr u32 kBtnStickRight = 0x80000u;
constexpr u32 kLeftMask = kBtnDPadLeft | kBtnStickLeft;
constexpr u32 kRightMask = kBtnDPadRight | kBtnStickRight;

REX_IMPORT(__imp__sub_82179F78, g_move_object,
           void(double, double, double, double, u32, u32, u32, u32, u32, u32, u32, u32));

// sub_82178A88(&registry, id, &{x,y,z}, 0, 0, -1) - the instant placement the
// screen init uses, with no animation.
REX_IMPORT(__imp__sub_82178A88, g_set_object_pos, void(u32, u32, u32, u32, u32, u32));

// sub_821F6580(root, id) -> object. The id->object resolver sub_82200FE8 itself
// uses on these same ids.
REX_IMPORT(__imp__sub_821F6580, g_resolve_object, u32(u32, u32));

// Slides row `row`'s highlight bar onto value `value_index`. Called on
// Options entry (so each bar starts on the active value) and on every
// change. `move` picks the mechanism: the animated slide the row handler
// uses on a value change, or the instant set the screen init uses when
// Options opens.
void MoveOptionBar(u8* base, u32 row, int value_index, bool move) {
  if (row >= kOptionRowCount || g_bar_id[row] == 0xFFFFFFFFu || !g_bar_vec) {
    return;
  }
  // Mirror sub_82201620's own base-X selection. Its third case (language >= 7)
  // reads an uninitialised local, so it is deliberately not reproduced.
  const u32 lang = REX_LOAD_U32(kLanguage);
  const int32_t base_x = (lang >= 3 && lang < 5) ? 530 : 480;
  const int32_t x = base_x + value_index * kBarColumnStride;
  const int32_t handler_y =
      kRowY0 + kRowYStep * static_cast<int32_t>(row) + 130 + kBarShrinkFixup;
  const int32_t y = move ? handler_y : handler_y + 740;

  const auto put = [&](u32 off, float v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    REX_STORE_U32(g_bar_vec + off, bits);
  };
  put(0, static_cast<float>(x));
  put(4, static_cast<float>(y));
  put(8, 0.0f);

  if (move) {
    g_move_object(0.0, kBarMoveSpeed, kBarMoveSpeed, kBarMoveSpeed, kTextRegistry,
                  g_bar_id[row], g_bar_vec, 0, 0, 0, 0, kBarMoveMode);
  } else {
    g_set_object_pos(kTextRegistry, g_bar_id[row], g_bar_vec, 0, 0, 0xFFFFFFFFu);
  }
}

bool FullscreenEnabled() {
  const auto* entry = rex::cvar::GetFlagInfo("fullscreen");
  return entry && entry->getter() == "true";
}

int FullscreenGetIndex() { return FullscreenEnabled() ? 0 : 1; }

void FullscreenSetIndex(u8* base, int idx) {
  const bool on = (idx == 0);
  if (FullscreenEnabled() == on) {
    return;
  }
  // settings.cpp owns the window and the settings file, so the cvar update,
  // the actual window mode change and persistence all happen there.
  eternalsonata::SetFullscreenSetting(on);
  REXLOG_INFO("[options] fullscreen -> {}", on ? "true" : "false");
}

int FrameRateGetIndex() {
  const std::string cur = REXCVAR_GET(frame_rate);
  for (int i = 0; i < static_cast<int>(std::size(kFrameRateIds)); ++i) {
    if (cur == kFrameRateIds[i]) {
      return i;
    }
  }
  return 0;
}

void FrameRateSetIndex(u8* base, int idx) {
  eternalsonata::SetFrameRateSetting(kFrameRateIds[idx]);
  REXLOG_INFO("[options] frame_rate -> {}", kFrameRateIds[idx]);
}

int ResolutionGetIndex() {
  const auto* entry = rex::cvar::GetFlagInfo("resolution");
  const std::string cur = entry ? entry->getter() : std::string();
  for (int i = 0; i < static_cast<int>(std::size(kResolutionIds)); ++i) {
    if (cur == kResolutionIds[i]) {
      return i;
    }
  }
  return 0;
}

void ResolutionSetIndex(u8* base, int idx) {
  eternalsonata::SetResolutionSetting(kResolutionIds[idx]);
  REXLOG_INFO("[options] resolution -> {}", kResolutionIds[idx]);
}

void WriteGuestString(u8* base, u32 at, const char* s) {
  for (u32 i = 0;; ++i) {
    REX_STORE_U8(at + i, static_cast<u8>(s[i]));
    if (!s[i]) {
      return;
    }
  }
}

void WriteTextRecord(u8* base, u32 at, u32 id, int32_t x, int32_t y) {
  REX_STORE_U32(at + 0x00, kTextRecord);
  REX_STORE_U32(at + 0x04, id);
  REX_STORE_U32(at + 0x08, static_cast<u32>(x));
  REX_STORE_U32(at + 0x0C, static_cast<u32>(y));
  REX_STORE_U32(at + 0x10, 300);  // width, as on every other row
  REX_STORE_U32(at + 0x14, 48);   // height
  REX_STORE_U32(at + 0x18, 0);
  REX_STORE_U32(at + 0x1C, kTextRecordBytes);
  REX_STORE_U32(at + 0x20, 0xFFFFFFFFu);
  REX_STORE_U32(at + 0x24, 1);
}

// Clone the stock Subtitles bar record verbatim and move it to our row's y.
// Cloning rather than hand-writing keeps every field we have not identified
// (notably the width at +0xC) at whatever the game already uses.
void WriteBarRecord(u8* base, u32 at, u32 src_list, int32_t y, int32_t width) {
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + kBarRecordOffset),
              kBarRecordBytes);
  REX_STORE_U32(at + kBarBlockYOffset, static_cast<u32>(y));
  REX_STORE_U32(at + kBarBlockWOffset, static_cast<u32>(width));
  REX_STORE_U32(at + kBarBlockHOffset, static_cast<u32>(kBarHeight));
}

int32_t RowBarDisplayY(u32 row) {
  return kRowY0 + kRowYStep * static_cast<int32_t>(row) + kBarShrinkFixup;
}

// Builds the extended list: every kOptionRows entry gets a label + N value
// text records at its own 50px-pitch row, then every row's bar record,
// appended after the stock icon record (see the ordering rule below).
//
// The contents are rewritten on every Options entry rather than built once.
// That costs nothing (it is a memcpy of under 0x900 bytes) and keeps each
// row's value text in step with its cvar, since the interpreter only reads
// the list at screen construction.
void EnsureOptionRows(u8* base, int lang_idx) {
  if (g_fs_failed) {
    return;
  }
  auto* mem = rex::system::kernel_memory();
  if (!mem) {
    g_fs_failed = true;
    return;
  }

  u32 bytes = kOptionsListBytes + kIconRecordBytes + static_cast<u32>(sizeof(u32));
  for (const OptionRow& row : kOptionRows) {
    bytes += kTextRecordBytes + row.value_count * kTextRecordBytes + kBarRecordBytes;
  }

  if (!g_fs_list) {
    g_fs_list = mem->SystemHeapAlloc(bytes, 0x20);
    if (!g_fs_list) {
      g_fs_failed = true;
      REXLOG_WARN("[options] native rows: guest allocation failed");
      return;
    }
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      g_label_addr[r] = mem->SystemHeapAlloc(64, 0x20);
      if (!g_label_addr[r]) {
        g_fs_failed = true;
        REXLOG_WARN("[options] native rows: guest allocation failed");
        return;
      }
      for (u32 v = 0; v < kOptionRows[r].value_count; ++v) {
        const OptionValue& val = kOptionRows[r].values[v];
        if (!val.literal) {
          continue;
        }
        g_value_addr[r][v] = mem->SystemHeapAlloc(64, 0x20);
        if (!g_value_addr[r][v]) {
          g_fs_failed = true;
          REXLOG_WARN("[options] native rows: guest allocation failed");
          return;
        }
        WriteGuestString(base, g_value_addr[r][v], val.literal);
      }
    }
    g_bar_vec = mem->SystemHeapAlloc(16, 0x20);
  }
  const u32 list = g_fs_list;

  // The source Options list is per-language (see kOptionsListByLang) - same
  // byte layout at a different address - so every copy below reads from
  // whichever one is active right now rather than a single fixed address.
  const u32 src_list = kOptionsListByLang[lang_idx];

  // Labels are rewritten every entry (not just on first allocation) so a
  // language change while playing takes effect the next time Options opens,
  // matching the game's own text - and matching how the values below are
  // already rebuilt every entry to stay in step with their cvars.
  for (u32 r = 0; r < kOptionRowCount; ++r) {
    WriteGuestString(base, g_label_addr[r], kOptionRows[r].label->text[lang_idx]);
  }

  // Insert, do not append. Records past the last row set drawing state, so a
  // row appended at the very end inherits that trailing state and renders in
  // the wrong place (observed: label offset from the cursor, which sat
  // correctly at y=530). Splicing the records in immediately after the
  // Sottotitoli record keeps them in the same state as the real rows.
  std::memcpy(REX_RAW_ADDR(list), REX_RAW_ADDR(src_list), kInsertOffset);
  u32 at = list + kInsertOffset;

  // Mirror the Sottotitoli row's layout for every row: label at X=120 and
  // every value drawn side by side from X=490, 200px apart.
  for (u32 r = 0; r < kOptionRowCount; ++r) {
    const int32_t y = kRowY0 + kRowYStep * static_cast<int32_t>(r);
    WriteTextRecord(base, at, kRowSidBase + kRowSidStride * r, kRowXLabel, y);
    at += kTextRecordBytes;
    for (u32 v = 0; v < kOptionRows[r].value_count; ++v) {
      WriteTextRecord(base, at, kRowSidBase + kRowSidStride * r + 1 + v,
                       kRowXValue0 + static_cast<int32_t>(v) * kBarColumnStride, y);
      at += kTextRecordBytes;
    }
  }

  // The stock bar record sits at exactly kInsertOffset, and it is the *last*
  // object-creating record in the list - everything after it is the
  // 1500/1502 selection block. That matters: sub_82201620 indexes the id array
  // at screen+0x4C **positionally** (elements 2/3/4 and 11), so a bar inserted
  // anywhere earlier would renumber the stock rows and move their highlights.
  // Copy the stock record through first, then append ours, so each row's bar
  // takes the next free index and nothing shifts.
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + kInsertOffset),
              kIconRecordBytes);
  at += kIconRecordBytes;
  for (u32 r = 0; r < kOptionRowCount; ++r) {
    WriteBarRecord(base, at, src_list, RowBarDisplayY(r), kOptionRows[r].bar_width);
    at += kBarRecordBytes;
  }
  const u32 rest = kInsertOffset + kIconRecordBytes;
  std::memcpy(REX_RAW_ADDR(at), REX_RAW_ADDR(src_list + rest),
              kOptionsListBytes - rest);
  at += kOptionsListBytes - rest;
  REX_STORE_U32(at, kListTerminator);

  REXLOG_INFO("[options] {} native rows built (list=0x{:08X})", kOptionRowCount, list);
}

}  // namespace

// sub_8223B780(blob, string_id) -> char*: the BTX text lookup. Answer our
// synthetic ids ourselves and let every real id fall through untouched.
REX_EXTERN(__imp__sub_8223B780);

REX_HOOK_RAW(sub_8223B780) {
  const u32 sid = ctx.r4.u32;
  if (sid >= kRowSidBase) {
    const u32 rel = sid - kRowSidBase;
    const u32 row = rel / kRowSidStride;
    const u32 sub = rel % kRowSidStride;
    if (row < kOptionRowCount) {
      const OptionRow& def = kOptionRows[row];
      if (sub == 0) {
        if (g_label_addr[row]) {
          ctx.r3.u32 = g_label_addr[row];
          return;
        }
      } else if (sub - 1 < def.value_count) {
        const u32 v = sub - 1;
        const OptionValue& val = def.values[v];
        if (val.literal) {
          if (g_value_addr[row][v]) {
            ctx.r3.u32 = g_value_addr[row][v];
            return;
          }
        } else {
          // Reuse the game's own string so it reads correctly in whatever
          // language is active: rewrite r4 and let the stock lookup do the
          // work, keeping localisation free.
          //
          // NOTE: an earlier attempt marked the inactive option by prefixing
          // the game's "<g>" tag (seen in strings like "<g>You have no score
          // pieces."). That tag is NOT interpreted here - it rendered
          // literally, which is why the dimmed option appeared shifted three
          // characters right. Every option is therefore drawn plain;
          // indicating which one is active still needs the game's own
          // highlight mechanism (see docs §14, open work).
          ctx.r4.u32 = val.btx_id;
          __imp__sub_8223B780(ctx, base);
          return;
        }
      }
    }
  }
  __imp__sub_8223B780(ctx, base);
}

// sub_821F2F38(a1, list, ...): the display-list interpreter. Swap the Options
// list for our extended copy; every other screen is left alone.
REX_EXTERN(__imp__sub_821F2F38);

REX_HOOK_RAW(sub_821F2F38) {
  // The interpreter runs once per screen build, so the list it is handed is a
  // reliable "which screen is up" signal for the selection patch below.
  // Measured: this fires exactly once per Options entry, not per frame - so
  // the display list is a build step and nothing in it can animate. Anything
  // that moves while the screen is up (the cursor, the value highlight) is a
  // live object driven elsewhere.
  const int lang_idx = OptionsListIndex(ctx.r4.u32);
  g_options_active = (lang_idx >= 0);
  g_bar_dumped = false;  // re-dump the bar objects on each Options entry
  if (g_options_active) {
    EnsureOptionRows(base, lang_idx);
    if (g_fs_list) {
      ctx.r4.u32 = g_fs_list;
    }
  }
  __imp__sub_821F2F38(ctx, base);
}

// ---------------------------------------------------------------------------
// Value-highlight hunt: live memory differ
// ---------------------------------------------------------------------------
//
// The bar that marks a two-option row's active choice is still unidentified
// (docs/debug-hooks.md §14). Every static lead was ruled out, and the one
// targeted diff that was tried only covered 0x800 bytes of the 0x824D0440
// config struct - which does not even hold the Subtitles setting, or that diff
// would have caught it. So do it properly: snapshot *all* committed guest
// memory in state A, again in state B, and intersect across repeated toggles.
// Whatever survives is the state the highlight is driven by; from there a
// watchpoint on the survivor finds the code that reads it.
//
// Workflow (keys are polled here because this hook runs every menu frame):
//   F9  - capture state A   (e.g. Sottotitoli = Si)
//   F10 - capture state B   (e.g. Sottotitoli = NO)
//   F11 - report surviving candidates
//   F12 - reset the hunt
// Alternate F9/F10 across several toggles; the candidate set collapses fast.
//
// This is a debug tool, inert unless those keys are pressed.

namespace {

// Guest virtual memory, plus the 4 KiB view of the physical heap. The
// 0xA0000000 and 0xC0000000 views alias the same physical pages as 0xE0000000,
// so scanning them would only produce duplicate hits.
constexpr struct {
  u32 lo, hi;
} kScanRanges[] = {{0x82000000u, 0xA0000000u}, {0xE0000000u, 0xFB000000u}};
constexpr u32 kScanPage = 0x1000u;

struct ScanCandidate {
  u32 addr;
  u32 a;  // raw big-endian, as stored in guest memory
  u32 b;
};

std::vector<u32> g_scan_pages;      // committed page bases, collected once
std::vector<u32> g_scan_snap;       // state-A snapshot, kScanPage/4 dwords each
std::vector<ScanCandidate> g_scan_cands;
bool g_scan_have_a = false;
bool g_scan_have_cands = false;
size_t g_scan_last_size = 0;  // convergence tracking for the auto-report
int g_scan_stable = 0;
bool g_scan_reported = false;

// Collects the committed pages once. Uncommitted pages must be skipped: the
// guest address space is reserved as one big host range, so touching a page
// that was never committed faults.
void ScanCollectPages() {
  auto* mem = rex::system::kernel_memory();
  if (!mem) {
    return;
  }
  g_scan_pages.clear();
  for (const auto& r : kScanRanges) {
    for (u32 p = r.lo; p < r.hi; p += kScanPage) {
      auto* heap = mem->LookupHeap(p);
      if (!heap) {
        continue;
      }
      u32 protect = 0;
      if (!heap->QueryProtect(p, &protect) || protect == 0) {
        continue;
      }
      g_scan_pages.push_back(p);
    }
  }
  REXLOG_INFO("[scan] {} committed pages ({} MiB)", g_scan_pages.size(),
              (g_scan_pages.size() * kScanPage) >> 20);
}

// REX_RAW_ADDR, not `base + addr`: the physical views above 0xE0000000 carry a
// +0x1000 host offset on Windows.
inline u32 ScanRead(u8* base, u32 addr) {
  u32 v;
  std::memcpy(&v, REX_RAW_ADDR(addr), sizeof(v));
  return v;  // kept raw (big-endian); only swapped when reported
}

void ScanCaptureA(u8* base) {
  if (g_scan_pages.empty()) {
    ScanCollectPages();
    if (g_scan_pages.empty()) {
      return;
    }
  }
  if (g_scan_have_cands) {
    // Filter: a real candidate must return to its state-A value.
    const size_t before = g_scan_cands.size();
    std::erase_if(g_scan_cands, [&](const ScanCandidate& c) {
      return ScanRead(base, c.addr) != c.a;
    });
    REXLOG_INFO("[scan] A: {} -> {} candidates", before, g_scan_cands.size());
    return;
  }
  g_scan_snap.resize(g_scan_pages.size() * (kScanPage / sizeof(u32)));
  for (size_t i = 0; i < g_scan_pages.size(); ++i) {
    std::memcpy(&g_scan_snap[i * (kScanPage / sizeof(u32))],
                REX_RAW_ADDR(g_scan_pages[i]), kScanPage);
  }
  g_scan_have_a = true;
  REXLOG_INFO("[scan] baseline A captured");
}

void ScanCaptureB(u8* base) {
  if (!g_scan_have_a) {
    REXLOG_WARN("[scan] press F9 for a state-A baseline first");
    return;
  }
  if (g_scan_have_cands) {
    const size_t before = g_scan_cands.size();
    std::erase_if(g_scan_cands, [&](const ScanCandidate& c) {
      return ScanRead(base, c.addr) != c.b;
    });
    REXLOG_INFO("[scan] B: {} -> {} candidates", before, g_scan_cands.size());
    return;
  }
  // First B: everything that moved since the baseline becomes a candidate.
  constexpr size_t kDwords = kScanPage / sizeof(u32);
  for (size_t i = 0; i < g_scan_pages.size(); ++i) {
    const u32 page = g_scan_pages[i];
    const u32* snap = &g_scan_snap[i * kDwords];
    for (size_t j = 0; j < kDwords; ++j) {
      const u32 now = ScanRead(base, page + static_cast<u32>(j * 4));
      if (now != snap[j]) {
        g_scan_cands.push_back({page + static_cast<u32>(j * 4), snap[j], now});
      }
    }
  }
  g_scan_have_cands = true;
  g_scan_snap.clear();
  g_scan_snap.shrink_to_fit();
  REXLOG_INFO("[scan] {} initial candidates", g_scan_cands.size());
}

// Dumps every survivor to a file - the set converges to a few hundred, which is
// far too many for the log but trivial to grep offline. Values are shown as
// hex, signed int and float, because a menu coordinate could plausibly be any
// of the three.
void ScanReport(u8* base) {
  FILE* f = std::fopen("logs/scan_candidates.txt", "w");
  if (!f) {
    REXLOG_WARN("[scan] cannot open logs/scan_candidates.txt");
    return;
  }
  std::fprintf(f, "# addr        A(hex)     A(int)   A(float)      B(hex)     B(int)   B(float)\n");
  for (const auto& c : g_scan_cands) {
    const u32 a = __builtin_bswap32(c.a);
    const u32 b = __builtin_bswap32(c.b);
    float af, bf;
    std::memcpy(&af, &a, 4);
    std::memcpy(&bf, &b, 4);
    std::fprintf(f, "0x%08X  0x%08X %10d %12g   0x%08X %10d %12g\n", c.addr, a,
                 static_cast<int32_t>(a), af, b, static_cast<int32_t>(b), bf);
  }
  std::fclose(f);
  REXLOG_INFO("[scan] {} candidates written to logs/scan_candidates.txt",
              g_scan_cands.size());
}

void ScanReset() {
  g_scan_cands.clear();
  g_scan_snap.clear();
  g_scan_snap.shrink_to_fit();
  g_scan_have_a = false;
  g_scan_have_cands = false;
  g_scan_last_size = 0;
  g_scan_stable = 0;
  g_scan_reported = false;
  REXLOG_INFO("[scan] reset");
}

// Auto-capture, driven by the Subtitles row itself. A two-option row is
// idempotent per direction: left always selects the left option, right the
// right one. So "left pressed" is unambiguously state A and "right pressed" is
// state B, no matter what order they come in - the user just parks the cursor
// on Subtitles and alternates left/right, and the candidate set collapses on
// its own. Captures are deferred a few frames so the value (and the bar) have
// settled before the snapshot.
int g_scan_pending = 0;   // frames left before the deferred capture
bool g_scan_pending_a = false;

void ScanOnRowInput(u8* base, bool left) {
  g_scan_pending = 4;
  g_scan_pending_a = left;
}

void ScanTick(u8* base) {
  if (g_scan_pending && --g_scan_pending == 0) {
    if (g_scan_pending_a) {
      ScanCaptureA(base);
    } else {
      ScanCaptureB(base);
    }
    // Auto-report once the set stops shrinking. Waiting for some small
    // threshold is wrong - the true answer set has a floor (a few hundred
    // addresses genuinely alternate with the setting), so "converged" is the
    // real signal, not "small". Dumping again on every later capture would
    // also let a stray press overwrite a good dump with an empty one.
    if (g_scan_have_cands) {
      g_scan_stable = (g_scan_cands.size() == g_scan_last_size) ? g_scan_stable + 1 : 0;
      g_scan_last_size = g_scan_cands.size();
      if (g_scan_stable == 3 && !g_scan_reported && !g_scan_cands.empty()) {
        g_scan_reported = true;
        ScanReport(base);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Highlight-bar dump
// ---------------------------------------------------------------------------
//
// sub_82201620 (the Options row input handler) reaches a row's highlight bar as
// `*(u32*)(root[709] + N)` with N = 84/88/92 for the page-1 rows, 120 for
// Subtitles and 92 for Voice - where `root` is dword_824400E4 and `[709]` is
// the same `4 * (page + 709)` slot sub_821F2F38 allocates the 1744-byte screen
// object into. Walk that chain and dump what is actually there: the object's
// vtable identifies its class, which is what a new bar would have to be built
// as. Runs once per Options entry, gated behind menu_scan.
constexpr u32 kUiRoot = 0x824400E4u;
constexpr u32 kScreenSlotBase = 709u;
constexpr u32 kBarOffsets[] = {84, 88, 92, 120};

inline bool GuestPtr(u32 v) { return v >= 0x82000000u && v < 0xFB000000u; }

void DumpHighlightBars(u8* base) {
  const u32 root = REX_LOAD_U32(kUiRoot);
  if (!GuestPtr(root)) {
    return;
  }
  const u32 page = REX_LOAD_U8(root + 2833);
  const u32 screen = REX_LOAD_U32(root + 4 * (page + kScreenSlotBase));
  REXLOG_INFO("[bar] root=0x{:08X} page={} screen=0x{:08X}", root, page, screen);
  if (!GuestPtr(screen)) {
    return;
  }

  // The four "bar" offsets are not separate fields at all: 84/88/92/120 are
  // simply elements 2/3/4/11 of one array of registry ids at +0x4C, filled in
  // display-list order. Log the array and its count byte directly, so a
  // controlled A/B can be read straight out of the log.
  u32 n = 0;
  std::string ids;
  for (; n < 24; ++n) {
    const u32 id = REX_LOAD_U32(screen + 0x4C + 4 * n);
    if (id == 0xFFFFFFFFu) {
      break;
    }
    ids += fmt::format("{:02X} ", id);
  }
  REXLOG_INFO("[bar] count@0x17C={} ids@0x4C({})= {}",
              REX_LOAD_U8(screen + 0x17C), n, ids);

  // Resolve the stock Subtitles bar and row 0's (Fullscreen's), and dump
  // both. Comparing the two objects field by field is how the residual
  // vertical offset gets fixed exactly: whatever field differs by something
  // other than the 50px row pitch is the one the height scale is disturbing.
  const u32 stock_id = REX_LOAD_U32(screen + 120);
  for (int which = 0; which < 2; ++which) {
    const u32 id = which ? g_bar_id[0] : stock_id;
    if (id == 0xFFFFFFFFu) {
      continue;
    }
    const u32 obj = g_resolve_object(root, id);
    REXLOG_INFO("[bar] {} bar id=0x{:X} obj=0x{:08X}", which ? "ours" : "stock",
                id, obj);
    if (!GuestPtr(obj)) {
      continue;
    }
    for (u32 o = 0; o < 0x80; o += 0x10) {
      REXLOG_INFO("[bar]   +0x{:02X}: {:08X} {:08X} {:08X} {:08X}", o,
                  REX_LOAD_U32(obj + o), REX_LOAD_U32(obj + o + 4),
                  REX_LOAD_U32(obj + o + 8), REX_LOAD_U32(obj + o + 12));
    }
  }

  for (const u32 off : kBarOffsets) {
    const u32 obj = REX_LOAD_U32(screen + off);
    if (!GuestPtr(obj)) {
      REXLOG_INFO("[bar] screen+{:3} = 0x{:08X} (not an object)", off, obj);
      continue;
    }
    // +0 is the vtable on every object in this UI (sub_820E64F0 sets it), so
    // it is the class fingerprint - and it is a static address, which means it
    // can be looked up in IDA to find the constructor and thence the record
    // handler that builds one.
    const u32 vtable = REX_LOAD_U32(obj);
    REXLOG_INFO("[bar] screen+{:3} -> obj=0x{:08X} vtable=0x{:08X}", off, obj,
                vtable);
    for (u32 row = 0; row < 0x60; row += 0x10) {
      REXLOG_INFO("[bar]   +0x{:02X}: {:08X} {:08X} {:08X} {:08X}", row,
                  REX_LOAD_U32(obj + row), REX_LOAD_U32(obj + row + 4),
                  REX_LOAD_U32(obj + row + 8), REX_LOAD_U32(obj + row + 12));
    }
  }

  // Dump the whole 1744-byte screen object. The four known slots hold small
  // integers, not pointers - they are registry ids (sub_82179F78 forwards its
  // second argument straight to sub_821771F8 as an id), so the rest of the
  // struct is where any *other* per-row id lives. Diffing this file with and
  // without the probe bar record is what identifies our row's id.
  static int s_dump_index = 0;
  char path[128];
  std::snprintf(path, sizeof(path), "logs/screen_dump_%d.txt", s_dump_index++);
  FILE* f = std::fopen(path, "w");
  if (!f) {
    return;
  }
  std::fprintf(f, "# screen object 0x%08X (1744 bytes)\n", screen);
  for (u32 o = 0; o < 1744; o += 16) {
    std::fprintf(f, "+0x%04X  %08X %08X %08X %08X\n", o, REX_LOAD_U32(screen + o),
                 REX_LOAD_U32(screen + o + 4), REX_LOAD_U32(screen + o + 8),
                 REX_LOAD_U32(screen + o + 12));
  }
  std::fclose(f);
  REXLOG_INFO("[bar] screen object written to {}", path);
}

// Edge-detected hotkeys. GetAsyncKeyState is fine to call from the guest
// thread; nothing here runs unless a key is actually pressed.
void ScanPollKeys(u8* base) {
#ifdef _WIN32
  static const struct {
    int vk;
    void (*fn)(u8*);
  } kKeys[] = {
      {VK_F9, &ScanCaptureA},
      {VK_F10, &ScanCaptureB},
      {VK_F11, &ScanReport},
      {VK_F12, [](u8*) { ScanReset(); }},
  };
  static bool s_down[std::size(kKeys)] = {};
  for (size_t i = 0; i < std::size(kKeys); ++i) {
    const bool down = (GetAsyncKeyState(kKeys[i].vk) & 0x8000) != 0;
    if (down && !s_down[i]) {
      kKeys[i].fn(base);
    }
    s_down[i] = down;
  }
#else
  (void)base;
#endif
}

}  // namespace

// sub_821F62B8 is the per-frame cursor update for the menu. We piggyback on it
// to keep the native rows selectable (the screen resets the group's count on
// every rebuild) and to handle input while the cursor sits on one of them.
REX_EXTERN(__imp__sub_821F62B8);

REX_HOOK_RAW(sub_821F62B8) {
  __imp__sub_821F62B8(ctx, base);

  if (!g_options_active || !g_fs_list) {
    return;
  }
  if (!g_bar_dumped) {
    g_bar_dumped = true;
    // Bar records are appended in row order after every stock object-creating
    // record, so our ids are always the last kOptionRowCount entries of the
    // array at screen+0x4C, in row order.
    const u32 root = REX_LOAD_U32(kUiRoot);
    if (root >= 0x82000000u && root < 0xFB000000u) {
      const u32 page = REX_LOAD_U8(root + 2833);
      const u32 screen = REX_LOAD_U32(root + 4 * (page + kScreenSlotBase));
      if (screen >= 0x82000000u && screen < 0xFB000000u) {
        u32 ids[32];
        u32 n = 0;
        for (; n < std::size(ids); ++n) {
          const u32 id = REX_LOAD_U32(screen + 0x4C + 4 * n);
          if (id == 0xFFFFFFFFu) {
            break;
          }
          ids[n] = id;
        }
        if (n >= kOptionRowCount) {
          for (u32 r = 0; r < kOptionRowCount; ++r) {
            g_bar_id[r] = ids[n - kOptionRowCount + r];
          }
        }
        REXLOG_INFO("[options] bar ids: {:x} {:x} {:x}", g_bar_id[0], g_bar_id[1],
                    g_bar_id[2]);
      }
    }
    // Place every bar the way the screen init places the stock ones:
    // instantly, in init space. No settling delay - sub_82178A88 is not an
    // animation.
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      MoveOptionBar(base, r, kOptionRows[r].get_index(), /*move=*/false);
    }
    if (REXCVAR_GET(menu_scan)) {
      DumpHighlightBars(base);
    }
  }
  const u32 menu = REX_LOAD_U32(0x824400E8u);
  if (menu < 0x82000000u || menu >= 0xFB000000u) {
    return;
  }

  // Find the Options screen's bottom group and give it kOptionRowCount more
  // rows. Each node is {id @ +0, subitem block @ +4, subitem pointer array @
  // +8, count byte @ +0x0C (mirrored at +0x0D), current index @ +0x2C, next @
  // +0x30}. The array is pre-allocated with 10 slots; unused ones are parked
  // at the sentinel (10000 + i, 10000 + i), so no allocation is needed - only
  // a position and a bigger count.
  //
  // The count check also makes this idempotent: after patching, count no
  // longer matches kOptGroupCount, so it stops matching until the screen is
  // rebuilt (which resets the count to kOptGroupCount).
  for (u32 i = REX_LOAD_U32(menu + 392);
       i >= 0x82000000u && i < 0xFB000000u; i = REX_LOAD_U32(i + 48)) {
    if (REX_LOAD_U32(i) != kOptGroupId ||
        REX_LOAD_U8(i + 0x0C) != kOptGroupCount) {
      continue;
    }
    const u32 arr = REX_LOAD_U32(i + 8);
    if (arr < 0x82000000u || arr >= 0xFB000000u) {
      continue;
    }
    const u32 s0 = REX_LOAD_U32(arr);
    const u32 s1 = REX_LOAD_U32(arr + 4);
    if (s0 < 0x82000000u || s0 >= 0xFB000000u || s1 < 0x82000000u ||
        s1 >= 0xFB000000u) {
      continue;
    }
    // Confirm this really is the Options bottom group before writing.
    if (static_cast<int32_t>(REX_LOAD_U32(s0 + 8)) != kOptRow0Y ||
        static_cast<int32_t>(REX_LOAD_U32(s1 + 8)) != kOptRow1Y) {
      continue;
    }

    u32 srow[kOptionRowCount];
    bool ok = true;
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      const u32 s = REX_LOAD_U32(arr + 8 + 4 * r);
      if (s < 0x82000000u || s >= 0xFB000000u) {
        ok = false;
        break;
      }
      srow[r] = s;
    }
    if (!ok) {
      continue;
    }

    // The cursor column x is per-language (confirmed by diffing the raw
    // display lists 2026-08-06: Italian/French author 550, English 500), so
    // it has to be read from a stock row rather than hardcoded - a mismatch
    // here is what made Down-navigation skip straight over the new rows in
    // English/German/Spanish despite the count byte being patched correctly.
    const u32 opt_x = REX_LOAD_U32(s0 + 4);
    for (u32 r = 0; r < kOptionRowCount; ++r) {
      const int32_t y = kOptRow1Y + kRowYStep * static_cast<int32_t>(r + 1);
      REX_STORE_U32(srow[r] + 4, opt_x);
      REX_STORE_U32(srow[r] + 8, static_cast<u32>(y));
    }
    REX_STORE_U8(i + 0x0C, static_cast<u8>(kOptGroupCount + kOptionRowCount));
    REX_STORE_U8(i + 0x0D, static_cast<u8>(kOptGroupCount + kOptionRowCount));
    REXLOG_INFO("[options] {} native rows made selectable (node=0x{:08X})",
                kOptionRowCount, i);
    break;
  }

  // Handle input, but only while the cursor is actually parked on one of our
  // rows: group 2 selected and its sub-index at or past kOptGroupCount. The
  // game has no handler of its own for those indices, so nothing else
  // consumes the press.
  if (REX_LOAD_U32(menu + 396) != kOptGroupId) {
    return;
  }
  for (u32 i = REX_LOAD_U32(menu + 392);
       i >= 0x82000000u && i < 0xFB000000u; i = REX_LOAD_U32(i + 48)) {
    if (REX_LOAD_U32(i) != kOptGroupId) {
      continue;
    }
    const u8 row = REX_LOAD_U8(i + 0x2C);

    // The just-pressed mask stays set for a whole guest frame, so latch on our
    // own observation of the transition - this stays correct even if the cursor
    // update runs more than once per frame.
    static u32 s_prev_pressed = 0;
    const u32 pressed = REX_LOAD_U32(kPad0 + kPadPressed);
    const u32 fresh = pressed & ~s_prev_pressed;
    s_prev_pressed = pressed;

    // Subtitles (row 0 of this group) is the reference two-option row for the
    // highlight hunt - it is a stock row, so its value and highlight move
    // through the game's own code path.
    if (row == kSubtitleRowIndex && REXCVAR_GET(menu_scan)) {
      if (fresh & kLeftMask) {
        ScanOnRowInput(base, true);
      } else if (fresh & kRightMask) {
        ScanOnRowInput(base, false);
      }
    }

    if (row < kOptGroupCount || row - kOptGroupCount >= kOptionRowCount) {
      return;
    }
    const u32 opt_row = row - kOptGroupCount;
    const OptionRow& def = kOptionRows[opt_row];
    const int cur = def.get_index();
    int next = cur;

    // Match how a real two-option row behaves: left steps toward the first
    // value, right toward the last (clamped at the ends, not wrapping) - for
    // a boolean row that is exactly "left picks Si, right picks NO". A cycles
    // forward through every value, wrapping.
    if (fresh & kLeftMask) {
      if (cur > 0) {
        next = cur - 1;
      }
    } else if (fresh & kRightMask) {
      if (cur < static_cast<int>(def.value_count) - 1) {
        next = cur + 1;
      }
    } else if (fresh & kBtnA) {
      next = (cur + 1) % static_cast<int>(def.value_count);
    }

    if (next != cur) {
      def.set_index(base, next);
      MoveOptionBar(base, opt_row, next, /*move=*/true);
    }
    return;
  }
}
