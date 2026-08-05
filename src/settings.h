// eternalsonata - ReXGlue Recompiled Project
//
// Game-curated settings: the game's own defaults for SDK cvars, and a small
// player-facing settings overlay (Fullscreen/Resolution, plus a collapsed
// Advanced section) that replaces the SDK's developer settings panel on F4
// when `settings_manager_enabled = true`. See rex::cvar::SetDefaultValue,
// rex::cvar::SaveConfigSubset, and rex::ui::DrawCvarWidget in the SDK for the
// generic mechanism this builds on.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace rex::input {
class InputSystem;
}  // namespace rex::input

namespace eternalsonata {

// Overrides the SDK's built-in cvar defaults with the game's own. Call once,
// before rex::ReXApp::SetupEnvironment() (i.e. before any config file is
// loaded), so a saved config or CLI/env override still takes precedence.
void ApplySettingDefaults();

// Enumerates GPU plugins and (if built with Vulkan) Vulkan physical devices
// once, caching the results for CreateSettingsDialog. Both enumerations load
// GPU plugin DLLs / query the driver, so this is meant to run once at
// startup (e.g. from OnPostSetup) rather than every time the settings
// overlay is opened.
void InitSettingsCaches();

// Persists the "basic" settings (Fullscreen, Resolution, ...) to the same file
// the overlay writes, using the path captured by CreateSettingsDialog. Exists
// so settings changed outside the overlay - notably the native Fullscreen row
// added to the game's own Options screen in eternalsonata_hooks.cpp - survive a
// restart. No-op if CreateSettingsDialog has not run yet.
void SaveUserSettings();

// Binds the window and settings file that SetFullscreenSetting/SaveUserSettings
// act on. Call this at startup (OnPostSetup). The F4 overlay is created lazily,
// so capturing these in CreateSettingsDialog alone leaves the native Options
// row unable to apply or persist anything until the overlay has been opened
// once - which is exactly how it behaved before this existed.
void BindSettingsTargets(rex::ui::Window* window,
                         std::filesystem::path user_settings_path);

// Applies the fullscreen setting end to end: updates the cvar, applies it to
// the window (the cvar alone is inert - nothing in FlagEntry watches it), and
// persists. Used by the native Fullscreen row in the game's Options screen.
void SetFullscreenSetting(bool enabled);

// Applies the frame-rate cap end to end: updates the frame_rate cvar
// ("30"/"60"/"unlocked") and persists. Used by the native Frame Rate row in
// the game's Options screen; the value itself is applied by the host limiter
// in eternalsonata_hooks.cpp, which reads the cvar directly.
void SetFrameRateSetting(const char* value);

// Applies the adaptive-frame-rate toggle end to end: updates the
// adaptive_framerate cvar and persists. Used by the native Adaptive Frame
// Rate row in the game's Options screen.
void SetAdaptiveFramerateSetting(bool enabled);

// Applies a named resolution end to end: updates the resolution cvar and the
// paired resolution_scale cvar (see ResolutionScaleFor), and persists. `value`
// is one of "720p"/"1080p"/"1440p"/"4K". Used by the native Resolution row in
// the game's Options screen.
void SetResolutionSetting(const char* value);

// Creates the curated settings overlay. `user_settings_path` is where the
// friendly settings (Fullscreen, Resolution) are persisted;
// `app_config_path` is where everything else (the Advanced section) is
// persisted, matching the SDK's normal cvar config file. `window` is used
// by the "Restart Now" button on the pending-restart banner: it relaunches
// the process (rex::platform::process::Relaunch) then requests `window`
// close so the new instance picks up the just-changed cvars. `input_system`
// is forwarded to the SDK's own rex::ui::SettingsDialog, opened on demand
// via the "Advanced (Developer) Settings" button, so its gamepad rebind
// capture works the same as it does from F4; may be null.
std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system = nullptr);

}  // namespace eternalsonata
