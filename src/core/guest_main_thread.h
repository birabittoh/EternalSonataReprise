// eternalsonata - Run work on the guest *main* thread.
//
// rex::system::ModRegistry::RegisterTick looks like the natural place to make
// guest calls from a debug tool, and it does have a bound ThreadState, but it
// runs on the command-processor thread (see mod_registry.h: "Runs on the
// command-processor thread, not the render/UI thread") -- i.e. re-entrantly,
// from inside the CP's swap handler. Guest code that touches the model /
// resource manager (dword_824CF500) or allocates is not safe there: calling
// sub_820FCF80 from the tick crashed the game outright.
//
// Everything queued here is instead drained from the sub_8210AAD8 hook in
// eternalsonata_framerate.cpp -- the render pump's present, which runs once
// per frame on the same guest thread the game makes these calls from itself.
#pragma once

#include <functional>

namespace eternalsonata {

// Queues work to run once, on the next guest main-thread frame. Safe to call
// from any thread, including the ImGui draw thread.
void PostToGuestMainThread(std::function<void()> work);

// Drains the queue. Only ever called from the per-frame guest main-thread
// hook; callbacks may make guest calls.
void DrainGuestMainThread();

// True while the calling thread is inside DrainGuestMainThread, i.e. while it
// is safe to make guest calls right here instead of queueing them. Lets an API
// that is normally called from the ImGui thread (see party_system.cpp) run
// inline, and return a real result, when it happens to be called from a guest
// hook instead.
bool OnGuestMainThread();

}  // namespace eternalsonata
