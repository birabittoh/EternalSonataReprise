// eternalsonata - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/rex_app.h>
#include <rex/system/game_data_selector.h>
#include <rex/ui/window.h>

#include "icon.generated.h"

class EternalsonataApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<EternalsonataApp>(new EternalsonataApp(ctx, "eternalsonata", PPCImageConfig));
  }

  bool SetupEnvironment() override {
    if (!rex::ReXApp::SetupEnvironment())
      return false;

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256 = "91184E7765172A358ECAA6E5CA1784DB1AE796C60F25051A45C5206F8949501E";

    return rex::system::GameDataSelector::EnsureGameData(settings);
  }

  void OnPreSetup(rex::RuntimeConfig& /*config*/) override {}

  void OnPostSetup() override {
    window()->SetIcon(eternalsonata::kIconPNG, eternalsonata::kIconPNGSize);
  }

  void OnShutdown() override {
  }
};