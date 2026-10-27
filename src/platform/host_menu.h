// eternalsonata - Host menu: the one entry point a touch-only device has into
// every SDK overlay (F1-F4/F7/F2), opened by the Android back button (see
// window_sdl.cpp's SDL_HINT_ANDROID_TRAP_BACK_BUTTON) or a gamepad Back/View
// button fallback. Backed by a real Android AlertDialog (see host_menu.cpp
// and EternalSonataActivity.showHostMenu), not an ImGui overlay: it needs to
// draw as its own window above the touch controls and the game surface, both
// of which ImGui overlays share a render target with. Android only; desktop
// already reaches every overlay by its own F-key.
//
// It also carries the actions that have no overlay behind them and that an
// Android user cannot perform for themselves, because Android 11+ hides
// Android/data from on-device file managers: installing mods and picking a
// GPU driver. Saves are imported and exported on the start screen.
#pragma once

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace eternalsonata {

class HostMenu {
 public:
  explicit HostMenu(rex::ui::Window* window);
  ~HostMenu();

  HostMenu(const HostMenu&) = delete;
  HostMenu& operator=(const HostMenu&) = delete;

  // Used by the JNI callbacks in host_menu.cpp to reach the window that owns
  // the SDL/UI thread the callback has to marshal onto.
  rex::ui::Window* window_for_callback() const { return window_; }

 private:
  rex::ui::Window* window_;
};

}  // namespace eternalsonata
