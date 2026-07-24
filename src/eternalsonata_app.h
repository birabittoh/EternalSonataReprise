// eternalsonata - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/rex_app.h>

class EternalsonataApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<EternalsonataApp>(new EternalsonataApp(ctx, "eternalsonata", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& /*config*/) override {}

  void OnPostSetup() override {}

  void OnShutdown() override {
  }
};
