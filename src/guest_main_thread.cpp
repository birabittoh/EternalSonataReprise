#include "guest_main_thread.h"

#include <mutex>
#include <utility>
#include <vector>

namespace eternalsonata {

namespace {

std::mutex g_mutex;
std::vector<std::function<void()>> g_pending;

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
  for (auto& fn : work) {
    fn();
  }
}

}  // namespace eternalsonata
