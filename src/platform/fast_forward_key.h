#pragma once

#include <memory>

namespace rex::ui {
class Window;
}

namespace eternalsonata {

// Owns the window listener; the definition lives in the .cpp.
class FastForwardKey {
 public:
  virtual ~FastForwardKey() = default;
};

// Tracks whether the fast forward key (bind_fast_forward) is down. Key events
// come from the SDK window, so it works on every host.
std::unique_ptr<FastForwardKey> CreateFastForwardKey(rex::ui::Window* window);

// Read by TurboHeld every frame, from the guest thread.
bool FastForwardKeyHeld();

}  // namespace eternalsonata
