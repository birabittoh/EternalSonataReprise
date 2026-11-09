// eternalsonata - The PS3's opening look over the battlefield.
//
// The PS3 battle FSM (EBOOT sub_19B5C0, state 4 at 0x19C13C) holds the
// state for 1000 ticks of 1/300 s, or until cross or circle is pressed,
// before it sets up the intro. The 360's state 4 sets the intro up on its
// first frame, so the camera goes straight to the first speaker.
//
// What the camera does meanwhile also differs. Camera mode 1, which the
// battle starts in, is the 360's idle orbit (sub_82199600) but on the PS3
// (sub_1528D8) a pan whose aim sweeps from -x to x of the field's half width
// over the same 1000 ticks, eased by the camera blend (1 - cos(pi t)) / 2.
// While the state is held the orbit's first pose is kept and slid sideways
// across most of that width.

#include "generated/eternalsonata_init.h"

#include <bit>
#include <cmath>
#include <numbers>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "eternalsonata_hooks_internal.h"
#include "target.h"

REXCVAR_DEFINE_DOUBLE(battle_intro_overview_time, -1.0, "Eternal Sonata",
                      "Seconds the battle opens on a look over the battlefield, A or B "
                      "skips it: 0 never, negative follows the game data (PS3 3.33, 360 none)")
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

// Battle manager (dword_824D0440) FSM state.
constexpr uint32_t kBattleState = 0x824D0440u + 533040u;
constexpr uint32_t kStateIntroSetup = 4u;

// Battlefield half width, the PS3's manager+0x80CF8.
constexpr uint32_t kFieldHalfWidth = 0x825513B0u;
// The full width overshoots the field from the orbit's framing.
constexpr float kSlideScale = 0.75f;

// Camera controller block 0 (0x824CEBDC): its aim (+184) and the yaw drift
// sub_82199600 arms for the orbit (speed, limits, step).
constexpr uint32_t kControllerAim = 0x824CEC94u;
constexpr uint32_t kOrbitSpeed = 0x824CEDE4u;
constexpr uint32_t kOrbitSpeedCount = 4u;

// The active render camera and its fields read by sub_821078B0.
constexpr uint32_t kRenderCamera = 0x82440618u;
constexpr uint32_t kCamAngles = 32u;
constexpr uint32_t kCamTarget = 996u;
constexpr uint32_t kCamSmoothedTarget = 1008u;
constexpr uint32_t kCamSnapTarget = 1147u;

double g_elapsed = 0.0;
double g_hold = kHoldTicks;
uint32_t g_camera = 0;
bool g_have_pose = false;
float g_target[3];
float g_aim[3];
float g_orbit_speed[kOrbitSpeedCount];
float g_yaw;

rex::memory::Memory* Memory() { return rex::Runtime::instance()->memory(); }

template <typename T>
T Load(uint32_t address) {
  return rex::memory::load_and_swap<T>(Memory()->TranslateVirtual<uint8_t*>(address));
}

template <typename T>
void Store(uint32_t address, T value) {
  rex::memory::store_and_swap<T>(Memory()->TranslateVirtual<uint8_t*>(address), value);
}

float LoadFloat(uint32_t address) { return std::bit_cast<float>(Load<uint32_t>(address)); }
void StoreFloat(uint32_t address, float value) { Store<uint32_t>(address, std::bit_cast<uint32_t>(value)); }

// What sub_82181728(&flt_824D0130, 0) returns, with the wall clock correction
// its hook applies.
double FrameTicks() {
  const uint8_t rate = Load<uint8_t>(kFrameRateByte);
  if (rate == 0)
    return 0.0;
  return double(300 / rate) * LoadFloat(kDeltaCategory0) * LoadFloat(kDeltaScale) *
         eternalsonata_hooks::FrameDeltaScale();
}

// The hold in 1/300 s ticks, zero for none.
double HoldTicks() {
  const double seconds = REXCVAR_GET(battle_intro_overview_time);
  if (seconds < 0.0)
    return eternalsonata::IsPs3Target() ? kHoldTicks : 0.0;
  return seconds * 300.0;
}

bool SkipPressed() {
  for (uint32_t port = 0; port < kPadCount; ++port) {
    if (Load<uint8_t>(kPadConnected + kPadStride * port) &&
        (Load<uint32_t>(kPadPressed + kPadStride * port) & kSkipButtons))
      return true;
  }
  return false;
}

void TakeCamera() {
  if (g_camera)
    return;
  g_camera = Load<uint32_t>(kRenderCamera);
}

void ReleaseCamera() {
  if (g_have_pose) {
    for (uint32_t i = 0; i < kOrbitSpeedCount; ++i)
      StoreFloat(kOrbitSpeed + 4 * i, g_orbit_speed[i]);
    for (uint32_t i = 0; i < 3; ++i)
      StoreFloat(kControllerAim + 4 * i, g_aim[i]);
    if (Load<uint32_t>(kRenderCamera) == g_camera) {
      for (uint32_t i = 0; i < 3; ++i) {
        StoreFloat(g_camera + kCamTarget + 4 * i, g_target[i]);
        StoreFloat(g_camera + kCamSmoothedTarget + 4 * i, g_target[i]);
      }
      Store<uint8_t>(g_camera + kCamSnapTarget, 1);
    }
  }
  g_camera = 0;
  g_have_pose = false;
}

void EndOverview() {
  g_elapsed = 0.0;
  ReleaseCamera();
}

}  // namespace

// Battle FSM state 4 (sub_821ACBF8): true skips the intro setup this frame.
extern "C++" bool EternalSonataBattleIntroOverview();

bool EternalSonataBattleIntroOverview() {
  g_hold = HoldTicks();
  if (g_hold <= 0.0) {
    EndOverview();
    return false;
  }
  TakeCamera();
  g_elapsed += FrameTicks();
  if (g_elapsed > g_hold || SkipPressed()) {
    EndOverview();
    return false;
  }
  return true;
}

// Scene render (sub_820C7538), after the object update has moved the orbit and
// before the render camera is rebuilt from its fields.
extern "C++" void EternalSonataBattleOverviewCamera();

void EternalSonataBattleOverviewCamera() {
  if (!g_camera)
    return;
  if (Load<uint32_t>(kBattleState) != kStateIntroSetup) {
    EndOverview();
    return;
  }
  if (Load<uint32_t>(kRenderCamera) != g_camera)
    return;
  if (!g_have_pose) {
    for (uint32_t i = 0; i < 3; ++i) {
      g_target[i] = LoadFloat(g_camera + kCamTarget + 4 * i);
      g_aim[i] = LoadFloat(kControllerAim + 4 * i);
    }
    for (uint32_t i = 0; i < kOrbitSpeedCount; ++i)
      g_orbit_speed[i] = LoadFloat(kOrbitSpeed + 4 * i);
    g_yaw = LoadFloat(g_camera + kCamAngles + 4);
    g_have_pose = true;
  }
  // The orbit is the controller's yaw drift; with it stopped the camera keeps
  // its first rotation and follows the aim, which slides along the camera's
  // right axis across the field's width.
  for (uint32_t i = 0; i < kOrbitSpeedCount; ++i)
    StoreFloat(kOrbitSpeed + 4 * i, 0.0f);
  const float t = float(std::fmin(g_elapsed / g_hold, 1.0));
  const float blend = (1.0f - std::cos(std::numbers::pi_v<float> * t)) * 0.5f;
  const float slide = LoadFloat(kFieldHalfWidth) * kSlideScale * (2.0f * blend - 1.0f);
  const float right[3] = {std::cos(g_yaw), 0.0f, -std::sin(g_yaw)};
  for (uint32_t i = 0; i < 3; ++i) {
    StoreFloat(kControllerAim + 4 * i, g_aim[i] + right[i] * slide);
    StoreFloat(g_camera + kCamTarget + 4 * i, g_target[i] + right[i] * slide);
    StoreFloat(g_camera + kCamSmoothedTarget + 4 * i, g_target[i] + right[i] * slide);
  }
  Store<uint8_t>(g_camera + kCamSnapTarget, 1);
}
