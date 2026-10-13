// eternalsonata - ReXGlue Recompiled Project
//
// Keeps guest threads off the little cores of a big.LITTLE phone. The render
// task waits on one worker thread per scene object every frame, and a worker
// the scheduler parks on an in-order little core stretches that wait.
#pragma once

namespace eternalsonata {

// Call once per guest frame from the render thread; rescans rarely, since
// threads are created as areas load. No-op off Android.
void PinGuestThreadsToBigCores();

// For a host thread that works alongside the render thread. No-op off Android.
void PinThisThreadToBigCores();

}  // namespace eternalsonata
