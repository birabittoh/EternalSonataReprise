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
#include "intro_music.h"
#include "ui_text.h"
#include "loading_screen.h"
#include "native_renderer_plume.h"
#include "release_id.h"

namespace eternalsonata {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kFadeSeconds = 1.6f;

// The progress theme's brown, with the logo's light blue as the accent.
constexpr ImU32 kTop = IM_COL32(0x2C, 0x1A, 0x0B, 255);
constexpr ImU32 kBottom = IM_COL32(0x0E, 0x08, 0x03, 255);
constexpr ImVec4 kAccent(0x3A / 255.0f, 0xAA / 255.0f, 0xDC / 255.0f, 1.0f);
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
    BuildOptions();
  }

  void Update(const GameDataPrompt& prompt) {
    // Land on Extract or Start once it can be used.
    const bool usable = prompt.ready || prompt.can_extract;
    if (prompt.confirm.empty() != prompt_.confirm.empty() ||
        prompt.can_delete != prompt_.can_delete) {
      prompt_ = prompt;
      BuildOptions();
      on_picker_ = picker_open_ = false;
      return;
    }
    if (usable && (prompt.ready != prompt_.ready || prompt.can_extract != prompt_.can_extract)) {
      selected_ = StartIndex();
      on_picker_ = picker_open_ = false;
    } else if (!usable && selected_ == StartIndex())
      selected_ = 0;
    prompt_ = prompt;
  }
  // Sources on the left, then what to do with them on the right. Extract
  // until the phases are done, then Start. Asking to delete has only a choice.
  void BuildOptions() {
    options_.clear();
    if (!prompt_.confirm.empty()) {
      options_.push_back({IntroText::kCancel, GameDataChoice::kCancel, 0, 0});
      options_.push_back({IntroText::kDeleteFiles, GameDataChoice::kConfirmDelete, 1, 0});
      selected_ = 0;
      return;
    }
    // Processed files are replaced by deleting them, not by picking a source.
    if (prompt_.can_delete) {
      options_.push_back({IntroText::kDeleteFiles, GameDataChoice::kDelete, 0, 0});
    } else {
      options_.push_back({IntroText::kSelectFile, GameDataChoice::kDiscImage, 0, 0});
      if (prompt_.can_pick_folder)
        options_.push_back({IntroText::kSelectFolder, GameDataChoice::kFolder, 0, 1});
    }
    options_.push_back({IntroText::kStart, GameDataChoice::kStart, 1, 0});
    options_.push_back({IntroText::kQuit, GameDataChoice::kQuit, 1, 1});
    selected_ = prompt_.ready || prompt_.can_extract ? StartIndex() : 0;
  }
  void SetBusy(bool busy) { busy_ = busy; }
  void SetWorking(bool working) {
    working_ = working;
    if (working)
      phases_.fill(PhaseState::kPending);
  }
  void SetPhase(Phase phase, PhaseState state) { phases_[size_t(phase)] = state; }
  void SetProgress(const std::string& title, float fraction, const std::string& detail) {
    work_title_ = TrProgress(title);
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
      if (!prompt_.confirm.empty()) {
        body = prompt_.confirm;
      } else if (prompt_.ready) {
        body = prompt_.release.empty() ? Tr(IntroText::kReady)
                                       : Tr(IntroText::kReadyRelease, prompt_.release);
      } else if (prompt_.can_extract) {
        body = (prompt_.release.empty() ? std::string(Tr(IntroText::kFound))
                                        : Tr(IntroText::kFoundRelease, prompt_.release)) +
               "\n" + Tr(IntroText::kPressExtract);
      } else {
        body = Tr(IntroText::kNeedFiles);
        if (!prompt_.can_pick_folder)
          body += std::string("\n") + Tr(IntroText::kSelectIsoOnly);
        if (!prompt_.copy_hint.empty())
          body += "\n" + Tr(IntroText::kCopyHint, prompt_.copy_hint);
      }
      y += CenteredText(draw, font, 22.0f * unit, size.x * 0.5f, y, wrap, Color(kText, body_in),
                        body);
      if (!prompt_.error.empty() && !prompt_.can_extract && !prompt_.release.empty() &&
          prompt_.error.find(prompt_.release) == std::string::npos) {
        y += 8.0f * unit;
        y += CenteredText(draw, font, 20.0f * unit, size.x * 0.5f, y, wrap,
                          Color(kText, body_in), Tr(IntroText::kSelected, prompt_.release));
      }
      if (!prompt_.error.empty()) {
        y += 8.0f * unit;
        y += CenteredText(draw, font, 20.0f * unit, size.x * 0.5f, y, wrap,
                          Color(kError, body_in), prompt_.error);
      }
    }

    // Ready once faded in, and not while a native dialog is open.
    const bool ready = body_in >= 1.0f && !busy_ && !working_;
    // One key press moves focus once, not again in the picker.
    const bool handled = DrawButtons(draw, font, size, unit,
                                     std::max(y + 22.0f * unit, size.y * 0.62f), body_in, ready);
    DrawLanguagePicker(draw, font, size, unit, body_in, ready && !handled);
    DrawMusicToggle(draw, font, size, unit, body_in);

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
    IntroText label;
    GameDataChoice choice;
    int column;
    int row;
  };

  // Whether the picked release has text in language `index`; any does
  // before one is picked.
  bool Supported(int index) const {
    return !prompt_.languages ||
           (prompt_.languages & TextLanguageBit(IntroLanguageCodeAt(index))) != 0;
  }

  int StartIndex() const {
    for (size_t i = 0; i < options_.size(); ++i) {
      if (options_[i].choice == GameDataChoice::kStart)
        return int(i);
    }
    return 0;
  }

  // The phase row and the current item's progress; returns the height used.
  float DrawWork(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float top, float wrap,
                 float alpha) {
    const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
    const float label_size = 20.0f * unit;
    const float step = std::min(220.0f * unit, size.x / float(kInstallPhaseCount));
    const float row_x = (size.x - step * float(kInstallPhaseCount)) * 0.5f;
    for (int i = 0; i < kInstallPhaseCount; ++i) {
      const PhaseState state = phases_[size_t(i)];
      const char* name = TrPhase(Phase(i));
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

  static bool Pressed(std::initializer_list<ImGuiKey> keys) {
    return std::any_of(keys.begin(), keys.end(), [](ImGuiKey k) { return ImGui::IsKeyPressed(k); });
  }

  // Returns whether it handled a key.
  bool DrawButtons(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float top, float alpha,
                   bool ready) {
    const int count = int(options_.size());
    // Start stays out of reach until there is something to extract or start.
    auto enabled = [&](int i) {
      const GameDataChoice choice = options_[size_t(i)].choice;
      if (choice == GameDataChoice::kDelete)
        return prompt_.can_delete;
      return choice != GameDataChoice::kStart ||
             (prompt_.can_extract || (prompt_.ready && Supported(IntroLanguage())));
    };
    if (!enabled(selected_))
      selected_ = 0;
    auto choice_of = [&](int i) {
      const GameDataChoice choice = options_[size_t(i)].choice;
      return choice == GameDataChoice::kStart && !prompt_.ready ? GameDataChoice::kExtract
                                                                : choice;
    };
    auto at = [&](int column, int row) {
      for (int i = 0; i < count; ++i) {
        const Option& option = options_[size_t(i)];
        if (option.column == column && option.row == row && enabled(i))
          return i;
      }
      return -1;
    };
    // Up and down stay in a column, and below its last usable button is the
    // language picker; left and right keep the row, or take the other
    // column's remaining usable button.
    auto move = [&](int dx, int dy) {
      const Option& from = options_[size_t(selected_)];
      if (dy != 0) {
        const int row = from.row + dy;
        const int i = row >= 0 && row <= 2 ? at(from.column, row) : -1;
        if (i >= 0)
          selected_ = i;
        else if (dy > 0)
          on_picker_ = true;
        return;
      }
      const int column = (from.column + dx + 2) % 2;
      for (int i : {at(column, from.row), at(column, 1), at(column, 0)}) {
        if (i >= 0) {
          selected_ = i;
          return;
        }
      }
    };
    const bool keys = ready && !on_picker_ && !picker_open_;
    bool handled = true;
    if (keys && Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown}))
      move(0, 1);
    else if (keys && Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp}))
      move(0, -1);
    else if (keys && Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight}))
      move(1, 0);
    else if (keys && Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft}))
      move(-1, 0);
    else if (keys && Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                              ImGuiKey_GamepadFaceDown}))
      choice_ = choice_of(selected_);
    else if (keys && Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}))
      choice_ = prompt_.confirm.empty() ? GameDataChoice::kQuit : GameDataChoice::kCancel;
    else
      handled = false;

    const float w = 300.0f * unit;
    const float h = 50.0f * unit;
    const float gap = 12.0f * unit;
    const float column_gap = 24.0f * unit;
    const float left = (size.x - (2.0f * w + column_gap)) * 0.5f;
    const float label_size = 24.0f * unit;
    for (int i = 0; i < count; ++i) {
      const Option& option = options_[size_t(i)];
      const ImVec2 min(left + float(option.column) * (w + column_gap),
                       top + float(option.row) * (h + gap));
      const ImVec2 max(min.x + w, min.y + h);
      const bool usable = enabled(i);
      // Delete Files only shows where it can be used.
      if (option.choice == GameDataChoice::kDelete && !usable)
        continue;
      ImGui::SetCursorScreenPos(min);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##option", ImVec2(w, h)) && ready && usable && !picker_open_)
        choice_ = choice_of(i);
      // Only a moving mouse selects, so a resting cursor does not undo the keys.
      if (ready && usable && !picker_open_ && ImGui::IsItemHovered() && MouseMoved()) {
        selected_ = i;
        on_picker_ = false;
      }
      ImGui::PopID();

      const bool active = i == selected_ && usable && !on_picker_;
      const float rounding = 6.0f * unit;
      // Disabled while working, and Start before it is ready.
      const float a = alpha * (usable && !working_ && !busy_ ? 1.0f : 0.4f);
      // Extract pulses a soft halo
      const bool glow = usable && choice_of(i) == GameDataChoice::kExtract;
      const float t = std::chrono::duration<float>(Clock::now() - *g_first_shown).count();
      const float pulse = 0.55f + 0.45f * std::sin(t * 3.0f);
      if (glow) {
        constexpr int kLayers = 8;
        for (int layer = kLayers; layer >= 1; --layer) {
          const float grow = float(layer) * 2.5f * unit * (0.7f + 0.3f * pulse);
          const float fade = 1.0f - float(layer - 1) / float(kLayers);
          draw->AddRectFilled(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow),
                              Color(kAccent, 0.09f * fade * fade * pulse * a), rounding + grow);
        }
      }
      draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((active ? 150 : 100) * a)), rounding);
      const ImU32 border = active ? Color(kAccent, a)
                           : glow ? Color(kAccent, (0.5f + 0.5f * pulse) * a)
                                  : IM_COL32(255, 255, 255, int(60 * a));
      draw->AddRect(min, max, border, rounding, 0,
                    std::max(1.0f, (active || glow ? 2.0f : 1.0f) * unit));
      const char* label =
          Tr(choice_of(i) == GameDataChoice::kExtract ? IntroText::kExtract : option.label);
      const ImVec2 extent = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
      draw->AddText(font, label_size,
                    ImVec2(std::floor(min.x + (w - extent.x) * 0.5f),
                           std::floor(min.y + (h - extent.y) * 0.5f)),
                    Color(kText, a * (active ? 1.0f : 0.8f)), label);
      if (option.choice == GameDataChoice::kStart && !Supported(IntroLanguage()))
        DrawLanguageWarning(draw, font, size, unit, ImVec2(max.x, min.y), h, alpha);
    }
    return keys && handled;
  }

  static bool MouseMoved() {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    return delta.x != 0.0f || delta.y != 0.0f;
  }

  // Bottom right: the current language, opening upward into the list.
  void DrawLanguagePicker(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha,
                          bool ready) {
    const int count = IntroLanguageCount();
    if (count < 2)
      return;
    const int current = IntroLanguage();
    auto pick = [&](int index) {
      SetIntroLanguage(index);
      picker_open_ = false;
    };

    if (ready && picker_open_) {
      if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp}))
        picker_highlight_ = std::max(0, picker_highlight_ - 1);
      else if (Pressed({ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown}))
        picker_highlight_ = std::min(count - 1, picker_highlight_ + 1);
      else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                        ImGuiKey_GamepadFaceDown}))
        pick(picker_highlight_);
      else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}))
        picker_open_ = false;
    } else if (ready && on_picker_) {
      if (Pressed({ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp})) {
        on_picker_ = false;
      } else if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft})) {
        pick((current + count - 1) % count);
      } else if (Pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight})) {
        pick((current + 1) % count);
      } else if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                          ImGuiKey_GamepadFaceDown})) {
        picker_open_ = true;
        picker_highlight_ = current;
      } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
        choice_ = GameDataChoice::kQuit;
      }
    }

    const float w = 220.0f * unit;
    const float h = 40.0f * unit;
    const float margin = 24.0f * unit;
    const float rounding = 6.0f * unit;
    const float text_size = 20.0f * unit;
    const float pad = 14.0f * unit;
    const ImVec2 min(size.x - margin - w, size.y - margin - h);
    const ImVec2 max(min.x + w, min.y + h);
    const float a = alpha * (working_ || busy_ ? 0.4f : 1.0f);

    // A press outside the open list closes it.
    bool over_picker = false;
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##language", ImVec2(w, h)) && ready) {
      picker_open_ = !picker_open_;
      picker_highlight_ = current;
      on_picker_ = true;
    }
    over_picker |= ImGui::IsItemHovered();
    if (ready && !picker_open_ && ImGui::IsItemHovered() && MouseMoved())
      on_picker_ = true;

    const float caption = 16.0f * unit;
    draw->AddText(font, caption, ImVec2(min.x, min.y - caption * 1.4f), Color(kText, 0.6f * a),
                  Tr(IntroText::kLanguage));
    const bool focused = on_picker_ || picker_open_;
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((focused ? 150 : 100) * a)), rounding);
    draw->AddRect(min, max, focused ? Color(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                  rounding, 0, std::max(1.0f, (focused ? 2.0f : 1.0f) * unit));
    const char* name = IntroLanguageName(current);
    const float text_y = std::floor(min.y + (h - text_size) * 0.5f);
    draw->AddText(font, text_size, ImVec2(min.x + pad, text_y), Color(kText, a), name);
    // An arrow pointing the way the list opens.
    const float s = 5.0f * unit;
    const ImVec2 tip(max.x - pad - s, min.y + h * 0.5f - s * 0.5f);
    draw->AddTriangleFilled(ImVec2(tip.x - s, tip.y + s), ImVec2(tip.x + s, tip.y + s), tip,
                            Color(kText, 0.8f * a));

    if (picker_open_) {
      const float item_h = 36.0f * unit;
      const ImVec2 list_min(min.x, min.y - 4.0f * unit - item_h * float(count));
      const ImVec2 list_max(max.x, min.y - 4.0f * unit);
      draw->AddRectFilled(list_min, list_max, IM_COL32(16, 10, 4, int(235 * a)), rounding);
      draw->AddRect(list_min, list_max, Color(kAccent, a), rounding, 0, std::max(1.0f, unit));
      for (int i = 0; i < count; ++i) {
        const ImVec2 item_min(list_min.x, list_min.y + item_h * float(i));
        const ImVec2 item_max(list_max.x, item_min.y + item_h);
        ImGui::SetCursorScreenPos(item_min);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##language_item", ImVec2(w, item_h)) && ready)
          pick(i);
        if (ImGui::IsItemHovered()) {
          over_picker = true;
          if (MouseMoved())
            picker_highlight_ = i;
        }
        ImGui::PopID();
        if (i == picker_highlight_)
          draw->AddRectFilled(item_min, item_max, Color(kAccent, 0.45f * a), rounding);
        // Still selectable; the warning says why Start is off.
        const float shade = Supported(i) ? (i == current ? 1.0f : 0.8f) : 0.35f;
        draw->AddText(font, text_size,
                      ImVec2(item_min.x + pad, std::floor(item_min.y + (item_h - text_size) * 0.5f)),
                      Color(kText, shade * a), IntroLanguageName(i));
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !over_picker)
        picker_open_ = false;
    }
  }

  // Bottom left: a note icon that mutes the music, and the piece playing.
  void DrawMusicToggle(ImDrawList* draw, ImFont* font, ImVec2 size, float unit, float alpha) {
    const float s = 40.0f * unit;
    const float margin = 24.0f * unit;
    const ImVec2 min(margin, size.y - margin - s);
    const ImVec2 max(min.x + s, min.y + s);
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##music", ImVec2(s, s)))
      SetIntroMusicEnabled(!IntroMusicEnabled());
    const bool on = IntroMusicEnabled();
    const bool hovered = ImGui::IsItemHovered();
    const float a = alpha;
    draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, int((hovered ? 150 : 100) * a)), 6.0f * unit);
    draw->AddRect(min, max, hovered ? Color(kAccent, a) : IM_COL32(255, 255, 255, int(60 * a)),
                  6.0f * unit, 0, std::max(1.0f, (hovered ? 2.0f : 1.0f) * unit));

    const ImU32 ink = Color(kText, (on ? 0.9f : 0.55f) * a);
    // Two beamed eighth notes, symmetric about the button's center.
    const float u = s / 40.0f;
    const ImVec2 c(min.x + s * 0.5f, min.y + s * 0.5f + 1.4f * u);
    auto at = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };
    for (const float head : {-7.0f, 7.0f}) {
      draw->AddEllipseFilled(at(head, 7.0f), ImVec2(4.6f * u, 3.3f * u), ink, -0.45f);
      draw->AddLine(at(head + 3.5f, 6.0f), at(head + 3.5f, head < 0 ? -9.0f : -12.0f), ink,
                    std::max(1.5f, 2.0f * u));
    }
    const ImVec2 beam[4] = {at(-4.5f, -10.0f), at(11.5f, -13.5f), at(11.5f, -9.0f), at(-4.5f, -5.5f)};
    draw->AddConvexPolyFilled(beam, 4, ink);
    if (!on)
      draw->AddLine(at(-12.0f, 12.0f), at(12.0f, -14.0f), Color(kText, a), std::max(1.5f, 2.0f * u));

    // The credit slides out from behind the icon, and back in when muted.
    const float step = ImGui::GetIO().DeltaTime / 0.7f;
    credit_ = on ? std::min(1.0f, credit_ + step) : std::max(0.0f, credit_ - step);
    const float shown = Smooth(credit_);
    const float text_size = 18.0f * unit;
    const char* credit = "Nocturne Op. 9, No. 2, performed by Aya Higuchi";
    const float text_x = max.x + 14.0f * unit;
    const ImVec2 extent = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, credit);
    draw->PushClipRect(ImVec2(text_x, min.y), ImVec2(size.x, max.y), true);
    const int first = draw->VtxBuffer.Size;
    draw->AddText(font, text_size,
                  ImVec2(std::floor(text_x - (1.0f - shown) * (extent.x + 14.0f * unit)),
                         std::floor(min.y + (s - text_size) * 0.5f)),
                  Color(kText, 0.75f * a), credit);
    // The left edge fades into the icon while sliding, and is solid at rest.
    const float edge = std::max(1.0f, 48.0f * unit * (1.0f - shown));
    for (int i = first; shown < 1.0f && i < draw->VtxBuffer.Size; ++i) {
      ImDrawVert& v = draw->VtxBuffer[i];
      const float k = std::clamp((v.pos.x - text_x) / edge, 0.0f, 1.0f);
      v.col = (v.col & 0x00FFFFFF) | (ImU32(float(v.col >> 24) * k) << 24);
    }
    draw->PopClipRect();
  }

  // The reason Start is off, to the right of the Start button whose top right
  // corner is `start_max`.
  void DrawLanguageWarning(ImDrawList* draw, ImFont* font, ImVec2 size, float unit,
                           ImVec2 start_max, float h, float a) {
    const std::string text = Tr(IntroText::kUnsupportedLanguage, prompt_.release);
    const float text_size = 18.0f * unit;
    const float text_x = start_max.x + 16.0f * unit;
    const float wrap = std::max(size.x - text_x - 16.0f * unit, 120.0f * unit);
    const ImVec2 box = font->CalcTextSizeA(text_size, FLT_MAX, wrap, text.c_str());
    draw->AddText(font, text_size,
                  ImVec2(text_x, std::floor(start_max.y + (h - box.y) * 0.5f)),
                  Color(kText, a), text.c_str(), nullptr, wrap);
  }

  GameDataPrompt prompt_;
  std::vector<Option> options_;
  int selected_ = 0;
  // The language picker has the keys instead of selected_.
  bool on_picker_ = false;
  bool picker_open_ = false;
  int picker_highlight_ = 0;
  bool busy_ = false;
  bool working_ = false;
  float credit_ = 0.0f;
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
  StartIntroMusic();
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
  StopIntroMusic();
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
