#pragma once

#include <memory>

namespace rex::input {
class InputSystem;
}
namespace rex::ui {
class Window;
}

namespace eternalsonata {

enum class ActiveInputKind {
  kUnknown,
  kKeyboardMouse,
  kXbox,
  kPlayStation,
  kNintendo,
  kOtherPad,
};

// Owns the window listener; the definition lives in the .cpp.
class ActiveInputTracker {
 public:
  virtual ~ActiveInputTracker() = default;
};

// Keyboard and mouse come from window events, pads are polled from ActiveInput.
std::unique_ptr<ActiveInputTracker> CreateActiveInputTracker(rex::ui::Window* window,
                                                             rex::input::InputSystem* input);

// What the player used last. Polls the pads, so call it from one thread.
ActiveInputKind ActiveInput();

const char* ToString(ActiveInputKind kind);

}  // namespace eternalsonata
