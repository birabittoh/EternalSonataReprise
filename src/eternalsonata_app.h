// eternalsonata - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/discord_rpc.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/system/game_data_selector.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_theme.h>
#include <rex/ui/window.h>
#include <rex/version.h>

#include "battle_system.h"
#include "field_player_model_override.h"
#include "fonts.generated.h"
#include "force_load_area.h"
#include "guest_profiler.h"
#include "host_timer_resolution.h"
#include "icon.generated.h"
#include "native_renderer.h"
#include "native_renderer_overlay.h"
#include "native_renderer_plume.h"
#include "native_renderer_profile.h"
#include "native_renderer_shader_debug.h"
#include "party_system.h"
#include "photo_system.h"
#include "room_presence.h"
#include "save_system.h"
#include "settings.h"
#include "touch_layout.h"

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>
#include <rex/filesystem.h>

#include <filesystem>
#include <string>

namespace eternalsonata {

// Make a GPU plugin reachable at the path the SDK's loader checks.
//
// LoadGpuPlugin resolves rexgpu-<name> against rex::filesystem::
// GetExecutableFolder(), which on Android is the app's files directory. There
// is no executable directory to stage into there: the plugin ships inside the
// APK and Android extracts it, alongside libeternalsonata.so itself, into the
// read-only native library directory. Without this the loader reports
// "GPU plugin 'xenos' not found at /data/.../files/librexgpu-xenos.so" and the
// app exits before the window ever appears.
//
// A symlink rather than a copy, for two reasons: it avoids duplicating four
// megabytes into the app's data on every launch, and it leaves dlopen opening a
// file the app cannot write, which is what an app targeting API 35 needs. The
// native library directory is found from this library's own path rather than
// through JNI, since the plugin sits next to it.
inline void StageAndroidGpuPlugin(const std::string& plugin_name) {
  if (plugin_name.empty())
    return;

  Dl_info info = {};
  if (dladdr(reinterpret_cast<const void*>(&StageAndroidGpuPlugin), &info) == 0 ||
      info.dli_fname == nullptr) {
    REXLOG_WARN(
        "android: could not locate this library, so GPU plugin '{}' cannot be staged where the "
        "loader looks for it",
        plugin_name);
    return;
  }

  const std::string file_name = "librexgpu-" + plugin_name + ".so";
  const std::filesystem::path source =
      std::filesystem::path(info.dli_fname).parent_path() / file_name;
  const std::filesystem::path link = rex::filesystem::GetExecutableFolder() / file_name;

  std::error_code ec;
  if (!std::filesystem::exists(source, ec)) {
    REXLOG_WARN("android: GPU plugin '{}' is not in the APK's library directory ({})", plugin_name,
                source.string());
    return;
  }

  // Replace whatever is there: the library directory path contains an
  // install-specific token, so a link left by a previous install is stale.
  if (std::filesystem::exists(std::filesystem::symlink_status(link, ec))) {
    if (std::filesystem::read_symlink(link, ec) == source && !ec)
      return;
    std::filesystem::remove(link, ec);
  }

  std::filesystem::create_symlink(source, link, ec);
  if (ec) {
    REXLOG_WARN("android: could not link GPU plugin '{}' into {} ({}), copying instead",
                plugin_name, link.string(), ec.message());
    std::filesystem::copy_file(source, link,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      REXLOG_ERROR("android: could not stage GPU plugin '{}': {}", plugin_name, ec.message());
      return;
    }
  }
  REXLOG_INFO("android: GPU plugin '{}' staged at {} -> {}", plugin_name, link.string(),
              source.string());
}

}  // namespace eternalsonata
#endif

class EternalsonataApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<EternalsonataApp>(new EternalsonataApp(ctx, "eternalsonata", PPCImageConfig));
  }

  bool SetupEnvironment() override {
    // Game defaults for SDK cvars must land before the SDK loads any config
    // file, so a saved/CLI/env override still wins.
    eternalsonata::ApplySettingDefaults();

    if (!rex::ReXApp::SetupEnvironment())
      return false;

    // User-facing settings (Fullscreen, Resolution) live in their own file,
    // separate from the advanced cvars in the app's normal config, and are
    // loaded last so they win over both.
    if (std::filesystem::exists(user_settings_path()))
      rex::cvar::LoadConfig(user_settings_path());

    // Both config files are loaded by now, so the cvar holds the user's value.
    // Has to land before the guest starts: the audio thread arms its 5 ms
    // periodic timer early, and the period it gets is whatever the host tick
    // rate is at that moment. See host_timer_resolution.h.
    eternalsonata::ApplyHostTimerResolution();

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256 = "91184E7765172A358ECAA6E5CA1784DB1AE796C60F25051A45C5206F8949501E";
    settings.config_path = config_path();

    return rex::system::GameDataSelector::EnsureGameData(settings);
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    atlas->AddFontDefault();

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(eternalsonata::kPTSerifRegularTTF),
                                static_cast<int>(eternalsonata::kPTSerifRegularTTFSize), 16.0f, &cfg);
  }

  // The overlay's whole color palette is mathematically derived (see
  // rex::ui::ApplyAccentTheme) from this single accent.
  static constexpr ImVec4 kDefaultAccentColor = ImVec4(0x4F / 255.0f, 0x28 / 255.0f, 0x06 / 255.0f, 1.00f);

  void OnConfigureStyle(ImGuiStyle& style) override {
    rex::ui::ApplyAccentTheme(style, kDefaultAccentColor);
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Identifies this project to the SDK's mod manager overlay ("All" tab)
    // as the goopie.xyz `recompName` to query the public mod catalog for
    config.catalog_name = "eternalsonata";

    // Lets the SDK's AutoUpdater (F1 mod manager overlay) check
    // github.com/birabittoh/EternalSonataReprise's Releases for a newer build
    // and offer to download + apply it. "{tag}"/"{platform}" are substituted
    // by the SDK.
    config.update_asset_format = "eternalsonata-{tag}-{platform}";
    config.update_repo = "birabittoh/EternalSonataReprise";

    // One-shot toast shown top-left as the game starts
    config.startup_hint = "Press F4 to open settings.";

    // "plume" is not a plugin the SDK can load; it selects this project's own
    // renderer. Clearing the name here (the last point before ReXApp decides
    // whether to call LoadGpuPlugin) is what leaves the SDK headless.
    // NativeRendererEnabled() keeps reading the cvar, which still says
    // "plume". See native_renderer.h.
    if (config.gpu_plugin == eternalsonata::kNativeRendererPluginName)
      config.gpu_plugin.clear();

#if REX_PLATFORM_ANDROID
    // Anything still named here is a real plugin the SDK is about to load, and
    // on Android it has to be linked into place first. See
    // StageAndroidGpuPlugin.
    eternalsonata::StageAndroidGpuPlugin(config.gpu_plugin);
#endif

    // Whatever the unloaded plugin would have registered, this renderer has to
    // register itself. Runs after the clear only for reading order; it looks at
    // the cvar, not at config.gpu_plugin.
    eternalsonata::RegisterNativeRendererCvars();
  }

  // With gpu_plugin set to "plume" the SDK loads no graphics backend, and this
  // project renders the guest itself at the Direct3D level rather than
  // emulating Xenos. Runs before the guest starts, so no D3D call can arrive
  // ahead of it. See native_renderer.h.
  void OnPreLaunchModule() override { eternalsonata::InitNativeRenderer(window()); }

  // Detached overlay mode: with no GPU plugin the SDK creates no presenter and
  // asks the app for a drawer instead. Returning null here is what left the
  // window without F3/F4 and every other overlay. See native_renderer_overlay.h.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return eternalsonata::CreatePlumeImmediateDrawer();
  }

  void OnPostSetup() override {
    // The overlays record into the host frame, so the renderer needs the drawer
    // that produces them. Done here because imgui_drawer() is only live once
    // presentation has been set up.
    eternalsonata::PlumeSetOverlayDrawer(imgui_drawer());

    // Seed the GPU plugin/Vulkan device lists once here rather than every
    // time the F4 settings overlay is opened (see settings.cpp).
    eternalsonata::InitSettingsCaches();

    // Bind the window/settings file now rather than waiting for the F4 overlay
    // to be constructed, so the native Fullscreen row in the game's own Options
    // screen can apply and persist from a cold start.
    eternalsonata::BindSettingsTargets(window(), user_settings_path());

    window()->SetIcon(eternalsonata::kIconPNG, eternalsonata::kIconPNGSize);
    window()->SetTitle("Eternal Sonata: Reprise " + std::string(REXGLUE_BUILD_TITLE));

    // Wire the F3 debug overlay's "Guest" FPS line to the real guest present
    // rate. Count guest frames at the per-swap boundary; the SDK's runtime
    // uses the same callback to tick the mod registry, so chain through to
    // keep that behavior while counting. The provider is only invoked while
    // the F3 overlay is open (ImGui draw thread), so the window below is
    // single-threaded; the swap counter itself is atomic.
    auto* gs = runtime()->graphics_system();
    if (gs) {
      gs->SetHostSwapCallback([this] {
        runtime()->mod_registry()->DispatchTick();
        guest_swap_count_.fetch_add(1, std::memory_order_relaxed);
      });
    }
    SetGuestFrameStats([this]() {
      rex::ui::FrameStats stats;
      const uint64_t count = guest_swap_count_.load(std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();

      // Trailing-window average over ~1s of wall time. The guest swap rate is
      // usually close to the host ImGui draw rate, so a per-frame delta
      // aliases (each UI frame catches 0 or 1 swaps -> the displayed value
      // flickers between 0 and the real rate); averaging over a window fixes
      // that.
      stats_samples_.push_back({now, count});
      const auto window = std::chrono::duration<double>(kStatsWindowSec);
      while (stats_samples_.size() > 1 &&
             std::chrono::duration<double>(now - stats_samples_.front().first) > window) {
        stats_samples_.pop_front();
      }
      const double dt = std::chrono::duration<double>(now - stats_samples_.front().first).count();
      const uint64_t frames = count - stats_samples_.front().second;
      if (dt > 0.0 && frames > 0) {
        stats.fps = static_cast<double>(frames) / dt;
        stats.frame_time_ms = dt / static_cast<double>(frames) * 1000.0;
      }
      stats.frame_count = count;
      return stats;
    });

    // The line the frame rate above does not answer: whether the frame is
    // waiting on us or on the GPU. Both halves are measured by the native
    // renderer -- the fence wait and the queue's own timestamp pair -- so the
    // verdict is a measurement, not a guess, and it is the first thing to read
    // before changing anything for speed. Only meaningful under the native
    // renderer; the Xenos backend measures neither, so the row is left out
    // rather than shown reading zero.
    if (eternalsonata::NativeRendererEnabled()) {
      SetDebugOverlayDetails([]() {
        const auto bound = eternalsonata::GetFrameBoundStats();
        const bool gpu_bound = bound.verdict[0] == 'G';
        const bool present_bound = bound.verdict[0] == 'P';
        const ImVec4 colour = present_bound ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f)
                              : gpu_bound   ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f)
                                            : ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
        ImGui::TextColored(colour, "%s", bound.verdict);
        ImGui::Text("CPU: %.2f ms busy + %.2f ms fence wait", bound.cpu_ms, bound.wait_ms);
        if (bound.gpu_valid) {
          ImGui::Text("GPU: %.2f ms", bound.gpu_ms);
        } else {
          ImGui::TextUnformatted("GPU: not timed");
        }
        ImGui::Text("Frame: %.2f ms", bound.frame_ms);
      });
    }

    // Discord Rich Presence: reports the field area the player is currently
    // in, updated once per guest frame (area id read from byte_8244B500 and
    // translated through the cfdata name table). See src/room_presence.h/.cpp.
    auto* ks = rex::system::kernel_state();
    eternalsonata::GetRoomPresence().Bind(ks, runtime());

    // Party state for mods: EternalSonataAddCharacterToParty and the rest of
    // src/eternalsonata_party_api.h answer "unavailable" until this runs.
    eternalsonata::BindPartySystem(runtime());

    // Battle state for mods: src/eternalsonata_battle_api.h answers
    // "unavailable" until this runs.
    eternalsonata::BindBattleSystem(runtime());

    // Save state for mods: src/eternalsonata_save_api.h answers "unavailable"
    // and publishes no save event until this runs.
    eternalsonata::BindSaveSystem(runtime());

    // Photo album for mods: src/eternalsonata_photo_api.h answers
    // "unavailable" and publishes no photo event until this runs.
    eternalsonata::BindPhotoSystem(runtime());

    // Debug tool: force-loads a field area via the F4 settings overlay's
    // "Force Load Area..." button. See force_load_area.h.
    eternalsonata::GetForceLoadArea().Bind(runtime());

    // Debug tool: per-frame override of the field leader's model handle,
    // driven by the same overlay's "Overworld Model" combo.
    eternalsonata::FieldPlayerModelOverride::Bind(runtime());

    WireShaderDebugger();

    // Guest-side frame profiler, shown and hidden by F3 alongside the SDK's own
    // debug overlay: that one says how fast the frame is, this one says why.
    // Constructed unconditionally because constructing an ImGuiDialog is what
    // registers it for input; it draws nothing, and samples nothing, until F3
    // turns it on. See src/guest_profiler.h.
    guest_profiler_overlay_ = eternalsonata::CreateGuestProfilerOverlay(imgui_drawer());

    // On-screen pad. The SDK's touch driver reports no device until a layout
    // is installed, so this is what turns the touch controls on for this game;
    // the touch_controls cvar (on by default only on Android) still decides
    // whether they are shown. See src/touch_layout.h.
    if (auto* input_sys = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      if (auto* touch = input_sys->GetDriver<rex::input::touch::TouchInputDriver>()) {
        touch->SetLayoutProvider(&eternalsonata::BuildTouchLayout);
      }
    }
  }

  // The host swap chain follows the window. Only records the request: this
  // arrives on the UI thread, and every other swap chain call happens on the
  // guest thread at present time.
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    eternalsonata::PlumeNotifyResize(pixel_width, pixel_height);
  }

  void OnShutdown() override {
    // Tear the host renderer down before the window goes away.
    eternalsonata::ShutdownPlumeBackend();

    // Stop the Discord presence worker and clear the presence on exit. The
    // SDK only ships the RPC implementation for Windows and GNU Linux.
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
    rex::discord_rpc::Stop();
#endif

    // Hand the process-wide timer tick back to the host.
    eternalsonata::ReleaseHostTimerResolution();
  }

  std::unique_ptr<rex::ui::ImGuiDialog> OnCreateUserSettingsOverlay() override {
    return eternalsonata::CreateSettingsDialog(
        imgui_drawer(), window(), user_settings_path(), config_path(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }

 private:
  // Points the SDK's F2 shader debugger at the native renderer instead of at
  // the emulated command processor, which does not exist in this build. The
  // overlay's whole data model is "a list of shaders, by hash"; this renderer's
  // shaders are the closed set in guest_shaders.bin, addressed by guest table
  // slot, so the override presents a slot where a hash would go. See
  // src/native_renderer_shader_debug.h.
  void WireShaderDebugger() {
    ShaderDebuggerOverride override;
    override.snapshot_provider = [] { return eternalsonata::GuestShaderSnapshot(); };
    override.details_provider = [](uint64_t id) { return eternalsonata::GuestShaderDetails(id); };
    override.disable_setter = [](uint64_t id, bool disabled) {
      eternalsonata::SetGuestShaderDisabled(id, disabled);
    };
    // Replacing a compiled blob under a live pipeline is not offered: the
    // pipeline cache hands out objects that the frame's command list only reads
    // at execute time, so swapping one mid-frame would retroactively change
    // draws already recorded against it. Leaving the callback unset makes the
    // dialog's "Load binary..." fail loudly rather than corrupt a frame.
    override.profiling_toggle = [](bool enabled) {
      eternalsonata::SetGuestShaderProfiling(enabled);
    };
    override.profiling_resetter = [] { eternalsonata::ResetGuestShaderProfiling(); };
    SetShaderDebuggerOverride(std::move(override));

    // The dialog persists the disable flags, but only applies them as the user
    // toggles a row. Reading the same file here means a shader switched off in
    // a previous session is off from the first frame, without the overlay ever
    // being opened.
    eternalsonata::SetGuestShaderBlacklist(
        rex::ui::ShaderDebuggerDialog::ReadShaderBlacklistFromToml(runtime()->ModDumpRoot() /
                                                                  "shaders.toml"));
  }

  std::filesystem::path user_settings_path() const { return user_data_root() / "settings.toml"; }

  // F3's guest-side half. Owned here so it lives as long as the drawer does.
  std::unique_ptr<rex::ui::ImGuiDialog> guest_profiler_overlay_;

  // Guest frame present count, bumped by the per-swap callback (any thread).
  std::atomic<uint64_t> guest_swap_count_{0};
  // Trailing window of (wall time, swap count) samples for the F3 "Guest" FPS
  // line, keeping only samples within kStatsWindowSec (ImGui thread only).
  static constexpr double kStatsWindowSec = 1.0;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> stats_samples_;
};