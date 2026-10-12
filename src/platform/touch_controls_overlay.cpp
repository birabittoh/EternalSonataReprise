/**
 * @file        touch_controls_overlay.cpp
 * @brief       Touch pad overlay. See touch_controls_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "touch_controls_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>

REXCVAR_DECLARE(double, touch_opacity);

namespace eternalsonata::touch {

namespace {

// Held controls brighten
constexpr float kPressedBoost = 2.0f;

constexpr float kPi = 3.14159265f;

enum class FaceGlyph { kCross, kCircle, kSquare, kTriangle };

struct FaceStyle {
  ImVec4 color;
  const char* label;
  FaceGlyph glyph;
};

bool GetFaceStyle(uint16_t button, TouchFaceStyle face_style, FaceStyle* style) {
  const bool ps = face_style == TouchFaceStyle::kPlayStation;
  switch (button) {
    case X_INPUT_GAMEPAD_A:
      *style = ps ? FaceStyle{{0.490f, 0.651f, 0.949f, 1.0f}, "", FaceGlyph::kCross}
                  : FaceStyle{{0.369f, 0.682f, 0.227f, 1.0f}, "A", FaceGlyph::kCross};
      return true;
    case X_INPUT_GAMEPAD_B:
      *style = ps ? FaceStyle{{0.957f, 0.392f, 0.408f, 1.0f}, "", FaceGlyph::kCircle}
                  : FaceStyle{{0.761f, 0.231f, 0.231f, 1.0f}, "B", FaceGlyph::kCircle};
      return true;
    case X_INPUT_GAMEPAD_X:
      *style = ps ? FaceStyle{{0.902f, 0.553f, 0.792f, 1.0f}, "", FaceGlyph::kSquare}
                  : FaceStyle{{0.243f, 0.471f, 0.761f, 1.0f}, "X", FaceGlyph::kSquare};
      return true;
    case X_INPUT_GAMEPAD_Y:
      *style = ps ? FaceStyle{{0.282f, 0.788f, 0.643f, 1.0f}, "", FaceGlyph::kTriangle}
                  : FaceStyle{{0.847f, 0.635f, 0.118f, 1.0f}, "Y", FaceGlyph::kTriangle};
      return true;
    default:
      return false;
  }
}

ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
          a.w + (b.w - a.w) * t};
}

ImU32 ColorWithAlpha(ImVec4 color, float alpha) {
  color.w = std::clamp(alpha, 0.0f, 1.0f);
  return ImGui::GetColorU32(color);
}

ImU32 FillColor(float alpha, bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f), 1.0f);
  return ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, a));
}

ImU32 EdgeColor(float alpha, bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f) * 1.6f, 1.0f);
  return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a));
}

ImFont* FaceButtonFont() {
  for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
    if (std::strcmp(font->GetDebugName(), "Touch controls") == 0) {
      return font;
    }
  }
  return ImGui::GetFont();
}

void DrawLabel(ImDrawList* draw_list, const char* label, float cx, float cy, float alpha,
               bool pressed) {
  if (!label || !*label) {
    return;
  }
  // The base size, not the current one: the layout is in window pixels, so the
  // labels must not follow a host's global UI scale (style.FontScaleMain).
  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetStyle().FontSizeBase;
  const ImVec2 size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f) * 1.8f, 1.0f);
  draw_list->AddText(font, font_size, ImVec2(cx - size.x * 0.5f, cy - size.y * 0.5f),
                     ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a)), label);
}

float SmoothStep(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Fills an ellipse as one mesh of nested rings shrinking toward `focus`, so a
// translucent gradient covers each pixel once instead of stacking into bands.
// `shade(t, dir)` gives the colour at ring t (0 at the focus, 1 at the edge)
// in direction dir; a one pixel fringe fades the edge out like ImGui's own AA.
template <typename Shade>
void FillShadedEllipse(ImDrawList* draw_list, ImVec2 center, ImVec2 radii, ImVec2 focus,
                       Shade shade) {
  constexpr int kRings = 16;
  constexpr int kSegments = 48;
  constexpr int kLoops = kRings + 2;  // focus, rings, fringe
  draw_list->PrimReserve((kLoops - 1) * kSegments * 6, kLoops * kSegments);
  const ImDrawIdx base = ImDrawIdx(draw_list->_VtxCurrentIdx);
  for (int loop = 0; loop + 1 < kLoops; ++loop) {
    for (int s = 0; s < kSegments; ++s) {
      const int n = (s + 1) % kSegments;
      const ImDrawIdx a = ImDrawIdx(base + loop * kSegments + s);
      const ImDrawIdx b = ImDrawIdx(base + loop * kSegments + n);
      const ImDrawIdx c = ImDrawIdx(base + (loop + 1) * kSegments + n);
      const ImDrawIdx d = ImDrawIdx(base + (loop + 1) * kSegments + s);
      draw_list->PrimWriteIdx(a);
      draw_list->PrimWriteIdx(b);
      draw_list->PrimWriteIdx(c);
      draw_list->PrimWriteIdx(a);
      draw_list->PrimWriteIdx(c);
      draw_list->PrimWriteIdx(d);
    }
  }
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  for (int loop = 0; loop < kLoops; ++loop) {
    const bool fringe = loop == kLoops - 1;
    const float t = fringe ? 1.0f : float(loop) / float(kRings);
    for (int s = 0; s < kSegments; ++s) {
      const float angle = 2.0f * kPi * float(s) / float(kSegments);
      const ImVec2 dir(std::cos(angle), std::sin(angle));
      const float grow = fringe ? 0.5f : (loop == kRings ? -0.5f : 0.0f);
      const ImVec2 ring_center(focus.x + (center.x - focus.x) * t,
                               focus.y + (center.y - focus.y) * t);
      const ImVec2 pos(ring_center.x + dir.x * (radii.x * t + grow),
                       ring_center.y + dir.y * (radii.y * t + grow));
      ImVec4 color = shade(t, dir);
      if (fringe) {
        color.w = 0.0f;
      }
      draw_list->PrimWriteVtx(pos, uv, ImGui::GetColorU32(color));
    }
  }
}

void DrawFaceButton(ImDrawList* draw_list, const FaceStyle& style, float cx, float cy, float radius,
                    float alpha, bool pressed) {
  const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 black(0.0f, 0.0f, 0.0f, 1.0f);
  const ImVec4 highlight = LerpColor(style.color, white, 0.40f);
  const ImVec4 shadow = LerpColor(style.color, black, 0.40f);
  const ImVec4 glow = LerpColor(style.color, white, 0.25f);
  const float face_alpha = std::min(alpha * (pressed ? 2.6f : 2.0f), 1.0f);
  const float face_radius = radius * (pressed ? 0.96f : 1.0f);
  const ImVec2 center(cx, cy + (pressed ? radius * 0.025f : 0.0f));

  // Lit from above, darkening toward the rim, with light passing through the
  // plastic and catching the lower edge.
  FillShadedEllipse(
      draw_list, center, ImVec2(face_radius, face_radius),
      ImVec2(center.x, center.y - face_radius * 0.35f), [&](float t, ImVec2 dir) {
        ImVec4 color = LerpColor(highlight, style.color, SmoothStep(0.0f, 0.6f, t));
        color = LerpColor(color, shadow, SmoothStep(0.55f, 1.0f, t));
        const float lower = std::max(dir.y, 0.0f);
        color = LerpColor(color, glow, 0.7f * lower * lower * SmoothStep(0.7f, 0.95f, t));
        color.w = face_alpha;
        return color;
      });

  // Soft edged sheen fading out toward the middle of the dome.
  const float gloss_alpha = std::min(alpha * (pressed ? 0.45f : 1.1f), 0.75f);
  const ImVec2 gloss_center(center.x, center.y - face_radius * 0.42f);
  const ImVec2 gloss_radii(face_radius * 0.62f, face_radius * 0.40f);
  FillShadedEllipse(draw_list, gloss_center, gloss_radii, gloss_center,
                    [&](float t, ImVec2 dir) {
                      const float y = 0.5f - 0.5f * dir.y * t;
                      ImVec4 color = white;
                      color.w = gloss_alpha * (0.15f + 0.85f * y * y) *
                                (1.0f - SmoothStep(0.55f, 1.0f, t));
                      return color;
                    });
  draw_list->AddCircle(center, face_radius, ColorWithAlpha(white, alpha * 0.62f), 48, 2.0f);
  if (pressed) {
    draw_list->AddCircle(center, face_radius + 2.0f, ColorWithAlpha(white, alpha * 0.85f), 48,
                         2.0f);
  }

  ImFont* font = FaceButtonFont();
  const float font_size = std::min(std::round(face_radius * 1.12f), font->LegacySize);
  const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, style.label);
  const ImVec2 text_pos(std::round(center.x - text_size.x * 0.5f + face_radius * 0.02f),
                        std::round(center.y - text_size.y * 0.5f));
  draw_list->AddText(font, font_size, ImVec2(text_pos.x, text_pos.y + 1.0f),
                     ColorWithAlpha(black, alpha * 0.42f), style.label);
  const ImU32 label_color = ColorWithAlpha(white, std::min(alpha * 2.1f, 1.0f));
  draw_list->AddText(font, font_size, text_pos, label_color, style.label);
}

void DrawFaceGlyph(ImDrawList* draw_list, FaceGlyph glyph, ImVec2 c, float size, ImU32 color,
                   float thickness) {
  switch (glyph) {
    case FaceGlyph::kCross: {
      const float d = size * 0.62f;
      draw_list->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), color, thickness);
      draw_list->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), color, thickness);
      break;
    }
    case FaceGlyph::kCircle:
      draw_list->AddCircle(c, size * 0.72f, color, 32, thickness);
      break;
    case FaceGlyph::kSquare: {
      const float h = size * 0.60f;
      draw_list->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), color, 0.0f, 0,
                         thickness);
      break;
    }
    case FaceGlyph::kTriangle: {
      // Dropped a little so its centroid reads as centred.
      const float r = size * 0.82f;
      const float y = c.y + size * 0.06f;
      draw_list->AddTriangle(ImVec2(c.x, y - r), ImVec2(c.x + r * 0.866f, y + r * 0.5f),
                             ImVec2(c.x - r * 0.866f, y + r * 0.5f), color, thickness);
      break;
    }
  }
}

// Matte black dish with a coloured symbol, lit from above: the bezel catches
// the light on top while the hollow inside brightens toward the bottom.
void DrawPlayStationButton(ImDrawList* draw_list, const FaceStyle& style, float cx, float cy,
                           float radius, float alpha, bool pressed) {
  const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 black(0.0f, 0.0f, 0.0f, 1.0f);
  const ImVec4 bezel_lit(0.30f, 0.30f, 0.33f, 1.0f);
  const ImVec4 bezel_dark(0.11f, 0.11f, 0.12f, 1.0f);
  const ImVec4 well_top(0.04f, 0.04f, 0.05f, 1.0f);
  const ImVec4 well_bottom(0.13f, 0.13f, 0.15f, 1.0f);
  const float face_alpha = std::min(alpha * (pressed ? 2.6f : 2.0f), 1.0f);
  const float face_radius = radius * (pressed ? 0.96f : 1.0f);
  const ImVec2 center(cx, cy + (pressed ? radius * 0.025f : 0.0f));

  FillShadedEllipse(draw_list, center, ImVec2(face_radius, face_radius), center,
                    [&](float t, ImVec2 dir) {
                      const float y = 0.5f + 0.5f * dir.y * t;
                      const ImVec4 well = LerpColor(well_top, well_bottom, y);
                      const ImVec4 bezel = LerpColor(bezel_lit, bezel_dark, 0.5f + 0.5f * dir.y);
                      ImVec4 color = LerpColor(well, bezel, SmoothStep(0.74f, 0.9f, t));
                      color.w = face_alpha;
                      return color;
                    });
  draw_list->AddCircle(center, face_radius, ColorWithAlpha(black, face_alpha * 0.8f), 48, 2.0f);
  draw_list->AddCircle(center, face_radius + 1.5f, ColorWithAlpha(white, alpha * 0.30f), 48,
                       1.0f);

  const float glyph_size = face_radius * 0.62f;
  const float thickness = std::max(2.0f, face_radius * (pressed ? 0.15f : 0.12f));
  if (pressed) {
    draw_list->AddCircleFilled(center, face_radius * 0.82f,
                               ColorWithAlpha(style.color, alpha * 0.35f), 48);
    draw_list->AddCircle(center, face_radius + 2.0f, ColorWithAlpha(style.color, alpha * 1.6f), 48,
                         2.0f);
  }
  DrawFaceGlyph(draw_list, style.glyph, ImVec2(center.x, center.y + 1.5f), glyph_size,
                ColorWithAlpha(black, alpha * 0.6f), thickness);
  const ImVec4 glyph_color = pressed ? LerpColor(style.color, white, 0.35f) : style.color;
  DrawFaceGlyph(draw_list, style.glyph, center, glyph_size,
                ColorWithAlpha(glyph_color, std::min(alpha * 2.4f, 1.0f)), thickness);
}

// One arrowhead of a D-pad, pointing away from the centre along (dx, dy).
void DrawDpadArrow(ImDrawList* draw_list, float cx, float cy, float radius, float dx, float dy,
                   float alpha, bool lit) {
  const float tip = radius * 0.72f;
  const float base = radius * 0.36f;
  const float half = radius * 0.20f;
  // Perpendicular to the direction, for the two base corners.
  const float px = -dy;
  const float py = dx;
  const ImVec2 a(cx + dx * tip, cy + dy * tip);
  const ImVec2 b(cx + dx * base + px * half, cy + dy * base + py * half);
  const ImVec2 c(cx + dx * base - px * half, cy + dy * base - py * half);
  draw_list->AddTriangleFilled(a, b, c, EdgeColor(alpha, lit));
}

}  // namespace

TouchControlsOverlay::TouchControlsOverlay(rex::ui::ImGuiDrawer* drawer, TouchInputDriver* driver,
                                           InputSystem* input_system)
    : rex::ui::ImGuiDialog(drawer), driver_(driver), input_system_(input_system) {}

void TouchControlsOverlay::OnDraw(ImGuiIO& io) {
  if (!driver_ || !input_system_) {
    return;
  }
  bool assigned = false;
  for (const auto& view : input_system_->SnapshotDevices()) {
    if (view.device.kind == DeviceKind::kTouch && view.guest_user_mask != 0) {
      assigned = true;
      break;
    }
  }
  if (!assigned) {
    return;
  }
  TouchVisualState state;
  if (!driver_->GetVisualState(&state)) {
    return;
  }

  const float width = io.DisplaySize.x;
  const float height = io.DisplaySize.y;
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }

  const float alpha = float(std::clamp(REXCVAR_GET(touch_opacity), 0.0, 1.0));
  if (alpha <= 0.0f) {
    return;
  }

  // The driver hit-tested against the window in pixels; ImGui draws in logical
  // points. Rebuilding the layout here rather than scaling the driver's copy
  // keeps both sides reading the same source of truth for the geometry.
  const TouchLayout layout = driver_->BuildLayout(width, height);

  // Foreground list so the pad sits above the game and above any overlay
  // window, drawn without a host window of its own so it never takes input:
  // touches belong to the driver, which sees them straight from the window.
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  for (size_t i = 0; i < layout.controls.size(); ++i) {
    const TouchControl& c = layout.controls[i];
    const bool pressed = (state.pressed_mask & (uint64_t(1) << i)) != 0;
    const uint16_t held = i < state.control_buttons.size() ? state.control_buttons[i] : 0;

    switch (c.shape) {
      case TouchControl::Shape::kCircle: {
        FaceStyle face_style;
        if (GetFaceStyle(c.button, layout.face_style, &face_style)) {
          if (layout.face_style == TouchFaceStyle::kPlayStation) {
            DrawPlayStationButton(draw_list, face_style, c.cx, c.cy, c.half_width, alpha, pressed);
          } else {
            DrawFaceButton(draw_list, face_style, c.cx, c.cy, c.half_width, alpha, pressed);
          }
        } else {
          draw_list->AddCircleFilled(ImVec2(c.cx, c.cy), c.half_width, FillColor(alpha, pressed),
                                     32);
          draw_list->AddCircle(ImVec2(c.cx, c.cy), c.half_width, EdgeColor(alpha, pressed), 32,
                               2.0f);
          DrawLabel(draw_list, c.label.c_str(), c.cx, c.cy, alpha, pressed);
        }
        break;
      }

      case TouchControl::Shape::kPill: {
        const ImVec2 lo(c.cx - c.half_width, c.cy - c.half_height);
        const ImVec2 hi(c.cx + c.half_width, c.cy + c.half_height);
        draw_list->AddRectFilled(lo, hi, FillColor(alpha, pressed), c.half_height);
        draw_list->AddRect(lo, hi, EdgeColor(alpha, pressed), c.half_height, 0, 2.0f);
        DrawLabel(draw_list, c.label.c_str(), c.cx, c.cy, alpha, pressed);
        break;
      }

      case TouchControl::Shape::kDpad:
        draw_list->AddCircleFilled(ImVec2(c.cx, c.cy), c.half_width, FillColor(alpha, false), 32);
        draw_list->AddCircle(ImVec2(c.cx, c.cy), c.half_width, EdgeColor(alpha, pressed), 32, 2.0f);
        // Each arrow lights on its own, so a diagonal lights two of them and
        // the drawing matches what the guest is being told.
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 0.0f, -1.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_UP) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 0.0f, 1.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_DOWN) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, -1.0f, 0.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_LEFT) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 1.0f, 0.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_RIGHT) != 0);
        break;
    }
  }

  for (size_t i = 0; i < layout.sticks.size() && i < state.sticks.size(); ++i) {
    const TouchStick& stick = layout.sticks[i];
    const TouchStickVisual& visual = state.sticks[i];
    // A free-placed stick has no resting position, so it only exists on screen
    // while a finger is planting it. A pinned one is always drawn.
    if (!visual.active && !stick.fixed) {
      continue;
    }
    const ImVec2 centre(visual.cx * width, visual.cy * height);
    const ImVec2 knob(visual.knob_cx * width, visual.knob_cy * height);
    draw_list->AddCircleFilled(centre, stick.radius, FillColor(alpha, false), 48);
    draw_list->AddCircle(centre, stick.radius, EdgeColor(alpha, false), 48, 2.0f);
    draw_list->AddCircleFilled(knob, stick.radius * 0.42f, FillColor(alpha, visual.active), 32);
    draw_list->AddCircle(knob, stick.radius * 0.42f, EdgeColor(alpha, visual.active), 32, 2.0f);
  }
}

}  // namespace eternalsonata::touch
