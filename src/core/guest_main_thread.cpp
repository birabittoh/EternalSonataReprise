#include "guest_main_thread.h"

#include <mutex>
#include <utility>
#include <vector>

namespace eternalsonata {

namespace {

std::mutex g_mutex;
std::vector<std::function<void()>> g_pending;

// Set for the duration of a drain, on the draining thread only. Thread-local
// rather than a plain flag: the question it answers is "is *this* thread the
// guest main thread", and any number of other threads may ask it concurrently.
thread_local bool g_draining = false;

}  // namespace

void PostToGuestMainThread(std::function<void()> work) {
  if (!work) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pending.push_back(std::move(work));
}

void DrainGuestMainThread() {
  std::vector<std::function<void()>> work;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.empty()) {
      return;
    }
    work.swap(g_pending);
  }
  // Run outside the lock: a callback is free to post follow-up work, which
  // then runs on the next frame rather than deadlocking here.
  g_draining = true;
  for (auto& fn : work) {
    fn();
  }
  g_draining = false;
}

bool OnGuestMainThread() { return g_draining; }

}  // namespace eternalsonata
