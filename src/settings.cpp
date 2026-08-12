// eternalsonata - ReXGlue Recompiled Project
// See settings.h for details.

#include "settings.h"

#include "debug_area_overlay.h"
#include "field_player_model_override.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/system/auto_updater.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/imgui_widgets.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/window.h>
#include <imgui.h>

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

// In-game frame-rate cap (see DrawFrameRateRow). The value is the target fps
// the host limiter in eternalsonata_framerate.cpp holds the guest to, and which it
// declares to the sim via byte_82465F90.
//
// 60 is the highest rate this engine can express, and there is no preset above
// it because none can exist. A rate has to satisfy three constraints at once:
//
//   - Fit in a byte. byte_82465F90 is a u8, so 300 truncates to 44.
//   - Divide 300, or `300 / rate` truncates and game speed is wrong. 120 gives
//     2.5 -> 2 (~20% slow motion, confirmed in-game); 180 gives 1.67 -> 1,
//     which is 0.6x speed even on a PC that renders 180 perfectly.
//   - Divide 60, to stay on the authored grid. `300 / rate` is the step added
//     per frame and the content is written around the stock 30 (step 10) and
//     60 (step 5), so the step has to stay a multiple of 5. 75 -> 4, 100 -> 3
//     and 150 -> 2 all come off that grid, and the character models visibly
//     twitch even though game speed is arithmetically exact.
//
// 75/100/150 fail the third, 180 fails the second, 300 fails the first. A 150
// preset was tried and removed for exactly this reason. Players who want to get
// through content faster hold the fast-forward key instead (see TurboHeld in
// eternalsonata_framerate.cpp).
//
// "unlocked" disables the limiter and runs fast in proportion — a frame-clocked
// engine has no speed-correct uncapped mode.
//
// 60 frame-skips adaptively (see AdaptiveFrameRate): if the host can't sustain
// it, the declared rate drops through 30 and 20 — both divisors of 60, so both
// on-grid — and the game skips frames at correct speed rather than running the
// sim in slow motion, then climbs back once there is headroom. The rungs are
// fixed rather than derived from the player's refresh rate on purpose: the
// authored cadence is a property of the content, so the game should behave the
// same on every monitor.
//
// Declared here (global scope) so the hooks can read it via
// REXCVAR_DECLARE/REXCVAR_GET.
REXCVAR_DEFINE_STRING(frame_rate, "30", "Eternal Sonata",
                      "In-game scene/sim advance rate: 30 (as the game asks - 30 for gameplay, 60 "
                      "on the title and the save menu), 60, or unlocked. 60 automatically skips "
                      "frames if the PC can't sustain it, to avoid slow motion.")
    .allowed({"stock", "30", "60", "unlocked"});

// Escape hatch for the adaptive ladder. With this false, the selected rate is
// pinned and never steps down — the pre-adaptive behaviour, where a host that
// can't keep up runs the sim in slow motion rather than skipping frames. Worth
// having because a false step-down is far more visible than mild slow motion.
REXCVAR_DEFINE_BOOL(adaptive_framerate, true, "Eternal Sonata",
                    "Let the 60 frame rate setting drop to a lower rate when the PC can't "
                    "keep up, instead of running in slow motion");

// Per-second frame pacing summary, off by default. Answers, in one line: what
// rate are we declaring to the sim, how many presents/sec are we actually
// getting, how long does a frame's work really take, and what is the adaptive
// ladder doing about it. See the present hook in eternalsonata_framerate.cpp.
REXCVAR_DEFINE_BOOL(frame_debug, false, "Eternal Sonata",
                    "Log a per-second frame pacing summary (declared rate, achieved presents/sec, "
                    "frame work time, adaptive ladder state)");

// Debug aid for reverse-engineering the Options screen: with this on, every
// left/right press on the Subtitles row snapshots all committed guest memory
// and intersects it against the previous snapshots of the *other* value, which
// narrows down where a row's value (and the highlight bar that follows it)
// actually lives. Costs a full memory diff per press, so it is off by default.
// See the "Value-highlight hunt" section in eternalsonata_options.cpp.
REXCVAR_DEFINE_BOOL(menu_scan, false, "Eternal Sonata",
                    "Debug: diff guest memory across Subtitles row toggles to locate menu "
                    "value state (logs candidates)");

REXCVAR_DEFINE_BOOL(debug_show_all_maps, false, "Eternal Sonata",
                    "Debug: show all areas in debug overlay, including unnamed event/menu areas");

// Which model the overworld leader wears. "party" tracks the active party's
// first member (the game itself always spawns Allegretto regardless of party
// order); a character name pins that character; "default" leaves the game's
// own choice alone. Read by the spawn hook in field_player_model_override.cpp.
//
// The tokens double as the debug overlay's combo values, so the two stay in
// step -- see FieldPlayerModelOverride::SelectionNames().
REXCVAR_DEFINE_STRING(field_leader_model, "default", "Eternal Sonata",
                      "Model used by the overworld leader: default (the game's own), party (the "
                      "active party's first member), or a specific character")
    .allowed({"default", "party", "allegretto", "polka", "beat", "frederic", "viola", "salsa",
              "jazz", "falsetto", "claves", "march"});

namespace eternalsonata {

namespace {

// Everything build.py used to write into the shipped eternalsonata.toml.
// These are the game's defaults, not necessarily the SDK's; kept here so
// there is one place that owns "what Eternal Sonata ships with".
struct DefaultValue {
  const char* cvar;
  const char* value;
};

constexpr std::array kGameDefaults = {
    DefaultValue{"gpu_plugin", "xenos"},
    DefaultValue{"game_data_root", "assets"},
    DefaultValue{"gpu_allow_invalid_fetch_constants", "true"},
    DefaultValue{"d3d12_readback_memexport", "true"},
    DefaultValue{"d3d12_readback_resolve", "true"},
    DefaultValue{"clear_memory_page_state", "true"},
    // The game binds an 8-tile-wide render target at EDRAM tile 1720 whose draws
    // give no usable height estimate, so it claims all 2048 tiles and takes
    // ownership of the scene color target at tile 0 by wrapping. That range then
    // resolves out of the wrong surface as zeros, which is the black cross-fade
    // source on camera transitions and the black half of the save screenshot.
    DefaultValue{"no_edram_wrap_claim", "true"},
    // The SDK's vblank pump ties the presentation-interval wait to vsync, so
    // with vsync on the host limiter's "60" declared rate only ever achieves
    // the display's vblank-gated actual rate; on a 60 Hz-capped/throttled path
    // that reads as a real fps half of what's declared, which the frame-clocked
    // sim turns directly into half-speed motion (game speed = actual/declared).
    DefaultValue{"vsync", "false"},
    DefaultValue{"swap_post_effect", "fxaa"},
    DefaultValue{"mnk_capture_mouse", "false"},
    DefaultValue{"mnk_mode", "true"},
    DefaultValue{"resolution", "720p"},
    DefaultValue{"resolution_scale", "1"},
    DefaultValue{"fullscreen", "false"},
    DefaultValue{"audio_mute", "false"},
    DefaultValue{"audio_volume", "1"},
    DefaultValue{"shader_dump_enabled", "false"},
    DefaultValue{"texture_dump_enabled", "false"},
    DefaultValue{"texture_dump_format", "png"},
    DefaultValue{"texture_dump_skip_sizes", "1280x720,640x360,720x720"},
};

// cvars persisted to the friendly settings.toml by the Basic section.
// vulkan_device gets a custom row (a dynamic dropdown) rather than the generic
// DrawCvarWidget path, but is still listed here so the generic Reset-All /
// restart-tracking loops cover it; GetFlagInfo/ResetToDefault etc. no-op
// harmlessly for it on a build without Vulkan. gpu_backend no longer has a row
// at all (it is set from the config file or the Advanced section), but stays
// listed so an existing saved value survives a Reset-All round trip.
constexpr std::array<const char*, 12> kBasicCvarNames = {
    "fullscreen",  "resolution",   "resolution_scale", "user_language",
    "input_backend", "gpu_backend", "vulkan_device", "frame_rate",
    "audio_mute", "audio_volume", "adaptive_framerate", "field_leader_model"};

// audio_volume is stored (and applied to samples by the SDL audio driver) as
// linear amplitude, but human loudness perception is roughly logarithmic --
// a linear slider (amplitude == percent/100) would spend most of its travel
// on barely-perceptible changes near the top end and cram all the audible
// range into the last few percent at the bottom. Map the displayed 0-100%
// through a dB curve instead: -40dB at 0% (quiet enough to treat as silence
// below) up to 0dB (full amplitude) at 100%, evenly spaced in dB rather than
// in amplitude.
constexpr double kMinVolumeDb = -40.0;

double VolumeAmplitudeFromPercent(int percent) {
  if (percent <= 0)
    return 0.0;
  if (percent >= 100)
    return 1.0;
  double db = kMinVolumeDb * (100 - percent) / 100.0;
  return std::pow(10.0, db / 20.0);
}

int VolumePercentFromAmplitude(double amplitude) {
  if (amplitude <= 0.0)
    return 0;
  double db = 20.0 * std::log10(amplitude);
  if (db <= kMinVolumeDb)
    return 0;
  return std::clamp(static_cast<int>(std::lround(100.0 - db * 100.0 / kMinVolumeDb)), 0, 100);
}

struct LanguageOption {
  const char* id;  // stringified XLanguage value, as stored by the cvar
  const char* label;
};

// XLanguage IDs per the Xbox 360 kernel's user_language cvar
constexpr std::array kLanguageOptions = {
    LanguageOption{"1", "English"},
    LanguageOption{"3", "German"},
    LanguageOption{"4", "French"},
    LanguageOption{"5", "Spanish"},
    LanguageOption{"6", "Italian"},
};

struct FrameRateOption {
  const char* id;  // value stored by the frame_rate cvar
  const char* label;
};

constexpr std::array kFrameRateOptions = {
    // "30" follows the game's own requests rather than pinning every screen to
    // 30: the title and the save menu ask for 60, and their logic is written for
    // it. The host limiter paces whatever is asked for, so speed stays correct.
    FrameRateOption{"30", "30 FPS"},
    FrameRateOption{"60", "60 FPS"},
    FrameRateOption{"unlocked", "Unlocked"},
};

// cvars rendered generically in the collapsed Advanced section, persisted to
// the app's normal cvar config (eternalsonata.toml).
//
// Deliberately absent: gpu_allow_invalid_fetch_constants, d3d12_readback_resolve
// and no_edram_wrap_claim. Those are not preferences, they are the workarounds
// this game needs to render correctly (no_edram_wrap_claim in particular is the
// fix for the black cross-fades and the black half of the save screenshot -- see
// kGameDefaults). They keep their defaults from kGameDefaults and can still be
// set from eternalsonata.toml for debugging; they just are not offered as
// something to switch off by hand.
constexpr std::array<const char*, 7> kAdvancedCvarNames = {
    "shader_dump_enabled",
    "texture_dump_enabled",
    "texture_dump_format",
    "texture_dump_skip_sizes",
    "mnk_capture_mouse",
    "mnk_mode",
    "swap_post_effect",
};

// resolution_scale value that renders at "100%" (native) for a given display
// resolution. The SDK's resolution_scale is an integer EDRAM/draw
// supersampling factor (range 1-8), not a fractional multiplier, so this
// table is the source of truth for what "100%" means per resolution;
// DrawRenderScaleRow derives 50%-100% steps from it at runtime.
int ResolutionScaleFor(const std::string& resolution) {
  if (resolution == "1080p")
    return 2;
  if (resolution == "1440p")
    return 3;
  if (resolution == "4K")
    return 4;
  return 1;  // 720p, and fallback for anything unrecognized.
}

// Vertical pixel count of each named resolution preset.
int ResolutionHeightFor(const char* option) {
  std::string opt = option;
  if (opt == "1080p")
    return 1080;
  if (opt == "1440p")
    return 1440;
  if (opt == "4K")
    return 2160;
  return 720;  // 720p
}

// Remembered from CreateSettingsDialog so settings changed outside the overlay
// - e.g. the native Fullscreen row in the game's own Options screen - can be
// persisted to the same file the overlay writes. See SaveUserSettings. Also
// used by DesktopDisplayHeight below, since it's the only handle to the
// engine's Window this file has outside the settings dialog itself.
std::filesystem::path g_user_settings_path;
rex::ui::Window* g_window = nullptr;

// Height in pixels of the display the window is (or would be) shown on.
// Falls back to 4K (no filtering) if it can't be determined.
//
// Routed through rex::ui::Window::GetDesktopDisplayHeight() (backed by
// SDL_GetDisplayForWindow/SDL_GetDesktopDisplayMode) rather than a direct
// platform or SDL call from here: rex::runtime ships as rexruntimerd.dll
// with SDL3-static as an *interface* link dependency, so the DLL and this
// exe each get their own statically-linked copy of SDL3 with independent
// subsystem state. The DLL's copy is the one that calls SDL_Init/creates
// the window, so a direct SDL_GetPrimaryDisplay() call made from this exe's
// own copy would see no video subsystem and always fail; going through the
// Window object's virtual method instead runs inside the DLL, against the
// copy of SDL that actually owns the window. That also makes this
// cross-platform for free (Windows/X11/Wayland) instead of the previous
// GetSystemMetrics(SM_CYSCREEN), which only ever worked on Windows.
int DesktopDisplayHeight() {
  uint32_t height = g_window ? g_window->GetDesktopDisplayHeight() : 0;
  return height > 0 ? static_cast<int>(height) : 2160;
}

std::vector<std::string> BasicCvarNames() {
  return std::vector<std::string>(kBasicCvarNames.begin(), kBasicCvarNames.end());
}

// Populated once by InitSettingsCaches() at startup; CuratedSettingsDialog
// reads from these instead of re-enumerating GPU plugins/Vulkan devices
// every time the F4 overlay is opened.
std::vector<std::string> g_gpu_plugin_names_cache;
#if REX_HAS_VULKAN
std::vector<rex::ui::vulkan::DeviceInfo> g_vulkan_devices_cache;
#endif

class CuratedSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  CuratedSettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                        std::filesystem::path user_settings_path,
                        std::filesystem::path app_config_path,
                        rex::input::InputSystem* input_system)
      : rex::ui::ImGuiDialog(drawer),
        window_(window),
        user_settings_path_(std::move(user_settings_path)),
        app_config_path_(std::move(app_config_path)),
        input_system_(input_system) {
    gpu_plugin_names_ = g_gpu_plugin_names_cache;
#if REX_HAS_VULKAN
    vulkan_devices_ = g_vulkan_devices_cache;
#endif
  }

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin("Settings##rex", nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (AnyPendingRestart()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
      ImGui::TextWrapped("Some changes require a restart to take effect.");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Restart Now")) {
        if (rex::platform::process::Relaunch() && window_) {
          window_->RequestClose();
        }
      }
      ImGui::Separator();
    }

    DrawUpdateSection();

    DrawFullscreenRow();
    DrawResolutionRow();
    DrawRenderScaleRow();
    DrawFrameRateRow();
    DrawAdaptiveFrameRateRow();
    DrawAudioMuteRow();
    DrawAudioVolumeRow();
    DrawLanguageRow();
    DrawFieldLeaderModelRow();
    DrawInputBackendRow();
#if REX_HAS_VULKAN
    if (rex::cvar::GetFlagByName("gpu_backend") == "vulkan") {
      DrawVulkanDeviceRow();
    }
#endif

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Advanced")) {
      for (const char* name : kAdvancedCvarNames) {
        DrawAdvancedRow(name);
      }
      DrawGpuPluginRow();
    }

    ImGui::Separator();
    if (ImGui::Button("Reset All to Defaults")) {
      for (const char* name : kBasicCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      for (const char* name : kAdvancedCvarNames) {
        rex::cvar::ResetToDefault(name);
      }
      // Not in either list above since it gets a custom row (dynamic
      // dropdown), not the generic DrawCvarWidget path.
      rex::cvar::ResetToDefault("gpu_plugin");
      SaveBasic();
      SaveAdvanced();
    }
    ImGui::SameLine();
    // Debug tool: force-loads a field area out of turn. See
    // debug_area_overlay.h / force_load_area.h for the safety caveats.
    if (ImGui::Button(debug_area_overlay_ ? "Close Debug" : "Debug...")) {
      if (debug_area_overlay_) {
        debug_area_overlay_.reset();
      } else {
        debug_area_overlay_ = eternalsonata::CreateDebugAreaOverlay(imgui_drawer());
      }
    }
    ImGui::SameLine();
    // Opens the SDK's own full cvar browser (the same one bind_settings/F4
    // would show if settings_manager_enabled were false) for anything not
    // surfaced above. It's a separate top-level ImGuiDialog -- constructing
    // it registers it with the drawer (see ImGuiDialog's ctor), so it starts
    // drawing/receiving input immediately, independent of this dialog; this
    // button just toggles that lifetime, mirroring bind_settings's own
    // open/close toggle in rex_app.cpp. Given a distinct window_title
    // ("All Settings##rexdev") so its ImGui window doesn't share an ID
    // with this dialog's own "Settings##rex" -- same ID would merge both
    // dialogs' draws into a single squeezed window instead of two.
    if (ImGui::Button(dev_settings_overlay_ ? "Close All Settings" : "All Settings...")) {
      if (dev_settings_overlay_) {
        dev_settings_overlay_.reset();
      } else {
        dev_settings_overlay_ = std::make_unique<rex::ui::SettingsDialog>(
            imgui_drawer(), app_config_path_, input_system_, "All Settings##rexdev");
      }
    }

    ImGui::End();
  }

 private:
  // GetPendingRestartFlags() only tracks cvars that were actually changed at
  // runtime (settings UI, console, mods) this session -- values applied
  // while loading a config file at boot don't count, so a saved preference
  // that simply differs from the SDK's factory default (e.g. Resolution set
  // to 1080p) doesn't trip this on a fresh launch. See SetFlagByNameImpl's
  // mark_restart parameter in the SDK's cvar.cpp.
  bool AnyPendingRestart() {
    auto pending = rex::cvar::GetPendingRestartFlags();
    auto is_tracked = [&pending](const char* name) {
      return std::find(pending.begin(), pending.end(), name) != pending.end();
    };
    for (const char* name : kBasicCvarNames) {
      if (is_tracked(name))
        return true;
    }
    for (const char* name : kAdvancedCvarNames) {
      if (is_tracked(name))
        return true;
    }
    // Not in either list above since it gets a custom row (dynamic
    // dropdown), not the generic DrawCvarWidget path.
    if (is_tracked("gpu_plugin"))
      return true;
    return false;
  }

  void SaveBasic() { rex::cvar::SaveConfigSubset(user_settings_path_, BasicCvarNames()); }
  void SaveAdvanced() { rex::cvar::SaveConfig(app_config_path_); }

  // Game self-update (see rex::system::AutoUpdater), surfaced here rather
  // than the SDK's mod manager overlay (F1) since a player who never touches
  // mods should still be told about an available update
  void DrawUpdateSection() {
    if (!update_check_requested_) {
      update_check_requested_ = true;
      auto_updater_.CheckAsync();
    }

    // A previous session already downloaded and staged an update (whether or
    // not this one ever calls CheckAsync/InstallAsync again); offer the
    // restart regardless of auto_updater_'s own in-memory state.
    if (rex::system::AutoUpdater::HasPendingSelfUpdate(rex::filesystem::GetExecutableFolder())) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
      ImGui::TextWrapped("An update has been downloaded.");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Restart & Apply##autoupdate")) {
        // This install root contains the running executable itself,
        // which stays locked for this process's whole lifetime (see
        // AutoUpdater::ApplyAndRestart's contract). The spawned helper
        // outlives this process, applies the swap, and launches the new exe.
        if (rex::system::AutoUpdater::ApplyAndRestart(rex::filesystem::GetExecutableFolder(),
                                                       rex::filesystem::GetExecutablePath()) &&
            window_) {
          window_->RequestClose();
        }
      }
      ImGui::Separator();
      return;
    }

    auto install = auto_updater_.InstallSnapshot();
    if (install.in_progress) {
      if (install.total_bytes > 0) {
        ImGui::TextDisabled("Downloading update... %.0f%%",
                            100.0 * static_cast<double>(install.downloaded_bytes) /
                                static_cast<double>(install.total_bytes));
      } else {
        ImGui::TextDisabled("Downloading update...");
      }
      ImGui::Separator();
      return;
    }
    if (install.done && !install.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
      ImGui::TextWrapped("%s", install.message.c_str());
      ImGui::PopStyleColor();
      ImGui::Separator();
      return;
    }

    if (auto_updater_.state() != rex::system::UpdateCheckState::kUpdateAvailable) {
      return;  // kIdle/kChecking/kUpToDate/kFailed: nothing worth showing.
    }
    auto info = auto_updater_.Available();
    if (!info) {
      return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1.0f));
    ImGui::TextWrapped("Update available: v%s", info->version.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Download Update")) {
      auto_updater_.InstallAsync(*info, rex::filesystem::GetExecutableFolder());
    }
    ImGui::Separator();
  }

  void DrawFullscreenRow() {
    const auto* entry = rex::cvar::GetFlagInfo("fullscreen");
    if (!entry)
      return;
    ImGui::TextUnformatted("Fullscreen");
    ImGui::SameLine(180.0f);
    ImGui::PushID("fullscreen");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawAudioMuteRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_mute");
    if (!entry)
      return;
    ImGui::TextUnformatted("Mute Audio");
    ImGui::SameLine(180.0f);
    ImGui::PushID("audio_mute");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // audio_volume is a Double cvar (0.0-1.0 linear amplitude, applied directly
  // to samples by the SDL audio driver); DrawCvarWidget's generic Double path
  // is a plain InputDouble box, not a slider, so this draws its own row the
  // same way DrawRenderScaleRow does for resolution_scale -- displaying and
  // editing a perceptually-spaced percentage (see VolumeAmplitudeFromPercent)
  // rather than the raw amplitude directly.
  void DrawAudioVolumeRow() {
    const auto* entry = rex::cvar::GetFlagInfo("audio_volume");
    if (!entry)
      return;
    const auto* mute_entry = rex::cvar::GetFlagInfo("audio_mute");
    if (mute_entry && mute_entry->getter() == "true")
      return;

    int percent = VolumePercentFromAmplitude(std::atof(entry->getter().c_str()));

    ImGui::TextUnformatted("Volume");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("audio_volume");
    bool changed = ImGui::SliderInt("##v", &percent, 0, 100, "%d%%");
    if (changed) {
      rex::cvar::SetFlagByName("audio_volume", std::to_string(VolumeAmplitudeFromPercent(percent)),
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawResolutionRow() {
    const auto* entry = rex::cvar::GetFlagInfo("resolution");
    if (!entry)
      return;
    static constexpr std::array<const char*, 4> kAllOptions = {"720p", "1080p", "1440p", "4K"};

    // Only offer presets that fit on the user's actual display -- no point
    // letting someone pick 4K on a 1080p monitor.
    int count = std::clamp(AllowedResolutionCount(), 1, static_cast<int>(kAllOptions.size()));
    std::vector<const char*> options(kAllOptions.begin(), kAllOptions.begin() + count);

    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
      if (current == options[i]) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Resolution");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("resolution");
    int sel = cur_idx;
    // Discrete slider, matching DrawFrameRateRow: snaps between presets
    // rather than allowing arbitrary drag positions.
    if (ImGui::SliderInt("##v", &sel, 0, static_cast<int>(options.size()) - 1, options[sel],
                         ImGuiSliderFlags_NoInput)) {
      rex::cvar::SetFlagByName("resolution", options[sel], /*persist=*/true);
      rex::cvar::SetFlagByName("resolution_scale",
                               std::to_string(ResolutionScaleFor(options[sel])),
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  // resolution_scale is an integer cvar (range 1-8) whose named-resolution
  // steps (1/2/3/4 for 720p/1080p/1440p/4K, per ResolutionScaleFor) are the
  // only meaningful values -- each option here renders at one of the named
  // resolutions up to and including the current display resolution (e.g. at
  // 1440p: render at 720p/1080p/1440p, i.e. 33%/67%/100%). Rather than
  // sliding over the percentage itself (which lets the handle rest on
  // in-between values while dragging, since e.g. 73% is a perfectly valid
  // int even though no scale produces it), the slider's domain *is* the
  // list of valid scales, so it's mechanically impossible to drag to
  // anything else. 720p's base of 1 has only one valid scale, so the
  // slider is disabled there instead of doing nothing.
  void DrawRenderScaleRow() {
    const auto* scale_entry = rex::cvar::GetFlagInfo("resolution_scale");
    const auto* res_entry = rex::cvar::GetFlagInfo("resolution");
    if (!scale_entry || !res_entry)
      return;

    int base = ResolutionScaleFor(res_entry->getter());
    int current_scale = std::atoi(scale_entry->getter().c_str());

    std::vector<int> valid_scales;
    for (int k = 1; k <= base; ++k) {
      valid_scales.push_back(k);
    }

    int idx = 0;
    for (int i = 0; i < static_cast<int>(valid_scales.size()); ++i) {
      if (valid_scales[i] == current_scale) {
        idx = i;
        break;
      }
    }

    int max_idx = static_cast<int>(valid_scales.size()) - 1;
    if (max_idx == 0)
      return;  // Only one valid scale (720p) -- nothing to offer, hide the row.

    ImGui::PushID("render_scale_percent");

    ImGui::TextUnformatted("Render Resolution");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    bool changed = ImGui::SliderInt("##v", &idx, 0, max_idx, "");

    int display_percent = static_cast<int>(std::lround(100.0 * valid_scales[idx] / base));
    ImGui::SameLine();
    ImGui::Text("%d%%", display_percent);

    if (changed) {
      rex::cvar::SetFlagByName("resolution_scale", std::to_string(valid_scales[idx]),
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawLanguageRow() {
    const auto* entry = rex::cvar::GetFlagInfo("user_language");
    if (!entry)
      return;
    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(kLanguageOptions.size()); ++i) {
      if (current == kLanguageOptions[i].id) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Language");
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("user_language");
    if (ImGui::BeginCombo("##v", kLanguageOptions[cur_idx].label)) {
      for (int i = 0; i < static_cast<int>(kLanguageOptions.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(kLanguageOptions[i].label, selected)) {
          rex::cvar::SetFlagByName("user_language", kLanguageOptions[i].id, /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  // Which character model the overworld leader wears. The game always spawns
  // Allegretto there regardless of party order; the spawn hook in
  // field_player_model_override.cpp substitutes a different cached model
  // handle. Selection is owned by FieldPlayerModelOverride (which mirrors the
  // field_leader_model cvar into an atomic for the guest thread), so this row
  // goes through it rather than touching the cvar directly.
  void DrawFieldLeaderModelRow() {
    int selection = eternalsonata::FieldPlayerModelOverride::Selection();

    ImGui::TextUnformatted("Field Leader Model");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Which character is shown walking around the overworld.\n\n"
          "\"Follow active party\" uses whoever is first in the party, which you reorder "
          "from the status screen.\n\n"
          "The model can only be swapped while the field is paused, so a change takes "
          "effect the next time you close a menu or move between areas -- not instantly. "
          "Characters whose model has not been loaded yet fall back to the default.");
    }
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("field_leader_model");
    if (ImGui::Combo("##v", &selection,
                     eternalsonata::FieldPlayerModelOverride::SelectionNames(),
                     eternalsonata::FieldPlayerModelOverride::kSelectionCount)) {
      // SetSelection persists via SaveUserSettings itself.
      eternalsonata::FieldPlayerModelOverride::SetSelection(selection);
    }
    ImGui::PopID();
  }

  // Controls the frame rate the game runs at. The hooks in
  // eternalsonata_framerate.cpp read the frame_rate cvar, declare that rate to the
  // sim (byte_82465F90) and hold the present thread to it with a host limiter.
  // Applied at runtime, no restart needed.
  void DrawFrameRateRow() {
    std::string current = rex::cvar::GetFlagByName("frame_rate");
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(kFrameRateOptions.size()); ++i) {
      if (current == kFrameRateOptions[i].id) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("Frame Rate");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "How often the in-game scene and simulation advance. The original "
          "game is capped at 30 FPS; Unlocked runs as fast as the CPU and GPU "
          "allow.");
    }
    ImGui::SameLine(180.0f);
    // Match the combo boxes in this menu (Language, Input Backend, ...).
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("frame_rate");
    int sel = cur_idx;
    // Discrete 0..N-1 slider; format shows the label of the current option
    // (re-evaluated per frame). NoInput keeps it snapping between presets.
    if (ImGui::SliderInt("##v", &sel, 0,
                         static_cast<int>(kFrameRateOptions.size()) - 1,
                         kFrameRateOptions[sel].label,
                         ImGuiSliderFlags_NoInput)) {
      rex::cvar::SetFlagByName("frame_rate", kFrameRateOptions[sel].id,
                               /*persist=*/true);
      SaveBasic();
    }
    ImGui::PopID();
  }

  // Companion to the Frame Rate row above, and only meaningful next to it: it
  // governs what happens when the selected rate can't be sustained. Hidden
  // for "30" and "Unlocked", which have nothing to step down to
  void DrawAdaptiveFrameRateRow() {
    const auto* entry = rex::cvar::GetFlagInfo("adaptive_framerate");
    if (!entry)
      return;
    if (rex::cvar::GetFlagByName("frame_rate") != "60")
      return;

    ImGui::TextUnformatted("Adaptive Frame Rate");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "If the PC can't hold 60 FPS, drop to 30 and then 20 rather than running "
          "the game in slow motion. Returns to 60 once there is headroom again.");
    }
    ImGui::SameLine(180.0f);
    ImGui::PushID("adaptive_framerate");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  void DrawInputBackendRow() {
    const auto* entry = rex::cvar::GetFlagInfo("input_backend");
    if (!entry)
      return;
    ImGui::TextUnformatted("Input Backend");
    ImGui::SameLine(180.0f);
    ImGui::PushID("input_backend");
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveBasic();
    }
    ImGui::PopID();
  }

  // gpu_plugin names a rexgpu-<name>[postfix].dll staged next to the
  // executable; unlike gpu_backend it has no fixed `.allowed(...)` list
  // since the valid set depends on what's actually staged there, so this
  // combo is populated from rex::system::EnumerateGpuPlugins() instead of
  // going through the generic DrawCvarWidget (which would fall back to a
  // plain text field for an unconstrained string cvar).
  void DrawGpuPluginRow() {
    const auto* entry = rex::cvar::GetFlagInfo("gpu_plugin");
    if (!entry || gpu_plugin_names_.empty())
      return;

    std::string current = entry->getter();
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(gpu_plugin_names_.size()); ++i) {
      if (current == gpu_plugin_names_[i]) {
        cur_idx = i;
        break;
      }
    }

    ImGui::TextUnformatted("gpu_plugin");
    if (!entry->description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(240.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("gpu_plugin");
    if (ImGui::BeginCombo("##v", gpu_plugin_names_[cur_idx].c_str())) {
      for (int i = 0; i < static_cast<int>(gpu_plugin_names_.size()); ++i) {
        bool selected = (i == cur_idx);
        if (ImGui::Selectable(gpu_plugin_names_[i].c_str(), selected)) {
          rex::cvar::SetFlagByName("gpu_plugin", gpu_plugin_names_[i], /*persist=*/true);
          SaveAdvanced();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

#if REX_HAS_VULKAN
  // vulkan_device is a raw index into the physical-device list the Vulkan
  // provider enumerates at graphics setup time (-1 = auto-select). Entries
  // flagged is_duplicate_of_earlier are the same physical device as an
  // earlier entry (a driver/ICD quirk, not a second GPU) -- skipped here
  // since offering them would be a redundant, indistinguishable choice, not
  // just a duplicate label; the earlier entry's index selects the exact same
  // device.
  void DrawVulkanDeviceRow() {
    const auto* entry = rex::cvar::GetFlagInfo("vulkan_device");
    if (!entry || vulkan_devices_.empty())
      return;

    int current = std::atoi(entry->getter().c_str());
    auto label_for = [this](int real_idx) -> const std::string& {
      static const std::string kAuto = "Auto";
      return real_idx < 0 ? kAuto : vulkan_devices_[real_idx].name;
    };

    ImGui::TextUnformatted("Vulkan Device");
    if (!entry->description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::PushID("vulkan_device");
    if (ImGui::BeginCombo("##v", label_for(current).c_str())) {
      {
        bool selected = (current < 0);
        ImGui::PushID(-1);
        if (ImGui::Selectable("Auto", selected)) {
          rex::cvar::SetFlagByName("vulkan_device", "-1", /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      for (int i = 0; i < static_cast<int>(vulkan_devices_.size()); ++i) {
        if (vulkan_devices_[i].is_duplicate_of_earlier)
          continue;
        bool selected = (current == i);
        ImGui::PushID(i);
        if (ImGui::Selectable(vulkan_devices_[i].name.c_str(), selected)) {
          rex::cvar::SetFlagByName("vulkan_device", std::to_string(i), /*persist=*/true);
          SaveBasic();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
#endif

  void DrawAdvancedRow(const char* name) {
    const auto* entry = rex::cvar::GetFlagInfo(name);
    if (!entry)
      return;

    bool read_only = (entry->lifecycle == rex::cvar::Lifecycle::kInitOnly);
    ImGui::PushID(name);
    if (read_only)
      ImGui::BeginDisabled();

    ImGui::TextUnformatted(name);
    if (!entry->description.empty() && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry->description.c_str());
    }
    ImGui::SameLine(240.0f);
    if (rex::ui::DrawCvarWidget(*entry, 160.0f, /*persist=*/true)) {
      SaveAdvanced();
    }

    if (read_only)
      ImGui::EndDisabled();
    ImGui::PopID();
  }

  rex::ui::Window* window_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path app_config_path_;
  std::vector<std::string> gpu_plugin_names_;
#if REX_HAS_VULKAN
  std::vector<rex::ui::vulkan::DeviceInfo> vulkan_devices_;
#endif
  rex::input::InputSystem* input_system_;
  std::unique_ptr<rex::ui::SettingsDialog> dev_settings_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> debug_area_overlay_;

  rex::system::AutoUpdater auto_updater_;
  bool update_check_requested_ = false;
};

}  // namespace

// Ordered ascending; both this overlay's Resolution row and the native
// Options screen's Resolution row (eternalsonata_options.cpp) treat this as
// the definitive list, each truncating to however many entries it itself
// offers (the native menu tops out at 1440p, no 4K row).
constexpr std::array<const char*, 4> kResolutionPresetsAscending = {"720p", "1080p", "1440p", "4K"};

int AllowedResolutionCount() {
  int display_height = DesktopDisplayHeight();
  int count = 0;
  for (const char* opt : kResolutionPresetsAscending) {
    if (ResolutionHeightFor(opt) > display_height)
      break;
    ++count;
  }
  return count > 0 ? count : 1;  // Always leave at least 720p.
}

void ApplySettingDefaults() {
  for (const auto& d : kGameDefaults) {
    rex::cvar::SetDefaultValue(d.cvar, d.value);
  }
}

void InitSettingsCaches() {
  g_gpu_plugin_names_cache = rex::system::EnumerateGpuPlugins();
#if REX_HAS_VULKAN
  g_vulkan_devices_cache = rex::ui::vulkan::EnumerateDevices();
#endif
}

void BindSettingsTargets(rex::ui::Window* window,
                         std::filesystem::path user_settings_path) {
  g_window = window;
  g_user_settings_path = std::move(user_settings_path);
}

void SaveUserSettings() {
  if (g_user_settings_path.empty()) {
    return;
  }
  rex::cvar::SaveConfigSubset(g_user_settings_path, BasicCvarNames());
}

void SetFrameRateSetting(const char* value) {
  auto* entry = rex::cvar::GetFlagInfo("frame_rate");
  if (!entry || !entry->setter || entry->getter() == value) {
    return;
  }
  // frame_rate is hot-reload, not kRequiresRestart, so this doesn't mark a
  // pending restart -- but going through SetFlagByName (as SetResolutionSetting
  // does) rather than entry->setter directly still matters: it's what runs
  // registered change callbacks and sets persist_to_config, same as every
  // other settings path in this file (DrawFrameRateRow included).
  rex::cvar::SetFlagByName("frame_rate", value, /*persist=*/true);
  SaveUserSettings();
}

void SetAdaptiveFramerateSetting(bool enabled) {
  auto* entry = rex::cvar::GetFlagInfo("adaptive_framerate");
  if (!entry || !entry->setter || (entry->getter() == "true") == enabled) {
    return;
  }
  entry->setter(enabled ? "true" : "false");
  SaveUserSettings();
}

void SetResolutionSetting(const char* value) {
  auto* res_entry = rex::cvar::GetFlagInfo("resolution");
  auto* scale_entry = rex::cvar::GetFlagInfo("resolution_scale");
  if (!res_entry || !res_entry->setter || !scale_entry || !scale_entry->setter ||
      res_entry->getter() == value) {
    return;
  }
  // resolution and resolution_scale are both kRequiresRestart -- go through
  // rex::cvar::SetFlagByName (not entry->setter directly, as the other
  // Set*Setting helpers in this file do) so the change is recorded by
  // MarkPendingRestart. That's what makes AnyPendingRestart() /
  // GetPendingRestartFlags() -- and so the overlay's "restart to apply"
  // banner -- notice a resolution change made from the native Options row,
  // not just from this file's own DrawResolutionRow.
  rex::cvar::SetFlagByName("resolution", value, /*persist=*/true);
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(ResolutionScaleFor(value)),
                           /*persist=*/true);
  SaveUserSettings();
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateSettingsDialog(
    rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    std::filesystem::path user_settings_path, std::filesystem::path app_config_path,
    rex::input::InputSystem* input_system) {
  g_user_settings_path = user_settings_path;
  g_window = window;
  return std::make_unique<CuratedSettingsDialog>(drawer, window, std::move(user_settings_path),
                                                 std::move(app_config_path), input_system);
}

}  // namespace eternalsonata
