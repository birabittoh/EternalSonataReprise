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
#include "host_timer_resolution.h"
#include "icon.generated.h"
#include "party_system.h"
#include "room_presence.h"
#include "save_system.h"
#include "settings.h"

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
  }

  void OnPostSetup() override {
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

    // Debug tool: force-loads a field area via the F4 settings overlay's
    // "Force Load Area..." button. See force_load_area.h.
    eternalsonata::GetForceLoadArea().Bind(runtime());

    // Debug tool: per-frame override of the field leader's model handle,
    // driven by the same overlay's "Overworld Model" combo.
    eternalsonata::FieldPlayerModelOverride::Bind(runtime());
  }

  void OnShutdown() override {
    // Stop the Discord presence worker and clear the presence on exit.
    rex::discord_rpc::Stop();

    // Hand the process-wide timer tick back to the host.
    eternalsonata::ReleaseHostTimerResolution();
  }

  std::unique_ptr<rex::ui::ImGuiDialog> OnCreateUserSettingsOverlay() override {
    return eternalsonata::CreateSettingsDialog(
        imgui_drawer(), window(), user_settings_path(), config_path(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }

 private:
  std::filesystem::path user_settings_path() const { return user_data_root() / "settings.toml"; }

  // Guest frame present count, bumped by the per-swap callback (any thread).
  std::atomic<uint64_t> guest_swap_count_{0};
  // Trailing window of (wall time, swap count) samples for the F3 "Guest" FPS
  // line, keeping only samples within kStatsWindowSec (ImGui thread only).
  static constexpr double kStatsWindowSec = 1.0;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> stats_samples_;
};