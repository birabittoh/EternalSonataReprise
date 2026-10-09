#pragma once

// The start screen: asks for the game files, shows the install phases
// (install_pipeline.h) as they run, and enables Start once they are done.
// Drawn through the ImGui overlay. Shown only when no prepared game directory
// is found, or one still has phases to run. Driven by the app's main loop,
// which is what feeds it input.

#include <functional>
#include <string>

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

// Around the install phases, which block the UI thread: shows the phase row
// and progress instead of the prompt. No effect without a screen.
void BeginIntroWork();
void EndIntroWork();
void SetIntroPhase(Phase phase, PhaseState state);
// Draws a frame itself, since the main loop is busy; throttled.
void ReportIntroProgress(const std::string& title, float fraction, const std::string& detail);

void HideIntroScreen();

// For OnWindowCloseRequested: answers a showing intro with kQuit. Returns
// whether one was showing.
bool RequestIntroQuit();

// Releases the art once game data is resolved.
void ReleaseIntroScreen();

}  // namespace eternalsonata
