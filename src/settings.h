// eternalsonata - ReXGlue Recompiled Project
//
// Game-curated settings: the game's own defaults for SDK cvars, and a small
// player-facing settings overlay (Fullscreen/Resolution, plus a collapsed
// Advanced section) that replaces the SDK's developer settings panel on F4
// when `settings_manager_enabled = true`. See rex::cvar::SetDefaultValue,
// rex::cvar::SaveConfigSubset, and rex::ui::DrawCvarWidget in the SDK for the
// generic mechanism this builds on.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace rex::input {
class InputSystem;
}  // namespace rex::input

namespace rex::system {
class ModRegistry;
}  // namespace rex::system

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

// Whether a cvar changed this session still needs a relaunch to take effect,
// the same state the F4 overlay's "Some changes require a restart to take
// effect." banner reads. IsCvarPendingRestart asks about one cvar (used by the
// native Options rows to mark themselves), AnyCvarPendingRestart about every
// cvar this settings UI owns. Only cvars actually changed at runtime count: a
// saved preference loaded at boot is not pending anything.
bool IsCvarPendingRestart(const char* name);
bool AnyCvarPendingRestart();

// Relaunches the game and closes this instance, so the next launch picks up the
// restart-scoped cvars changed this session. Safe to call from the guest CPU
// thread (the window close is marshalled onto the UI thread). Returns false, and
// does nothing, if the relaunch could not be started or BindSettingsTargets has
// not run yet.
bool RestartNow();

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

// One entry in the Language list: the five the game shipped with, followed by
// anything mods added (see RegisterLanguageListeners).
//
//   `id`       stringified XLanguage value, as stored by the user_language cvar
//   `label`    display text for the F4 overlay's combo ("English")
//   `code`     two-letter form the native Options screen's Text row draws
//   `btx_slot` the BTX language block this language's in-game text lives in, as
//              a fourcc *with* its trailing space ("ESP "). The guest picks its
//              block by a boot-latched index, so a mod-added language does not
//              get a block of its own; it piggybacks on an existing one, and
//              this says which. See ApplyBootLanguageDonorSlot.
struct LanguageOption {
  const char* id;
  const char* label;
  const char* code;
  const char* btx_slot;
};

// How many entries the Language list can hold in total, built-ins included.
// The cap is the native Options screen's Text row, not memory: its values are
// drawn side by side sharing one row's width (see eternalsonata_options_api.h),
// and ETERNALSONATA_MAX_ROW_VALUES is where that stops being legible.
// Registrations past the cap are dropped with a warning.
int MaxLanguageOptions();

// Subscribes to the three mod-registry events a translation mod publishes.
// Must run after Runtime exists but before any mod's OnCreateDialogs, i.e. from
// OnPostLoadXexImage, which the SDK calls immediately before it loads mod
// plugins. All three are first-wins with a WARN on duplicates, and drop
// malformed payloads with a WARN.
//
//   "settings.language_option"  adds a Language entry.
//       payload.u64   = the XLanguage id
//       payload.bytes = ASCII "Label" or "Label|CODE|SLOT" (see
//                       RegisterModLanguage; the short form derives the code
//                       from the label and leaves the slot unclaimed)
//   "settings.language_slot"    claims the BTX block a language's text lives in.
//       payload.u64   = the XLanguage id
//       payload.bytes = the BTX fourcc without its trailing space ("ESP")
//   "settings.native_string"    translates one string this project authors.
//       payload.u64   = the XLanguage id
//       payload.bytes = UTF-8 "key=value"
//
// Keys for the last one: `resolution_label`, `framerate_label`, `text_label`
// and `overworld_model_label` for the rows this project adds to the game's own
// Options screen, and `achv_name_<id>`, `achv_desc_<id>`,
// `achv_desc_locked_<id>` for the F7 achievements overlay and its unlock toast.
// `<id>` is AchievementInfo::id, never the row's position.
void RegisterLanguageListeners(rex::system::ModRegistry* registry);

// Records one translated string directly, for the declarative
// `[language.strings]` table in a mod's assets.toml. Shares its storage and its
// first-wins rule with the "settings.native_string" event above, so a mod may
// use either route. Returns false, and warns, on a duplicate or an empty field.
bool RegisterNativeString(uint32_t language_id, std::string_view key, std::string_view value);

// Adds a Language entry directly, for the declarative `[[language]]` block in a
// mod's assets.toml (see eternalsonata_asset_system.h). `slot` is a BTX fourcc
// with or without its trailing space, and may be empty to leave the language
// pointing at no block of its own. Returns false, and warns, on a duplicate id,
// a slot another language already claimed, or a full list.
bool RegisterModLanguage(std::string_view id, std::string_view label, std::string_view code,
                         std::string_view slot);

// The built-in languages followed by the mod-added ones, in registration order.
// Rebuilt on each call, so it is always current no matter when the caller runs
// relative to the mods. The returned pointers are owned by the registry and
// live for the process.
std::vector<LanguageOption> GetLanguageOptions();

// A mod-published translation for `key` in XLanguage `language_id`, as UTF-8,
// or nullptr if none was registered. Owned by the registry, valid for the
// process. Callers drawing into the game's own screens have to transcode: the
// guest font is single-byte (see assets::TranscodeToGameEncoding).
const char* FindNativeString(uint32_t language_id, std::string_view key);

// The user_language cvar as an ordered list, for the native Text row in the
// game's Options screen. Index 0..UserLanguageCount()-1; UserLanguageCode
// returns the two-letter form the row draws ("EN", "DE", ...), and
// UserLanguageIndex the entry currently selected (0 if unrecognised: a mod that
// added a language and was then disabled leaves its id behind in the config,
// and that has to read as the first entry rather than clamp to the last).
// SetUserLanguageSetting writes and persists it; the guest only reads
// its language at boot, so the change needs a restart to show, and it is
// recorded as a pending restart accordingly.
int UserLanguageCount();
const char* UserLanguageCode(int index);
const char* UserLanguageLabel(int index);
int UserLanguageIndex();
void SetUserLanguageSetting(int index);

// The XLanguage id of the entry BootUserLanguageIndex names. This is the id a
// translation mod's "settings.native_string" payloads have to carry for the
// labels this project draws into the game's own screens to come out translated.
uint32_t BootUserLanguageId();

// The BTX language block the process booted into, as a fourcc with its trailing
// space ("ESP "), or nullptr if the boot language claimed none. Text patches
// addressed to a mod-added language route here.
const char* BootBtxSlot();

// Points the guest at the donor BTX block when the boot language is one a mod
// added. The guest resolves its language straight out of the user_language cvar
// (the SDK's kernel reads it for XConfig, the XDBF locale and the achievement
// store alike), and it has no idea a mod invented XLanguage 9. So the live cvar
// is rewritten, without persisting, to the built-in language that owns the same
// BTX block. The player's actual selection is remembered separately, so both
// menus keep showing the language they chose.
//
// Call once at startup, after the mods have registered and before the guest
// boots. No-op when the boot language is a built-in one.
void ApplyBootLanguageDonorSlot();

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
