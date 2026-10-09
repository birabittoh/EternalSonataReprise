#include "intro_screen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "images.generated.h"
#include "loading_screen.h"
#include "native_renderer_plume.h"

namespace eternalsonata {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kFadeSeconds = 1.6f;

// The progress theme's brown, with its purple as the accent.
constexpr ImU32 kTop = IM_COL32(0x2C, 0x1A, 0x0B, 255);
constexpr ImU32 kBottom = IM_COL32(0x0E, 0x08, 0x03, 255);
constexpr ImVec4 kAccent(0x9B / 255.0f, 0x59 / 255.0f, 0xB6 / 255.0f, 1.0f);
constexpr ImVec4 kText(0.96f, 0.93f, 0.88f, 1.0f);
constexpr ImVec4 kError(1.0f, 0.55f, 0.50f, 1.0f);

rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_context = nullptr;
std::optional<Clock::time_point> g_first_shown;

struct Art {
  std::unique_ptr<rex::ui::ImmediateTexture> texture;
  float width = 0.0f;
  float height = 0.0f;
  bool tried = false;

  ImTextureID id() const { return texture ? reinterpret_cast<ImTextureID>(texture.get()) : ImTextureID{}; }
};

Art g_background;
Art g_logo;

// Art from res/ by file stem; missing art just draws nothing.
void Load(Art& art, const char* name) {
  if (art.tried)
    return;
  art.tried = true;
  for (const ResImage* image = kResImages; image->name; ++image) {
    if (std::strcmp(image->name, name) != 0)
      continue;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(image->data, image->size, w, h);
    auto* immediate = g_drawer->immediate_drawer();
    if (!immediate || rgba.empty()) {
      REXLOG_WARN("Intro: res/{}.png did not decode", name);
      return;
    }
    art.texture = immediate->CreateTexture(uint32_t(w), uint32_t(h),
                                           rex::ui::ImmediateTextureFilter::kLinear, false,
                                           rgba.data());
    art.width = float(w);
    art.height = float(h);
    return;
  }
}

ImFont* FindFont(const char* name) {
  for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
    if (std::strcmp(font->GetDebugName(), name) == 0)
      return font;
  }
  return ImGui::GetFont();
}

ImU32 Color(const ImVec4& c, float alpha = 1.0f) {
  return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

float Smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Centered, word wrapped; returns the height used.
float CenteredText(ImDrawList* draw, ImFont* font, float size, float center_x, float y,
                   float wrap, ImU32 color, const std::string& text) {
  const char* begin = text.c_str();
  const char* end = begin + text.size();
  float line_y = y;
  while (begin < end) {
    const char* newline = static_cast<const char*>(std::memchr(begin, '\n', size_t(end - begin)));
    const char* stop = newline ? newline : end;
    const char* wrap_at = font->CalcWordWrapPosition(size, begin, stop, wrap);
    if (wrap_at == begin)
      wrap_at = stop;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, wrap_at);
    draw->AddText(font, size, ImVec2(std::floor(center_x - extent.x * 0.5f), line_y), color, begin,
                  wrap_at);
    line_y += size * 1.3f;
    begin = wrap_at;
    while (begin < stop && *begin == ' ')
      ++begin;
    if (begin == stop && newline)
      ++begin;
  }
  return line_y - y;
}

// Image scaled to cover the whole rect, cropped evenly.
void Cover(ImDrawList* draw, const Art& art, ImVec2 size, ImU32 tint) {
  const float scale = std::max(size.x / art.width, size.y / art.height);
  const float u = std::min(1.0f, size.x / (art.width * scale));
  const float v = std::min(1.0f, size.y / (art.height * scale));
  draw->AddImage(art.id(), ImVec2(0, 0), size, ImVec2((1 - u) * 0.5f, (1 - v) * 0.5f),
                 ImVec2((1 + u) * 0.5f, (1 + v) * 0.5f), tint);
}

// Slow rising motes, for when there is no background art and to give one life.
void Motes(ImDrawList* draw, ImVec2 size, float unit, float t, float alpha) {
  constexpr int kCount = 36;
  for (int i = 0; i < kCount; ++i) {
    const float seed = float(i) * 12.9898f;
    const float rx = std::fmod(std::sin(seed) * 43758.5453f, 1.0f);
    const float ry = std::fmod(std::sin(seed * 1.7f) * 24634.6345f, 1.0f);
    const float speed = 0.015f + 0.02f * std::fabs(ry);
    const float x = std::fabs(rx) * size.x + std::sin(t * 0.3f + seed) * 18.0f * unit;
    const float phase = std::fmod(std::fabs(ry) + t * speed, 1.0f);
    const float y = size.y * (1.1f - phase * 1.2f);
    const float glow = std::sin(phase * 3.14159f) * (0.5f + 0.5f * std::sin(t * 1.3f + seed));
    const float r = (1.5f + 2.5f * std::fabs(rx)) * unit;
    draw->AddCircleFilled(ImVec2(x, y), r * 3.0f, IM_COL32(255, 220, 160, int(18 * glow * alpha)));
    draw->AddCircleFilled(ImVec2(x, y), r, IM_COL32(255, 240, 210, int(150 * glow * alpha)));
  }
}

class IntroDialog final : public rex::ui::ImGuiDialog {
 public:
  IntroDialog(rex::ui::ImGuiDrawer* drawer, const GameDataPrompt& prompt)
      : rex::ui::ImGuiDialog(drawer), prompt_(prompt) {
    selected_ = prompt.ready ? 0 : 1;
    options_.push_back({"Start", GameDataChoice::kStart});
    options_.push_back({"Select Disc Image", GameDataChoice::kDiscImage});
    if (prompt.can_pick_folder)
      options_.push_back({"Select Folder", GameDataChoice::kFolder});
    options_.push_back({"Quit", GameDataChoice::kQuit});
  }

  void Update(const GameDataPrompt& prompt) {
    // Start is first in the list; land on it once it can be used.
    if (prompt.ready && !prompt_.ready)
      selected_ = 0;
    else if (!prompt.ready && selected_ == 0)
      selected_ = 1;
    prompt_ = prompt;
  }
  void SetBusy(bool busy) { busy_ = busy; }
  void SetWorking(bool working) {
    working_ = working;
    if (working)
      phases_.fill(PhaseState::kPending);
  }
  void SetPhase(Phase phase, PhaseState state) { phases_[size_t(phase)] = state; }
  void SetProgress(const std::string& title, float fraction, const std::string& detail) {
    work_title_ = title;
    work_fraction_ = fraction;
    work_detail_ = detail;
  }

  std::optional<GameDataChoice> TakeChoice() {
    auto choice = choice_;
    choice_.reset();
    return choice;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    Load(g_background, "background");
    Load(g_logo, "logo");

    const ImVec2 size = io.DisplaySize;
    const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
    const float shown = Smooth(t / kFadeSeconds);
    // Laid out for 1280x720 and scaled to the window.
    const float unit = std::min(size.x / 1280.0f, size.y / 720.0f);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##intro", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, kTop, kTop, kBottom, kBottom);
    if (g_background.texture)
      Cover(draw, g_background, size, IM_COL32_WHITE);
    // Darkens the lower half the text sits on.
    draw->AddRectFilledMultiColor(ImVec2(0, size.y * 0.35f), size, IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190),
                                  IM_COL32(0, 0, 0, 190));
    Motes(draw, size, unit, t, shown);

    float y = 46.0f * unit;
    if (g_logo.texture) {
      const float w = std::min(620.0f * unit, size.x * 0.9f);
      const float h = w * g_logo.height / g_logo.width;
      const float rise = (1.0f - Smooth((t - 0.2f) / 1.2f)) * 16.0f * unit;
      const ImVec2 min((size.x - w) * 0.5f, y + rise);
      draw->AddImage(g_logo.id(), min, ImVec2(min.x + w, min.y + h), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, int(255 * Smooth((t - 0.2f) / 1.2f))));
      y += h + 22.0f * unit;
    } else {
      y += 220.0f * unit;
    }

    const float body_in = Smooth((t - 0.7f) / 0.8f);
    ImFont* font = FindFont(kLoadingScreenFontName);
    const float wrap = std::min(820.0f * unit, size.x - 32.0f);
    if (working_) {
      y += DrawWork(draw, font, size, unit, y, wrap, body_in);
    } else {
      std::string body;
      if (prompt_.ready) {
        body = "The game files are ready.";
      } else {
        body = "Eternal Sonata Reprise needs the game files from your own copy of the game.\n";
        body += prompt_.can_pick_folder
                    ? "Select an Xbox 360 disc image to extract them, or a folder with a PS3 disc "
                      "dump or files you already extracted."
                    : "Select an Xbox 360 disc image to extract them.";
        if (!prompt_.copy_hint.empty())
          body += "\nAlready extracted? Copy them to " + prompt_.copy_hint;
      }
      y += CenteredText(draw, font, 22.0f * unit, size.x * 0.5f, y, wrap, Color(kText, body_in),
                        body);
      if (!prompt_.error.empty()) {
        y += 8.0f * unit;
        y += CenteredText(draw, font, 20.0f * unit, size.x * 0.5f, y, wrap,
                          Color(kError, body_in), prompt_.error);
      }
    }

    DrawButtons(draw, font, size, unit, std::max(y + 22.0f * unit, size.y * 0.62f),
                body_in);

    // Fade from black on the first showing only; a cancelled dialog comes back
    // to the screen as it was.
    if (shown < 1.0f)
      ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), size,
                                                    IM_COL32(0, 0, 0, int(255 * (1.0f - shown))));
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  struct Option {
    const char* label;
    GameDataChoice choice;
  };

  // The phase row and the current item's progress; returns the height used.
  float DrawWork(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float top, float wrap,
                 float alpha) {
    const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
    const float label_size = 20.0f * unit;
    const float step = std::min(220.0f * unit, size.x / float(kInstallPhaseCount));
    const float row_x = (size.x - step * float(kInstallPhaseCount)) * 0.5f;
    for (int i = 0; i < kInstallPhaseCount; ++i) {
      const PhaseState state = phases_[size_t(i)];
      const char* name = PhaseName(Phase(i));
      const ImVec2 extent = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, name);
      const float cx = row_x + step * (float(i) + 0.5f);
      const float r = 6.0f * unit;
      const ImVec2 dot(cx, top + r);
      if (state == PhaseState::kActive) {
        const float pulse = 0.65f + 0.35f * std::sin(t * 4.0f);
        draw->AddCircleFilled(dot, r * 1.9f, Color(kAccent, 0.25f * pulse * alpha));
        draw->AddCircleFilled(dot, r, Color(kAccent, alpha));
      } else if (state == PhaseState::kDone) {
        draw->AddCircleFilled(dot, r, Color(kText, 0.85f * alpha));
      } else if (state == PhaseState::kSkipped) {
        draw->AddLine(ImVec2(dot.x - r, dot.y), ImVec2(dot.x + r, dot.y),
                      Color(kText, 0.35f * alpha), std::max(1.0f, 2.0f * unit));
      } else {
        draw->AddCircle(dot, r, Color(kText, 0.45f * alpha), 0, std::max(1.0f, 1.5f * unit));
      }
      const float text_alpha = state == PhaseState::kActive   ? 1.0f
                               : state == PhaseState::kDone   ? 0.85f
                                                              : 0.4f;
      draw->AddText(font, label_size, ImVec2(std::floor(cx - extent.x * 0.5f), top + 2.4f * r),
                    Color(kText, text_alpha * alpha), name);
    }
    float used = 2.4f * 6.0f * unit + label_size + 18.0f * unit;

    const float bar_w = std::min(560.0f * unit, size.x - 32.0f);
    const float bar_h = 10.0f * unit;
    const ImVec2 min((size.x - bar_w) * 0.5f, top + used);
    const ImVec2 max(min.x + bar_w, min.y + bar_h);
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int(140 * alpha)), bar_h * 0.5f);
    if (work_fraction_ >= 0.0f && work_fraction_ <= 1.0f) {
      draw->AddRectFilled(min, ImVec2(min.x + bar_w * work_fraction_, max.y), Color(kAccent, alpha),
                          bar_h * 0.5f);
    } else {
      // Unknown length: a segment sweeping along.
      const float seg = bar_w * 0.25f;
      const float at = (std::fmod(t * 0.6f, 1.0f)) * (bar_w + seg) - seg;
      const float x0 = std::max(0.0f, at), x1 = std::min(bar_w, at + seg);
      if (x1 > x0)
        draw->AddRectFilled(ImVec2(min.x + x0, min.y), ImVec2(min.x + x1, max.y),
                            Color(kAccent, alpha), bar_h * 0.5f);
    }
    used += bar_h + 14.0f * unit;
    if (!work_title_.empty())
      used += CenteredText(draw, font, 22.0f * unit, size.x * 0.5f, top + used, wrap,
                           Color(kText, alpha), work_title_);
    if (!work_detail_.empty())
      used += CenteredText(draw, font, 18.0f * unit, size.x * 0.5f, top + used, wrap,
                           Color(kText, 0.6f * alpha), work_detail_);
    return used;
  }

  void DrawButtons(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float top, float alpha) {
    const int count = int(options_.size());
    // Ready once faded in, and not while a native dialog is open.
    const bool ready = alpha >= 1.0f && !busy_ && !working_;
    auto pressed = [](std::initializer_list<ImGuiKey> keys) {
      return std::any_of(keys.begin(), keys.end(), [](ImGuiKey k) { return ImGui::IsKeyPressed(k); });
    };
    // Start stays out of reach until every phase is done.
    auto enabled = [&](int i) {
      return options_[size_t(i)].choice != GameDataChoice::kStart || prompt_.ready;
    };
    auto step = [&](int direction) {
      do {
        selected_ = (selected_ + direction + count) % count;
      } while (!enabled(selected_));
    };
    if (ready && pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown}))
      step(1);
    if (ready && pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp}))
      step(-1);
    if (ready && pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                          ImGuiKey_GamepadFaceDown}))
      choice_ = options_[size_t(selected_)].choice;
    if (ready && pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}))
      choice_ = GameDataChoice::kQuit;

    const float w = 340.0f * unit;
    const float h = 50.0f * unit;
    const float gap = 12.0f * unit;
    const float x = (size.x - w) * 0.5f;
    const float label_size = 24.0f * unit;
    for (int i = 0; i < count; ++i) {
      const ImVec2 min(x, top + float(i) * (h + gap));
      const ImVec2 max(min.x + w, min.y + h);
      ImGui::SetCursorScreenPos(min);
      ImGui::PushID(i);
      const bool usable = enabled(i);
      if (ImGui::InvisibleButton("##option", ImVec2(w, h)) && ready && usable)
        choice_ = options_[size_t(i)].choice;
      // Only a moving mouse selects, so a resting cursor does not undo the keys.
      const ImVec2 delta = ImGui::GetIO().MouseDelta;
      if (ready && usable && ImGui::IsItemHovered() && (delta.x != 0.0f || delta.y != 0.0f))
        selected_ = i;
      ImGui::PopID();

      const bool active = i == selected_ && usable;
      const float rounding = 6.0f * unit;
      // Disabled while working, and Start before it is ready.
      const float a = alpha * (usable && !working_ && !busy_ ? 1.0f : 0.4f);
      draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((active ? 150 : 100) * a)), rounding);
      draw->AddRect(min, max, active ? Color(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                    rounding, 0, std::max(1.0f, (active ? 2.0f : 1.0f) * unit));
      const char* label = options_[size_t(i)].label;
      const ImVec2 extent = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
      draw->AddText(font, label_size,
                    ImVec2(std::floor(min.x + (w - extent.x) * 0.5f),
                           std::floor(min.y + (h - extent.y) * 0.5f)),
                    Color(kText, a * (active ? 1.0f : 0.8f)), label);
    }
  }

  GameDataPrompt prompt_;
  std::vector<Option> options_;
  int selected_ = 0;
  bool busy_ = false;
  bool working_ = false;
  std::array<PhaseState, kInstallPhaseCount> phases_{};
  std::string work_title_;
  std::string work_detail_;
  float work_fraction_ = -1.0f;
  std::optional<GameDataChoice> choice_;
};

std::unique_ptr<IntroDialog> g_dialog;
std::function<void(GameDataChoice)> g_on_choice;

// The main loop sleeps until an event arrives, so frames are posted to it.
// One at a time: a busy UI thread (extraction) must not pile them up.
std::thread g_ticker;
std::atomic<bool> g_ticking{false};
std::atomic<bool> g_frame_queued{false};

void Answer(GameDataChoice choice) {
  if (auto on_choice = g_on_choice)
    on_choice(choice);
}

void PresentFrame() {
  g_frame_queued = false;
  if (!g_dialog)
    return;
  PlumePresentOverlayOnly();
  // Taken after the frame, so answering can destroy the dialog safely.
  if (g_dialog) {
    if (const auto choice = g_dialog->TakeChoice())
      Answer(*choice);
  }
}

void StartTicker() {
  if (g_ticking.exchange(true))
    return;
  g_ticker = std::thread([] {
    while (g_ticking.load(std::memory_order_acquire)) {
      if (!g_frame_queued.exchange(true))
        g_context->CallInUIThreadDeferred(PresentFrame);
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  });
}

void StopTicker() {
  if (!g_ticking.exchange(false))
    return;
  g_ticker.join();
}

}  // namespace

void BindIntroScreen(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context) {
  g_drawer = drawer;
  g_context = context;
}

bool IntroScreenAvailable() {
  return g_drawer && g_context && PlumeBackendReady();
}

void ShowIntroScreen(const GameDataPrompt& prompt, std::function<void(GameDataChoice)> on_choice) {
  g_on_choice = std::move(on_choice);
  if (g_dialog) {
    g_dialog->Update(prompt);
    g_dialog->SetBusy(false);
    return;
  }
  if (!g_first_shown)
    g_first_shown = Clock::now();
  g_dialog = std::make_unique<IntroDialog>(g_drawer, prompt);
  StartTicker();
}

void SetIntroBusy(bool busy) {
  if (g_dialog)
    g_dialog->SetBusy(busy);
}

void BeginIntroWork() {
  if (g_dialog)
    g_dialog->SetWorking(true);
}

void EndIntroWork() {
  if (g_dialog)
    g_dialog->SetWorking(false);
}

void SetIntroPhase(Phase phase, PhaseState state) {
  if (g_dialog)
    g_dialog->SetPhase(phase, state);
}

void ReportIntroProgress(const std::string& title, float fraction, const std::string& detail) {
  if (!g_dialog)
    return;
  g_dialog->SetProgress(title, fraction, detail);
  static Clock::time_point last;
  const auto now = Clock::now();
  if (now - last < std::chrono::milliseconds(33))
    return;
  last = now;
  // Without this the OS marks the window unresponsive, Android never learns the
  // surface went away, and the screen never follows a resize.
  g_context->ProcessPendingWindowEvents();
  PlumePresentOverlayOnly();
}

void HideIntroScreen() {
  StopTicker();
  if (!g_dialog)
    return;
  g_dialog.reset();
  g_on_choice = nullptr;
  // One more frame, so the intro is gone behind whatever comes next.
  PlumePresentOverlayOnly();
}

bool RequestIntroQuit() {
  if (!g_dialog)
    return false;
  // Deferred: this runs inside the window's own event handling.
  g_context->CallInUIThreadDeferred([] { Answer(GameDataChoice::kQuit); });
  return true;
}

void ReleaseIntroScreen() {
  g_background = {};
  g_logo = {};
}

}  // namespace eternalsonata
