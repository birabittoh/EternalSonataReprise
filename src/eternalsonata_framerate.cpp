#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <imgui.h>
#include <rex/cvar.h>

#include "eternalsonata_hooks_internal.h"

// frame_rate cvar: "30" / "60" / "unlocked". Defined (and persisted) in
// settings.cpp; declared here so the frame-driver hook can read it cheaply.
REXCVAR_DECLARE(std::string, frame_rate);
REXCVAR_DECLARE(bool, frame_debug);
REXCVAR_DECLARE(bool, adaptive_framerate);

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

REX_HOOK_RAW(sub_8210AAD8) {
  // The original clobbers r3, so capture the render-pump object up front.
  const u32 a1 = ctx.r3.u32;
  const u8 fps = AdaptiveFrameRate(RequestedFrameRate(g_guest_rate), g_guest_rate, /*measure=*/true);
  if (fps != g_applied_fps) {
    ApplyFrameRate(ctx, base, fps);
    g_applied_fps = fps;
  }
  __imp__sub_8210AAD8(ctx, base);
  // These live in eternalsonata_options.cpp (the value-highlight memory
  // differ), polled here so the F9-F12 hotkeys work from anywhere.
  eternalsonata_hooks::ScanPollKeys(base);
  eternalsonata_hooks::ScanTick(base);

  // Pace after the present, so the wait covers the whole frame. Passing 0 while
  // fast-forwarding skips the wait entirely; LimitFrame also drops its stale
  // deadline and flags the frame unmeasured, so unpaced frames can't be read as
  // headroom and talk the ladder into stepping up.
  LimitFrame(TurboHeld() ? 0 : fps);
  ReportFramePacing(base, fps);
}
