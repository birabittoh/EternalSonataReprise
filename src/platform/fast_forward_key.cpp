#include "fast_forward_key.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_STRING(bind_fast_forward, "Shift", "Keybinds",
                      "Fast forward while held (keyboard key name, or empty for none)");

namespace eternalsonata {
namespace {

std::atomic<bool> g_held{false};

}  // namespace

// RegisterBind only fires on key down, so a hold needs the raw down/up events.
class FastForwardKeyImpl final : public FastForwardKey,
                                 public rex::ui::WindowInputListener,
                                 public rex::ui::WindowListener {
 public:
  explicit FastForwardKeyImpl(rex::ui::Window* window) : window_(window) {
    window_->AddInputListener(this, 0);
    window_->AddListener(this);
  }

  ~FastForwardKeyImpl() override {
    window_->RemoveInputListener(this);
    g_held = false;
  }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (Matches(e)) {
      g_held = true;
    }
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (Matches(e)) {
      g_held = false;
    }
  }

  // The key-up goes to whichever window has focus, so release on focus loss.
  void OnLostFocus(rex::ui::UISetupEvent&) override { g_held = false; }

 private:
  static bool Matches(rex::ui::KeyEvent& e) {
    const auto key = rex::ui::ParseVirtualKey(REXCVAR_GET(bind_fast_forward));
    return key != rex::ui::VirtualKey::kNone && e.virtual_key() == key;
  }

  rex::ui::Window* window_;
};

std::unique_ptr<FastForwardKey> CreateFastForwardKey(rex::ui::Window* window) {
  return window ? std::make_unique<FastForwardKeyImpl>(window) : nullptr;
}

bool FastForwardKeyHeld() { return g_held.load(std::memory_order_relaxed); }

}  // namespace eternalsonata
