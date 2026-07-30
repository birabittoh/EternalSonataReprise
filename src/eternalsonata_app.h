// eternalsonata - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/system/game_data_selector.h>
#include <rex/ui/window.h>
#include <rex/version.h>

#include "icon.generated.h"
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

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256 = "91184E7765172A358ECAA6E5CA1784DB1AE796C60F25051A45C5206F8949501E";
    settings.config_path = config_path();

    return rex::system::GameDataSelector::EnsureGameData(settings);
  }

  void OnPreSetup(rex::RuntimeConfig& /*config*/) override {}

  void OnPostSetup() override {
    window()->SetIcon(eternalsonata::kIconPNG, eternalsonata::kIconPNGSize);
    window()->SetTitle("Eternal Sonata: Reprise " + std::string(REXGLUE_BUILD_TITLE));
  }

  void OnShutdown() override {
  }

  std::unique_ptr<rex::ui::ImGuiDialog> OnCreateUserSettingsOverlay() override {
    return eternalsonata::CreateSettingsDialog(
        imgui_drawer(), window(), user_settings_path(), config_path(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }

 private:
  std::filesystem::path user_settings_path() const { return user_data_root() / "settings.toml"; }
};