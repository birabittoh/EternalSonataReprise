#include "loading_screen.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

#include <imgui.h>

#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/progress_window.h>
#include <rex/ui/windowed_app_context.h>

#include "icon.generated.h"
#include "native_renderer_plume.h"
#include "progress_theme.h"

namespace eternalsonata {
namespace {

ImU32 Rgb(const uint8_t (&c)[3]) { return IM_COL32(c[0], c[1], c[2], 255); }

ImFont* FindFont(const char* name) {
  for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
    if (std::strcmp(font->GetDebugName(), name) == 0)
      return font;
  }
  return ImGui::GetFont();
}

class LoadingDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit LoadingDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

  void Set(const std::string& title, float fraction, const std::string& detail) {
    title_ = title;
    fraction_ = fraction;
    detail_ = detail;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const rex::ui::ProgressWindowTheme theme = ProgressTheme();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 size = io.DisplaySize;
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, Rgb(theme.background));

    // Laid out for 1280x720 and scaled to whatever the window is.
    const float unit = std::min(size.x / 1280.0f, size.y / 720.0f);
    const float bar_w = 640.0f * unit;
    const float bar_h = 22.0f * unit;
    const float bar_x = (size.x - bar_w) * 0.5f;
    const float bar_y = size.y * 0.5f + 60.0f * unit;

    if (ImTextureID icon = Icon()) {
      const float icon_size = 128.0f * unit;
      const ImVec2 min((size.x - icon_size) * 0.5f, bar_y - 90.0f * unit - icon_size);
      draw->AddImage(icon, min, ImVec2(min.x + icon_size, min.y + icon_size));
    }

    ImFont* font = FindFont(kLoadingScreenFontName);
    const float title_size = 36.0f * unit;
    const float detail_size = 20.0f * unit;
    draw->AddText(font, title_size, ImVec2(bar_x, bar_y - title_size - 16.0f * unit),
                  Rgb(theme.title_text), title_.c_str());
    draw->AddText(font, detail_size, ImVec2(bar_x, bar_y + bar_h + 14.0f * unit),
                  Rgb(theme.detail_text), detail_.c_str());

    const float border = std::max(1.0f, 2.0f * unit);
    draw->AddRect(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
                  Rgb(theme.bar_frame), 0.0f, 0, border);
    if (fraction_ >= 0.0f && fraction_ <= 1.0f) {
      const float inset = border * 2.0f;
      draw->AddRectFilled(
          ImVec2(bar_x + inset, bar_y + inset),
          ImVec2(bar_x + inset + (bar_w - inset * 2.0f) * fraction_, bar_y + bar_h - inset),
          Rgb(theme.bar_fill));
    }
  }

 private:
  // Created on first draw: the immediate drawer only has a device once the
  // native renderer is up, which is guaranteed by the time a frame is drawn.
  ImTextureID Icon() {
    if (!icon_ && !icon_failed_) {
      icon_failed_ = true;
      int w = 0, h = 0;
      std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(kIconPNG, kIconPNGSize, w, h);
      if (auto* immediate = imgui_drawer()->immediate_drawer(); immediate && !rgba.empty()) {
        icon_ = immediate->CreateTexture(uint32_t(w), uint32_t(h),
                                         rex::ui::ImmediateTextureFilter::kLinear, false,
                                         rgba.data());
        icon_failed_ = !icon_;
      }
    }
    return icon_ ? reinterpret_cast<ImTextureID>(icon_.get()) : ImTextureID{};
  }

  std::string title_;
  std::string detail_;
  float fraction_ = -1.0f;
  std::unique_ptr<rex::ui::ImmediateTexture> icon_;
  bool icon_failed_ = false;
};

rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_context = nullptr;
std::unique_ptr<LoadingDialog> g_dialog;
std::chrono::steady_clock::time_point g_last_frame;

}  // namespace

void BindLoadingScreen(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context) {
  g_drawer = drawer;
  g_context = context;
}

void UpdateLoadingScreen(const std::string& title, float fraction, const std::string& detail,
                         bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (g_dialog && !force && now - g_last_frame < std::chrono::milliseconds(33))
    return;
  g_last_frame = now;

  // Without this the OS marks the window unresponsive, Android never learns the
  // surface went away, and the screen never follows a resize.
  if (g_context)
    g_context->ProcessPendingWindowEvents();
  else
    rex::ui::PumpPlatformEvents();
  if (!g_drawer || !PlumeBackendReady())
    return;
  if (!g_dialog)
    g_dialog = std::make_unique<LoadingDialog>(g_drawer);
  g_dialog->Set(title, fraction, detail);
  PlumePresentOverlayOnly();
}

void HideLoadingScreen() { g_dialog.reset(); }

}  // namespace eternalsonata
