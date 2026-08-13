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

// How many entries of the ascending {"720p", "1080p", "1440p", "4K"} preset
// list fit on the user's actual display (always >= 1). Both this file's
// Resolution row and the native Options screen's Resolution row
// (eternalsonata_options.cpp) use this so a resolution wider than the
// display never shows up as selectable in either place.
int AllowedResolutionCount();

// Enumerates GPU plugins and (if built with Vulkan) Vulkan physical devices
// once, caching the results for CreateSettingsDialog. Both enumerations load
// GPU plugin DLLs / query the driver, so this is meant to run once at
// startup (e.g. from OnPostSetup) rather than every time the settings
// overlay is opened.
void InitSettingsCaches();

// Persists the "basic" settings (Fullscreen, Resolution, ...) to the same file
// the overlay writes, using the path captured by CreateSettingsDialog. Exists
// so settings changed outside the overlay - notably the native Resolution and
// Frame Rate rows added to the game's own Options screen in
// eternalsonata_options.cpp - survive a restart. No-op if CreateSettingsDialog
// has not run yet.
void SaveUserSettings();

// Binds the window and settings file that SaveUserSettings and the resolution
// path act on. Call this at startup (OnPostSetup). The F4 overlay is created
// lazily, so capturing these in CreateSettingsDialog alone leaves the native
// Options rows unable to apply or persist anything until the overlay has been
// opened once - which is exactly how it behaved before this existed.
void BindSettingsTargets(rex::ui::Window* window,
                         std::filesystem::path user_settings_path);

// Applies the frame-rate cap end to end: updates the frame_rate cvar
// ("30"/"60"/"adaptive"/"unlocked") and persists. The value itself is applied
// by the host limiter in eternalsonata_framerate.cpp, which reads the cvar
// directly. Prefer SetFrameRateOption below for anything menu-shaped.
void SetFrameRateSetting(const char* value);

// The Frame Rate presets as an ordered list. Both the overlay's Frame Rate
// slider and the native Options screen's Frame Rate row draw this same list so
// they cannot drift. FrameRateOptionLabel is the text shown ("30 FPS",
// "Adaptive", ...), FrameRateOptionIndex the entry the cvar currently holds,
// and SetFrameRateOption writes and persists it.
int FrameRateOptionCount();
const char* FrameRateOptionLabel(int index);
int FrameRateOptionIndex();
void SetFrameRateOption(int index);

// The user_language cvar as an ordered list, for the native Text row in the
// game's Options screen. Index 0..UserLanguageCount()-1; UserLanguageCode
// returns the two-letter form the row draws ("EN", "DE", ...), and
// UserLanguageIndex the entry the cvar currently holds (0 if unrecognised).
// SetUserLanguageSetting writes and persists it; the guest only reads its
// language at boot, so the change needs a restart to show, and it is recorded
// as a pending restart accordingly.
int UserLanguageCount();
const char* UserLanguageCode(int index);
int UserLanguageIndex();
void SetUserLanguageSetting(int index);

// The user_language entry the process *started* with, latched once (at
// InitSettingsCaches time) and stable for the rest of the run. This is the
// language every label we draw into the game's own screens has to use: changing
// user_language only takes effect on the next launch, so following the live
// cvar would leave our labels speaking a language the rest of the screen does
// not. Same index space as UserLanguageIndex.
int BootUserLanguageIndex();

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
