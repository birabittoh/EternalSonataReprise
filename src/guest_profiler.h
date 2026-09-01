// eternalsonata - ReXGlue Recompiled Project
//
// Where the *guest's* frame time goes.
//
// src/native_renderer_profile.h answers the same question for the native
// renderer's own host code. This file answers it for the recompiled guest, and
// it is deliberately backend agnostic: nothing here touches the GPU, a command
// buffer or a swap chain, so the numbers it prints mean the same thing under
// the Xenos backend and under the native renderer. Running the same scene on
// both and diffing these numbers separates a guest cost from a backend cost.
//
// Two independent instruments:
//
//  1. A sampling profiler over the guest's present thread. A host thread
//     suspends it at `guest_profile_hz`, unwinds it with RtlVirtualUnwind, and
//     buckets the frames. Because the recompiler emits one real host function
//     per guest function (`__imp__sub_82xxxxxx`), symbolising a host stack
//     yields guest addresses directly, with no guest-side cooperation and no
//     per-call instrumentation. The `incl` table names the guest function that
//     *owns* the frame; the `leaf` table names where the cycles land.
//
//  2. Exact counters on every blocking primitive the guest can reach. There
//     are only eight wait sites in the whole image (verified in IDA against
//     the KeWaitForSingleObject / KeWaitForMultipleObjects /
//     KeDelayExecutionThread / NtWaitForSingleObjectEx imports), so hooking
//     all of them is cheap and complete. This is what separates "the frame is
//     computing" from "the frame is blocked", which the sampler alone cannot
//     do reliably: a thread parked in a kernel wait still samples somewhere.
//
// Read the two together. If work time is high and the wait counters are near
// zero, the guest is out of CPU budget and `incl` names the culprit. If the
// wait counters account for the overrun, `incl` names who is stalling.
//
// The sampler attributes by stack, so it over-charges inlined host callees;
// treat its percentages as a ranking and the exact zone counters in
// native_renderer_profile.h as the measurement.
//
// The overlay below is the primary interface; `guest_profile` logs the same
// numbers once a second for a run you want to diff afterwards.

#ifndef ETERNALSONATA_GUEST_PROFILER_H
#define ETERNALSONATA_GUEST_PROFILER_H

#include <cstdint>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace eternalsonata {

// Called once per present, on the guest's present thread. Adopts that thread as
// the sampling target on the first call and starts the sampler when profiling
// is on.
void GuestProfilerFrameBoundary();

// Rolls the one-second window: refreshes the overlay's snapshot and, if
// `guest_profile` is set, logs it. Called from the present hook next to the
// other summaries.
void GuestProfilerReport();

// How long the guest's own present call took, in nanoseconds, so the summary
// can say how much of the frame's work time is the present rather than
// everything the guest did before reaching it.
void GuestProfilerNotePresent(uint64_t present_ns);

// The F3 panel. Always constructed; it draws only while toggled on, and it
// turns sampling on for exactly as long as it is open, so reading the numbers
// never costs anything when nobody is looking.
std::unique_ptr<rex::ui::ImGuiDialog> CreateGuestProfilerOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace eternalsonata

#endif  // ETERNALSONATA_GUEST_PROFILER_H
