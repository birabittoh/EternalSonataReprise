#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <imgui.h>
#include <rex/cvar.h>

#include "enemy_system.h"
#include "eternalsonata_hooks_internal.h"
#include "guest_main_thread.h"
#include "guest_profiler.h"
#include "native_renderer_profile.h"

// frame_rate cvar: "30" / "60" / "adaptive" / "unlocked". Defined (and
// persisted) in settings.cpp; declared here so the frame-driver hook can read it
// cheaply.
REXCVAR_DECLARE(std::string, frame_rate);
REXCVAR_DECLARE(bool, frame_debug);

// Bisection switches for the wall-clock mode, each bit disables one of its
// parts: 1 exact float delta, 2 per-frame byte (stays 60, so the game runs
// fast), 4 animation fixups, 8 physics and script timer frame time, 16
// script wait, 32 the NMTN key grid sampling.
REXCVAR_DEFINE_INT32(frame_wall_debug, 0, "Eternal Sonata",
                     "Bitmask disabling parts of the unlocked wall-clock stepping (debug)");

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
// cvar changes drastically: GraphicsSystem::SetupPresentation runs the pump at
// the video mode's refresh rate (60 Hz) when `vsync` is on and at 1000 Hz when
// it is off. We therefore never hand the guest a real interval while the pump
// is at refresh rate; see ApplyFrameRate. Pacing is the host limiter's job,
// which keeps the frame rate independent of `vsync`.
namespace {

REX_IMPORT(__imp__sub_8225A9F0, g_sub_8225A9F0, void(u32, u32));

// Maps the frame_rate cvar onto a target fps. 0 means "wall clock": no
// limiter, and the sim is stepped by measured frame time instead (see
// WallClockTick).
//
// Game speed is (actual fps / declared fps): the sim advances a fixed
// `300 / byte_82465F90` clock units per presented frame, so a paced rate must
// be one the host actually holds, and its step must be an integer (120 gives
// 2.5 -> 2 and was measured ~20% slow). The host limiter below is what makes
// declared == actual for the fixed rates.
//
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
  // "adaptive" targets 60 like "60" does; the two differ only in whether the
  // ladder below may step the declared rate down (see AdaptiveFrameRate).
  if (mode == "60" || mode == "adaptive")
    return 60;
  // Screens the stock game paces at 60 have per-present logic written for 60
  // presents a second, so "unlocked" leaves them at 60 and only frees gameplay.
  if (mode == "unlocked" || mode == "0")
    return stock == 60 ? 60 : 0;
  // The exact rates above 60: the byte stays constant and 300 / byte is an
  // integer, so every consumer stays bit exact at the cost of the display
  // having to be paced to one of them.
  if (mode == "75" || mode == "100" || mode == "150")
    return stock == 60 ? 60 : static_cast<u8>(std::stoi(mode));
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

// Fallback ladder. 30 is the floor: it's the game's own stock gameplay rate.
// 60 is the only adaptive target; rates above it are the wall-clock mode's job.
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
  // The ladder belongs to the "adaptive" setting alone. "60" is the same target
  // pinned, and "30"/"unlocked" have nothing to step down to.
  if (REXCVAR_GET(frame_rate) != "adaptive") {
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

// True when the SDK's vblank pump is running at the display refresh rate rather
// than free running at 1000 Hz; see the interval choice in ApplyFrameRate.
// Neither renderer registers `vsync` in the executable (the Xenos plugin owns
// the name on one path, RegisterNativeRendererCvars on the other), so an
// unregistered read is empty and reads as "free running" — which is also the
// state a build with no owner is in.
bool PumpIsVsynced() { return rex::cvar::GetFlagByName("vsync") == "true"; }

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
    // A real presentation interval is NOT inert: sub_8225E818 reads
    // device[13444], maps it to 0/1/2/3, and packs it into the swap scheduler
    // argument, where sub_8225E680 (vblank ISR, spinlocked on the kernel device
    // object) adds it to the flip schedule:
    //
    //   v17 = last_flip_target + interval;
    //   if (v17 <= vblank_count) { v17 = vblank_count; ... }
    //   if (v17 == vblank_count) flip now; else queue in the pending-flip ring;
    //
    // With IMMEDIATE the interval term is 0, so every flip takes the "flip now"
    // branch and the pending-flip ring goes unused — a different front-buffer
    // publication pattern than stock, which matters because the title runs two
    // front buffers (dword_82466100 / dword_82466104). That is the only reason
    // to want a real interval here, and it is worth having *only* while the
    // vblank pump is free running.
    //
    // With `vsync` on the pump ticks at 60 Hz, and then the schedule above
    // quantises: a frame whose work overruns a vblank cannot flip until the
    // next one, so the achieved rate snaps to 60/30/20 (interval 1) or
    // 30/20/15 (interval 2) with nothing in between. This engine is
    // frame-clocked — the sim advances a fixed 300/declared units per present —
    // so game speed is actual fps / declared fps, and a quantised present rate
    // is not choppiness but literal slow motion. It showed up as the game
    // dropping to half speed during battle attacks, i.e. exactly where a frame
    // first overruns 16.7 ms, and it did so on both renderers because the
    // quantisation lives in the guest's own flip scheduler rather than in
    // either backend.
    //
    // With `vsync` off the pump ticks at 1000 Hz, the interval wait is
    // satisfied immediately, and the stock flip scheduling costs nothing.
    const u32 interval =
        (!PumpIsVsynced() && fps <= 60 && 60u % fps == 0) ? (60u / fps - 1u) : 3u;
    g_sub_8225A9F0(frame, base, device, interval <= 2 ? (1u << interval) : 0x80000000u);
    REX_STORE_U8(0x82465F90, fps);
  } else {
    // Wall clock: the present hook rewrites the byte every frame (see
    // WallClockTick); the stock 60 only covers the frames until it does.
    g_sub_8225A9F0(frame, base, device, 0x80000000u);
    REX_STORE_U8(0x82465F90, static_cast<u8>(REX_LOAD_U32(0x8243D374)));
  }
}

// Last fps value we pushed into the device, so the per-frame re-apply below
// only touches D3D state when the cvar actually changed.
u8 g_applied_fps = 0xFF;

// The `vsync` reading that chose the interval currently in the device. The
// setting is hot-reloadable, and the interval ApplyFrameRate picks depends on
// it, so toggling vsync has to re-apply even though the fps did not change.
// 0xFF is "nothing applied yet", so the first frame always writes.
u8 g_applied_vsync = 0xFF;

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
// fast in exact proportion to the extra frames, for as long as the key is
// down. Nothing about the declared rate changes, so releasing the key returns
// to normal speed with no state to unwind. (In wall-clock mode the tick is
// suspended for the frame, which is the same thing.)
//
// Deliberately momentary rather than a toggle: a stuck fast-forward in a
// cutscene is unrecoverable without noticing what happened.
#ifdef _WIN32
constexpr int kTurboKey = VK_TAB;
#endif

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
void LimitFrame(double fps) {
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

  if (fps <= 0.0) {
    next_deadline = {};  // Fast-forward: drop any stale deadline.
    g_frame_measured = false;
    return;
  }

  const auto period =
      std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));

  // First frame, a rate change, or a long stall (loading screen, alt-tab):
  // restart the cadence instead of trying to catch up on missed frames. The
  // frame that spans a stall says nothing about sustainable performance, so
  // don't let it feed the ladder.
  if (next_deadline == clock::time_point{} || now > next_deadline + period * 4) {
    next_deadline = now + period;
    g_frame_measured = false;
    return;
  }

  {
    // Deliberate idle time, not CPU work, even though it runs on this thread;
    // see the profiler's cpu_ms/pacer_ms split for why it has its own zone.
    eternalsonata::ProfileZone pacer_zone(eternalsonata::kPhasePacerWait);
    constexpr auto kSpinMargin = std::chrono::microseconds(1500);
    const auto slack = next_deadline - now;
    if (slack > kSpinMargin) {
      PreciseSleep(slack - kSpinMargin);
    }
    while (clock::now() < next_deadline) {
      std::this_thread::yield();
    }
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

// ---------------------------------------------------------------------------
// Wall-clock stepping ("unlocked")
//
// The engine has no elapsed-time path: sub_8210AAD8 measures the frame's wall
// time into obj+264 and nothing reads it. Every consumer derives its step from
// byte_82465F90, in one of two ways:
//
//   - the integer `300 / byte`, inlined at 124 sites (IDA xrefs), a few of
//     which snapshot it at setup time (sub_820EA758 converts a script wait to
//     a frame count, sub_8212D350 stamps a battle signal with it);
//   - sub_82181728, the float delta getter behind ~100 more callers, which
//     returns `(float)(300 / byte) * scale[category] * global`.
//
// So above 60 fps there is no single byte that keeps game speed exact (the
// step has to be an integer, and 300 / byte is at most 5 for anything faster
// than 60), and an earlier 150 preset ran the frame-counted parts of the sim
// at 5x. Instead the byte is chosen per frame: the measured frame time is
// accumulated in 1/300 s units and each frame takes the integer step that
// keeps the running total on the wall clock, Bresenham style. Integer sites
// see steps that are exact on average and off by at most one unit at any
// moment; the float getter is corrected to the exact measured delta
// (FrameDeltaScale), which is what character animation runs on: its clock is
// a float accumulated from that getter and sampled by
// `frame = anim_fps * t / 300 + 1` (sub_820C1F20), with Hermite keys. The
// two truncations that do live in the animation update are patched in
// AnimEntryFixup and AnimPartFixup, the script wait is recomputed from the
// smoothed frame time so cutscene waits don't inherit the quantisation, and
// the two callers that read the byte as a frame rate rather than a step (the
// bone physics, a script timer) get the measured frame time instead.
//
// Only the present thread writes the byte and the g_wall_* values; the
// character updates run on three worker threads that the render task forks
// and joins before present, so they see one consistent frame. Nothing may
// write the byte from a worker (a save/restore there interleaves and leaves
// it corrupted for the main thread).
//
// Off-grid sampling: the stock clock only ever lands on NMTN key frames; see
// the sub_8211CCC8 hook for why any other phase snaps the body.
//
// Steps run 1..10: 10 is the stock 30 fps step, so frames slower than that
// go into slow motion exactly as the stock game does, and 1 is the byte's
// ceiling (300 fps), which the limiter enforces so faster frames can't speed
// the game up.
// ---------------------------------------------------------------------------

constexpr double kUnitsPerSecond = 300.0;
constexpr double kWallCeilingFps = 300.0;
constexpr int kWallMinStep = 1;
constexpr int kWallMaxStep = 10;

bool g_wall_active = false;
std::chrono::steady_clock::time_point g_wall_last{};
double g_wall_acc = 0.0;        // units owed to the integer clock
double g_wall_units = 5.0;      // this frame's exact delta, in units
double g_wall_avg_units = 5.0;  // smoothed, for setup-time conversions
int g_wall_step = 5;            // the integer step the byte currently encodes
u8 g_wall_rate = 60;            // smoothed fps with hysteresis

// The byte whose `300 / byte` is `step`. 300 itself doesn't fit, so step 1
// uses 255 (300 / 255 == 1); every other value is exact.
u8 ByteForStep(int step) { return step == 1 ? 255 : static_cast<u8>(300 / step); }

void WallClockTick(u8* base) {
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  double units = 5.0;
  if (g_wall_active) {
    units = std::chrono::duration<double>(now - g_wall_last).count() * kUnitsPerSecond;
  } else {
    g_wall_acc = 0.0;
    g_wall_avg_units = 5.0;
    g_wall_rate = 60;
    g_wall_active = true;
    REXLOG_INFO("[fps-cap] wall-clock stepping on");
  }
  g_wall_last = now;
  units = std::clamp(units, double(kWallMinStep), double(kWallMaxStep));

  // The frame about to run is stepped by the previous frame's duration; frame
  // times are steady enough that the one-frame lag is invisible, and the
  // accumulator makes the integer clock track the wall clock regardless.
  g_wall_units = units;
  g_wall_avg_units += (units - g_wall_avg_units) * 0.05;
  // Only move the stable rate when the average has clearly left it, so a
  // frame rate hovering around a boundary doesn't flap it every few frames.
  const int rate = std::clamp(int(std::lround(kUnitsPerSecond / g_wall_avg_units)), 30, 255);
  if (std::abs(rate - int(g_wall_rate)) >= std::max(3, int(g_wall_rate) / 25)) {
    g_wall_rate = static_cast<u8>(rate);
  }
  g_wall_acc += units;
  const int step = std::clamp(int(std::lround(g_wall_acc)), kWallMinStep, kWallMaxStep);
  g_wall_acc = std::clamp(g_wall_acc - step, -double(kWallMaxStep), double(kWallMaxStep));
  g_wall_step = step;
  if (REXCVAR_GET(frame_wall_debug) & 2) {
    g_wall_step = 5;
    REX_STORE_U8(0x82465F90, 60);
    return;
  }
  REX_STORE_U8(0x82465F90, ByteForStep(step));
}

void WallClockStop() {
  g_wall_active = false;
  REXLOG_INFO("[fps-cap] wall-clock stepping off");
}

}  // namespace

namespace eternalsonata_hooks {

bool WallClockActive() { return g_wall_active; }

// Multiplier that turns sub_82181728's integer-derived delta into the exact
// measured one. 1 outside wall-clock mode, so the fixed rates stay bit exact.
double FrameDeltaScale() {
  if (!g_wall_active || (REXCVAR_GET(frame_wall_debug) & 1)) {
    return 1.0;
  }
  return g_wall_units / double(g_wall_step);
}

// sub_820C8378(model, entry, f1, f2, f3) gets f1 = (dt / step) / (byte /
// anim_fps), both divisions integer, i.e. the animation frames this game frame
// is worth. With byte = 150 that is exact, with 100 it is 11% high and with 50
// it divides by zero. The exact value is dt * anim_fps / 300.
void AnimEntryFixup(PPCContext& ctx, u8* base) {
  if (!g_wall_active || (REXCVAR_GET(frame_wall_debug) & 4)) {
    return;
  }
  const auto anim_fps = static_cast<int32_t>(REX_LOAD_U32(ctx.r3.u32 + 31116));
  if (anim_fps > 0) {
    ctx.f1.f64 = ctx.f2.f64 * anim_fps / kUnitsPerSecond;
  }
}

// sub_820C9550 scrolls flagged parts' UVs by `rate * model[31168] / step` per
// frame, where sub_820C7538 stores this frame's dt into model[31168] just
// before the loop, so the scroll is a per-frame constant that scales with
// fps. Rewrite the field so the per-second rate matches the paced 60 (step 5).
void AnimPartFixup(PPCContext& ctx, u8* base) {
  if (!g_wall_active || (REXCVAR_GET(frame_wall_debug) & 4)) {
    return;
  }
  const float scaled = static_cast<float>(ctx.f1.f64 * g_wall_step / 5.0);
  u32 bits;
  std::memcpy(&bits, &scaled, sizeof bits);
  REX_STORE_U32(ctx.r3.u32 + 31168, bits);
}

}  // namespace eternalsonata_hooks

namespace {

// Script native 1059 (sub_820EA758): schedules a wait of `duration` units as
// ceil(duration / step) frames on the object's task, with the step current at
// call time. In wall-clock mode that step is one frame's quantised value, so
// two identical waits could differ by a whole unit (a third, at 144 fps).
// Convert through the smoothed frame time instead; the list walk is the
// original's.
REX_IMPORT(__imp__sub_8217B400, g_task_wait, u32(u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__sub_8217B4A0, g_task_wait_alt, u32(u32, u32, u32, u32, u32, u32));

}  // namespace

namespace {

// Physics re-init that follows a byte change (sub_8213E420): dt = scale / fps
// or a fixed scale / 60 per iteration, `60 / fps` iterations per frame. Above
// 60 fps that is one 1/60 step per frame, so hair and cloth would run fast in
// proportion; give each chain the measured frame time instead. The re-init is
// forced every frame below so this holds when the step is stable too.
REX_EXTERN(__imp__sub_8213FAA8);
REX_HOOK_RAW(sub_8213FAA8) {
  const u32 obj = ctx.r3.u32;
  const u32 chain = obj + 4560 * ctx.r4.u32;
  __imp__sub_8213FAA8(ctx, base);
  if (!g_wall_active || (REXCVAR_GET(frame_wall_debug) & 8)) {
    return;
  }
  u32 bits = REX_LOAD_U32(obj + 584008);
  float scale;
  std::memcpy(&scale, &bits, 4);
  const float dt = static_cast<float>(scale * g_wall_units / kUnitsPerSecond);
  std::memcpy(&bits, &dt, 4);
  REX_STORE_U32(chain + 312, bits);
  REX_STORE_U32(chain + 336, bits);
}

REX_EXTERN(__imp__sub_8213E420);
REX_HOOK_RAW(sub_8213E420) {
  if (g_wall_active && !(REXCVAR_GET(frame_wall_debug) & 8)) {
    REX_STORE_U32(ctx.r3.u32 + 584004, 0);  // differs from any byte: re-init
  }
  __imp__sub_8213E420(ctx, base);
}

// Script timer native: slot[16] = (int)(seconds * byte). Recompute from the
// smoothed frame time.
REX_EXTERN(__imp__sub_820F7910);
REX_HOOK_RAW(sub_820F7910) {
  const u32 args = ctx.r3.u32;
  __imp__sub_820F7910(ctx, base);
  if (!g_wall_active || (REXCVAR_GET(frame_wall_debug) & 8)) {
    return;
  }
  const u32 slot = 0x82440598u + 20 * REX_LOAD_U32(args);
  u32 bits = REX_LOAD_U32(args + 12);
  float seconds;
  std::memcpy(&seconds, &bits, 4);
  REX_STORE_U32(slot + 16, static_cast<u32>(int(seconds * kUnitsPerSecond / g_wall_avg_units)));
}

// NMTN channel sampler: keys are sparse (frame, value, tangent in, tangent
// out), Hermite between them. Rotation channels hold Euler angles and the
// exported data flips representation between adjacent frames now and then,
// the three components of a bone jumping together; interpolating across such
// a pair gives a different rotation altogether. The stock game never sees it
// because its clock only lands on key frames, where the Hermite reduces to
// the key value. Any other step samples between keys once per flip and the
// body snaps for a frame, and since a clock's off-grid phase survives a return
// to 60, so does the snap. Sample on the key grid like stock does: the keys
// carry no motion between frames anyway.
REX_EXTERN(__imp__sub_8211CCC8);
REX_HOOK_RAW(sub_8211CCC8) {
  if (!(REXCVAR_GET(frame_wall_debug) & 32)) {
    const u32 bits = REX_LOAD_U32(ctx.r3.u32 + 12);
    float unit;
    std::memcpy(&unit, &bits, 4);
    if (unit > 0.0f) {
      ctx.f1.f64 = std::round(ctx.f1.f64 / unit) * unit;
    }
  }
  __imp__sub_8211CCC8(ctx, base);
}

}  // namespace

REX_EXTERN(__imp__sub_820EA758);

REX_HOOK_RAW(sub_820EA758) {
  if (!g_wall_active || (REXCVAR_GET(frame_wall_debug) & 16)) {
    __imp__sub_820EA758(ctx, base);
    return;
  }
  const u32 args = ctx.r3.u32;
  const u16 id = static_cast<u16>(REX_LOAD_U32(args));
  const u32 duration = REX_LOAD_U32(args + 4);
  const u32 mode = REX_LOAD_U32(args + 8);
  u32 node = REX_LOAD_U32(0x82440590);
  while (node && REX_LOAD_U16(node + 8) != id) {
    node = REX_LOAD_U32(node);
  }
  if (node) {
    const u32 handle = REX_LOAD_U32(node + 4);
    const u32 frames = std::max(1u, static_cast<u32>(std::ceil(duration / g_wall_avg_units)));
    (mode == 1 ? g_task_wait_alt : g_task_wait)(0x824CF500u, handle, frames, 0, 0, 0xFFFFFFFFu);
  }
  ctx.r3.u64 = 0;
}

namespace {

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
      "work_max={:.2f}ms behind={} ahead={} up_block={} wall={} step={} units={:.2f} avg={:.2f} "
      "rate={}",
      presents / secs, REX_LOAD_U8(0x82465F90), fps, g_ladder_target, mean_ms,
      double(work_max.count()) / 1000.0, g_behind_score, g_ahead_score, g_up_block,
      g_wall_active, g_wall_step, g_wall_units, g_wall_avg_units, g_wall_rate);

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
  g_applied_vsync = PumpIsVsynced();
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
  const u8 vsynced = PumpIsVsynced();
  if (fps != g_applied_fps || vsynced != g_applied_vsync) {
    ApplyFrameRate(ctx, base, fps);
    g_applied_fps = fps;
    g_applied_vsync = vsynced;
  }
  // The guest's own present, timed so the profiler can separate it from
  // everything the guest did before reaching here. Both halves are guest CPU
  // work; the split only says which half to go looking in.
  const auto present_start = std::chrono::steady_clock::now();
  __imp__sub_8210AAD8(ctx, base);
  eternalsonata::GuestProfilerNotePresent(uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                           present_start)
          .count()));
  eternalsonata::GuestProfilerFrameBoundary();

  // Debug tools queue guest calls that are only safe on this thread; see
  // guest_main_thread.h for why the mod-registry tick will not do.
  eternalsonata::DrainGuestMainThread();
  // Reasserts the enemy rebalance overrides; see src/enemy_system.h for why it
  // has to happen every frame rather than once when a battle starts.
  eternalsonata::EnemySystemTick();
  // These live in eternalsonata_options.cpp (the value-highlight memory
  // differ), polled here so the F9-F12 hotkeys work from anywhere.
  eternalsonata_hooks::ScanPollKeys(base);
  eternalsonata_hooks::ScanTick(base);
  eternalsonata_hooks::OptionsTick();

  // Pace after the present, so the wait covers the whole frame. Passing 0 while
  // fast-forwarding skips the wait entirely; LimitFrame also drops its stale
  // deadline and flags the frame unmeasured, so unpaced frames can't be read as
  // headroom and talk the ladder into stepping up.
  const bool turbo = TurboHeld();
  const bool wall = fps == 0 && !turbo;
  LimitFrame(turbo ? 0.0 : (fps ? double(fps) : kWallCeilingFps));
  if (wall) {
    WallClockTick(base);
  } else if (g_wall_active) {
    // Leaving wall-clock mode (setting changed, or fast-forward): the byte
    // holds whatever step the last tick chose, so put the mode's rate back.
    WallClockStop();
    ApplyFrameRate(ctx, base, fps);
  }
  ReportFramePacing(base, fps);
  // Rolls the guest profiler's one-second window. Placed after the limiter so
  // its per-frame wait figures cover the whole frame, and so the summary's own
  // cost (symbolisation, once a second) lands outside the paced region rather
  // than eating into the next frame's budget.
  eternalsonata::GuestProfilerReport();
}
