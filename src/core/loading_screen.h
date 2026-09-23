#pragma once

// A full window progress screen for work that blocks the UI thread after the
// native renderer is up but before the guest presents anything. Drawn through
// the ImGui overlay by the native renderer, so it shows wherever the game
// itself can draw, Android included.

#include <string>

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}

namespace eternalsonata {

// Name of the atlas font baked for this screen; see OnConfigureFonts.
inline constexpr char kLoadingScreenFontName[] = "Loading screen";

// The context is what lets the window resize and close while the UI thread is
// busy with the step being reported.
void BindLoadingScreen(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context);

// Shows the screen on first use. Cheap to call per work item: frames are
// throttled, and every call keeps the window responsive. `fraction` outside
// [0,1] draws no fill. `force` presents even inside the throttle window, for
// the final state.
void UpdateLoadingScreen(const std::string& title, float fraction, const std::string& detail,
                         bool force = false);

// Drops the screen. The last frame stays on screen until the game presents.
void HideLoadingScreen();

}  // namespace eternalsonata
