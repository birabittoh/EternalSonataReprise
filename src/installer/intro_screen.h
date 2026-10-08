#pragma once

// The first run screen asking for the game files, drawn through the ImGui
// overlay like the loading screen. Shown only when no prepared game directory
// is found. Driven by the app's main loop, which is what feeds it input.

#include <functional>

#include "game_data.h"

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}

namespace eternalsonata {

void BindIntroScreen(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context);

// Whether the intro can draw: bound, and the native renderer is up.
bool IntroScreenAvailable();

// Shows the screen, or updates it when already showing, and returns at once.
// `on_choice` runs on the UI thread, outside any frame.
void ShowIntroScreen(const GameDataPrompt& prompt, std::function<void(GameDataChoice)> on_choice);

// Ignores the buttons while a native dialog is open over the screen.
void SetIntroBusy(bool busy);

void HideIntroScreen();

// For OnWindowCloseRequested: answers a showing intro with kQuit. Returns
// whether one was showing.
bool RequestIntroQuit();

// Releases the art once game data is resolved.
void ReleaseIntroScreen();

}  // namespace eternalsonata
