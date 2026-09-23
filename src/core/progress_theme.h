#pragma once

#include <rex/ui/progress_window.h>

namespace eternalsonata {

// The overlay's brown and purple palette, for every startup progress window.
inline rex::ui::ProgressWindowTheme ProgressTheme() {
  rex::ui::ProgressWindowTheme theme;
  theme.background[0] = 0x3D;
  theme.background[1] = 0x24;
  theme.background[2] = 0x10;
  theme.bar_fill[0] = 0x9B;
  theme.bar_fill[1] = 0x59;
  theme.bar_fill[2] = 0xB6;
  theme.bar_frame[0] = 0x4F;
  theme.bar_frame[1] = 0x28;
  theme.bar_frame[2] = 0x06;
  return theme;
}

}  // namespace eternalsonata
