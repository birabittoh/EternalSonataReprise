// eternalsonata - ReXGlue Recompiled Project
//
// The game's audio thread (sub_82143588) arms a periodic guest timer with
// NtSetTimerEx(period_ms = 5) and blocks on it forever, ticking the sound
// system once per fire. 5 ms means the sequencer is designed to run at 200 Hz,
// corroborated by the literal `* 200.0` tempo conversions in sub_821419E0 and
// sub_82147898.
//
// Windows rounds a waitable timer's period up to the current system timer
// resolution, which defaults to about 15.6 ms. Left alone, the sequencer ticks
// at roughly 64 Hz instead of 200 Hz and everything it drives runs 3.1x slow:
// music tempo, voice lines, and the story battle intro that waits on the
// sequencer to retire each line. Raising the host tick rate to 5 ms is what
// makes the guest's own period achievable.
//
// This lives here, in the game, rather than in the SDK on purpose. It is a
// property of this title's audio thread, not of the runtime, and keeping it
// project-side means the shipped SDK stays the stock pinned nightly.

#pragma once

namespace eternalsonata {

// Raises the process-wide host timer resolution to the host_timer_resolution_ms
// cvar, if that is finer than what the host already provides. Call at startup,
// after the config files are loaded and before the guest starts, and again
// whenever the cvar changes.
//
// Safe to call repeatedly: it reads the cvar afresh, no-ops when the applied
// period is already the requested one, and otherwise swaps the outstanding
// request for the new one without letting the tick rate dip in between. That
// is what lets the setting apply live rather than needing a restart; the guest
// audio thread's periodic timer is serviced by the system tick, so it picks up
// the new rate on its next expiry without being re-armed.
//
// No-op when the cvar is 0, and on non-Windows hosts (POSIX timers already run
// at nanosecond granularity).
void ApplyHostTimerResolution();

// Releases the raise requested by ApplyHostTimerResolution. Call once on
// shutdown. No-op if nothing was raised.
void ReleaseHostTimerResolution();

}  // namespace eternalsonata
