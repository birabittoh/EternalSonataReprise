// eternalsonata - The PS3's opening look over the battlefield.
//
// The PS3 battle FSM (EBOOT sub_19B5C0, state 4 at 0x19C13C) holds the
// state for 1000 ticks of 1/300 s, or until cross or circle is pressed,
// before it sets up the intro, so the battle camera's opening move plays
// out. The 360's state 4 sets the intro up on its first frame, so the
// camera goes straight to the first speaker.

#include "generated/eternalsonata_init.h"

#include <bit>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "eternalsonata_hooks_internal.h"
#include "target.h"

REXCVAR_DEFINE_STRING(battle_intro_overview, "auto", "Eternal Sonata",
                      "Whether the battle opens on a look over the battlefield: 360 (no), "
                      "ps3 (yes, A or B skips it) or auto (follows the game data)")
    .allowed({"auto", "360", "ps3"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr double kHoldTicks = 1000.0;  // EBOOT TOC float 0x53DAD8
constexpr uint32_t kFrameRateByte = 0x82465F90u;
// sub_82181728's category 0 scales.
constexpr uint32_t kDeltaScale = 0x824D0130u;
constexpr uint32_t kDeltaCategory0 = 0x824D0134u;
// Pad records read by sub_82199580: connected byte and pressed buttons.
constexpr uint32_t kPadConnected = 0x824BB41Au;
constexpr uint32_t kPadPressed = 0x824BB5C4u;
constexpr uint32_t kPadStride = 464u;
constexpr uint32_t kPadCount = 4u;
// A and B, where the PS3 takes cross and circle.
constexpr uint32_t kSkipButtons = 0x1000u | 0x2000u;

double g_elapsed = 0.0;

template <typename T>
T Load(uint32_t address) {
  auto* memory = rex::Runtime::instance()->memory();
  return rex::memory::load_and_swap<T>(memory->TranslateVirtual<uint8_t*>(address));
}

float LoadFloat(uint32_t address) { return std::bit_cast<float>(Load<uint32_t>(address)); }

// What sub_82181728(&flt_824D0130, 0) returns, with the wall clock correction
// its hook applies.
double FrameTicks() {
  const uint8_t rate = Load<uint8_t>(kFrameRateByte);
  if (rate == 0)
    return 0.0;
  return double(300 / rate) * LoadFloat(kDeltaCategory0) * LoadFloat(kDeltaScale) *
         eternalsonata_hooks::FrameDeltaScale();
}

bool OverviewEnabled() {
  const std::string& value = REXCVAR_GET(battle_intro_overview);
  return value == "ps3" || (value == "auto" && eternalsonata::IsPs3Target());
}

bool SkipPressed() {
  for (uint32_t port = 0; port < kPadCount; ++port) {
    if (Load<uint8_t>(kPadConnected + kPadStride * port) &&
        (Load<uint32_t>(kPadPressed + kPadStride * port) & kSkipButtons))
      return true;
  }
  return false;
}

}  // namespace

// Battle FSM state 4 (sub_821ACBF8): true skips the intro setup this frame.
extern "C++" bool EternalSonataBattleIntroOverview();

bool EternalSonataBattleIntroOverview() {
  if (!OverviewEnabled()) {
    g_elapsed = 0.0;
    return false;
  }
  g_elapsed += FrameTicks();
  if (g_elapsed > kHoldTicks || SkipPressed()) {
    g_elapsed = 0.0;
    return false;
  }
  return true;
}
