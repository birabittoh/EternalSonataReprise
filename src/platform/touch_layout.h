// eternalsonata - ReXGlue Recompiled Project
//
// The on-screen pad's design for this game.
//
// The SDK owns the machinery (hit-testing, multi-touch, the resulting
// X_INPUT_STATE and the drawing) but deliberately owns no layout, since every
// game wants a different one. This is Eternal Sonata's: it only lays out the
// inputs the game actually asks for, which is why there is no D-pad, no right
// stick, no Back and no stick press here even though the SDK can express all
// of them (see rex/input/touch/touch_input_driver.h).

#pragma once

#include <rex/input/touch/touch_input_driver.h>

#include <algorithm>

namespace eternalsonata {

// Builds the pad for a surface of the given size.
//
// Pure function of the size, as TouchLayoutProvider requires: the driver calls
// it in window pixels and the overlay calls it in ImGui points, and the two
// only describe the same pad because each rebuilds it in its own space.
inline rex::input::touch::TouchLayout BuildTouchLayout(float width, float height) {
  // rex::input for the X_INPUT_GAMEPAD_* masks, rex::input::touch for the
  // layout types and the Make* builders.
  using namespace rex::input;
  using namespace rex::input::touch;

  TouchLayout layout;
  if (width <= 0.0f || height <= 0.0f) {
    return layout;
  }

  // Every size is a fraction of the short side, so the pad keeps its
  // proportions and its reach from the edges on any aspect ratio instead of
  // stretching out with an ultra-wide screen.
  const float u = std::min(width, height);

  // Left stick, free-placed over the bottom-left of the screen: the thumb
  // plants it wherever it lands rather than having to find a fixed ring.
  layout.sticks.push_back(MakeStick(/*zone_left=*/0.0f, /*zone_top=*/height * 0.30f,
                                    /*zone_right=*/width * 0.45f, /*zone_bottom=*/height,
                                    /*radius=*/u * 0.16f, TouchAxis::kLeft));

  // Everything is kept this far off the edges. A phone's gesture-nav pill and
  // display cutout live right at them, and a thumb reaching the last few
  // millimetres of glass is both awkward and easy to misread as a system
  // gesture.
  const float inset = u * 0.06f;

  // A/B/X/Y diamond, bottom right, in the Xbox arrangement (A at the bottom).
  const float face_r = u * 0.085f;
  const float arm = u * 0.125f;
  const float fx = width - inset - face_r - arm;
  const float fy = height - inset - face_r - arm;
  layout.controls.push_back(MakeCircle(fx, fy + arm, face_r, "A", X_INPUT_GAMEPAD_A));
  layout.controls.push_back(MakeCircle(fx + arm, fy, face_r, "B", X_INPUT_GAMEPAD_B));
  layout.controls.push_back(MakeCircle(fx - arm, fy, face_r, "X", X_INPUT_GAMEPAD_X));
  layout.controls.push_back(MakeCircle(fx, fy - arm, face_r, "Y", X_INPUT_GAMEPAD_Y));

  // Shoulders, triggers and Start share one row along the top edge, where no
  // thumb rests during normal play.
  const float pill_rx = u * 0.075f;
  const float pill_ry = u * 0.030f;
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
  const float menu_rx = u * 0.10f;
  const float menu_offset = menu_rx + u * 0.01f;
  layout.controls.push_back(MakePill(width * 0.5f - menu_offset, pill_y, menu_rx, pill_ry, "GUIDE",
                                     X_INPUT_GAMEPAD_GUIDE));
  layout.controls.push_back(MakePill(width * 0.5f + menu_offset, pill_y, menu_rx, pill_ry, "START",
                                     X_INPUT_GAMEPAD_START));

  return layout;
}

}  // namespace eternalsonata
