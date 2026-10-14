// eternalsonata - ReXGlue Recompiled Project
//
// The on-screen pad's design for this game.
//
// The driver owns the machinery (hit-testing, multi-touch, the resulting
// X_INPUT_STATE) and the overlay the drawing; this is the layout. It only lays
// out the inputs the game actually asks for, which is why there is no D-pad, no
// right stick, no Back and no stick press here even though the driver can
// express all of them (see touch_input_driver.h).

#pragma once

#include "touch_input_driver.h"

#include <algorithm>
#include <utility>

#include <rex/cvar.h>

#include "button_glyphs.h"
#include "target.h"

REXCVAR_DECLARE(double, touch_scale);

namespace eternalsonata {

// Builds the pad for a surface of the given size.
//
// Pure function of the size, as TouchLayoutProvider requires: the driver calls
// it in window pixels and the overlay calls it in ImGui points, and the two
// only describe the same pad because each rebuilds it in its own space.
inline touch::TouchLayout BuildTouchLayout(float width, float height) {
  using namespace touch;

  TouchLayout layout;
  if (width <= 0.0f || height <= 0.0f) {
    return layout;
  }

  // Every size is a fraction of the short side, so the pad keeps its
  // proportions and its reach from the edges on any aspect ratio instead of
  // stretching out with an ultra-wide screen.
  const float u = std::min(width, height);
  const float scale = float(std::clamp(REXCVAR_GET(touch_scale), 0.5, 2.0));

  // Left stick, free-placed over the bottom-left of the screen: the thumb
  // plants it wherever it lands rather than having to find a fixed ring.
  layout.sticks.push_back(MakeStick(/*zone_left=*/0.0f, /*zone_top=*/height * 0.30f,
                                    /*zone_right=*/width * 0.45f, /*zone_bottom=*/height,
                                    /*radius=*/u * 0.16f * scale, TouchAxis::kLeft));

  // Everything is kept this far off the edges. A phone's gesture-nav pill and
  // display cutout live right at them, and a thumb reaching the last few
  // millimetres of glass is both awkward and easy to misread as a system
  // gesture.
  const float inset = u * 0.06f;

  // The art follows the override_button_glyphs setting; the pad itself stays positional.
  const bool ps3 = IsPs3Target();
  if (button_glyphs::UsePlayStationArt()) {
    layout.face_style = TouchFaceStyle::kPlayStation;
  }

  // A/B/X/Y diamond, bottom right, in the Xbox arrangement (A at the bottom).
  const float face_r = u * 0.085f * scale;
  const float arm = u * 0.125f * scale;
  const float fx = width - inset - face_r - arm;
  const float fy = height - inset - face_r - arm;
  layout.controls.push_back(MakeCircle(fx, fy + arm, face_r, "A", X_INPUT_GAMEPAD_A));
  layout.controls.push_back(MakeCircle(fx + arm, fy, face_r, "B", X_INPUT_GAMEPAD_B));
  layout.controls.push_back(MakeCircle(fx - arm, fy, face_r, "X", X_INPUT_GAMEPAD_X));
  layout.controls.push_back(MakeCircle(fx, fy - arm, face_r, "Y", X_INPUT_GAMEPAD_Y));

  // Fast forward sits past the diamond's top right corner, clear of Y and B.
  TouchControl ff = MakeCircle(fx + arm * 1.3f, fy - arm * 1.55f, u * 0.06f * scale, "", 0);
  ff.fast_forward = true;
  layout.controls.push_back(std::move(ff));

  // Shoulders, triggers and Start share one row along the top edge, where no
  // thumb rests during normal play.
  const float pill_rx = u * 0.075f * scale;
  const float pill_ry = u * 0.030f * scale;
  const float pill_y = inset + pill_ry;
  const float margin = inset;
  const float step = pill_rx * 2.0f + u * 0.02f;
  layout.controls.push_back(
      MakePill(margin + pill_rx, pill_y, pill_rx, pill_ry, "LT", 0, TouchTrigger::kLeft));
  layout.controls.push_back(MakePill(margin + pill_rx + step, pill_y, pill_rx, pill_ry, "LB",
                                     X_INPUT_GAMEPAD_LEFT_SHOULDER));
  layout.controls.push_back(MakePill(width - margin - pill_rx, pill_y, pill_rx, pill_ry, "RT", 0,
                                     TouchTrigger::kRight));
  layout.controls.push_back(MakePill(width - margin - pill_rx - step, pill_y, pill_rx, pill_ry,
                                     "RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER));
  // Start and Guide sit side by side, the pair centred on the row rather than
  // either one of them.
  const float menu_rx = u * 0.10f * scale;
  const float menu_offset = menu_rx + u * 0.01f;
  layout.controls.push_back(MakePill(width * 0.5f - menu_offset, pill_y, menu_rx, pill_ry,
                                     ps3 ? "MENU" : "GUIDE", X_INPUT_GAMEPAD_GUIDE));
  layout.controls.push_back(MakePill(width * 0.5f + menu_offset, pill_y, menu_rx, pill_ry, "START",
                                     X_INPUT_GAMEPAD_START));

  return layout;
}

}  // namespace eternalsonata
