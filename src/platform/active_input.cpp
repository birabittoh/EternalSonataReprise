#include "active_input.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/sdl/sdl_input_driver.h>
#if REX_PLATFORM_WIN32
#include <rex/input/xinput/xinput_input_driver.h>
#endif
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace eternalsonata {
namespace {

using namespace rex;
using namespace rex::input;

using Clock = std::chrono::steady_clock;

constexpr int16_t kStickInUse = 12000;
constexpr uint8_t kTriggerPressed = 30;
constexpr float kMeantMotion = 24.0f;
constexpr auto kMotionWindow = std::chrono::milliseconds(500);
constexpr auto kPollInterval = std::chrono::milliseconds(16);

constexpr uint16_t kMicrosoft = 0x045E;
constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kNintendo = 0x057E;

int64_t Now() { return Clock::now().time_since_epoch().count(); }

template <typename D>
int64_t Ticks(D d) {
  return std::chrono::duration_cast<Clock::duration>(d).count();
}

std::atomic<int64_t> g_keys_used{0};
std::atomic<int64_t> g_pad_used{0};
std::atomic<int64_t> g_touch_used{0};
std::atomic<rex::input::InputSystem*> g_input{nullptr};
std::atomic<ActiveInputKind> g_pad_kind{ActiveInputKind::kUnknown};

bool Has(std::string_view name, std::string_view word) {
  return name.find(word) != std::string_view::npos;
}

uint16_t GuidWord(std::string_view guid, size_t byte) {
  if (guid.size() < (byte + 2) * 2) {
    return 0;
  }
  const std::string lo(guid.substr(byte * 2, 2));
  const std::string hi(guid.substr(byte * 2 + 2, 2));
  return static_cast<uint16_t>(std::strtoul(lo.c_str(), nullptr, 16) |
                               (std::strtoul(hi.c_str(), nullptr, 16) << 8));
}

ActiveInputKind KindOf(const rex::input::DeviceInfo& info) {
  const uint16_t vendor = GuidWord(info.guid, 4);
  const std::string_view name = info.name;
  if (vendor == kSony || Has(name, "PS4") || Has(name, "PS5") || Has(name, "DualSense") ||
      Has(name, "DualShock")) {
    return ActiveInputKind::kPlayStation;
  }
  if (vendor == kNintendo || Has(name, "Switch") || Has(name, "Joy-Con")) {
    return ActiveInputKind::kNintendo;
  }
  if (vendor == kMicrosoft || Has(name, "Xbox") || Has(name, "XInput")) {
    return ActiveInputKind::kXbox;
  }
  return ActiveInputKind::kOtherPad;
}

bool PadInUse(const X_INPUT_GAMEPAD& pad) {
  return pad.buttons != 0 || pad.left_trigger > kTriggerPressed ||
         pad.right_trigger > kTriggerPressed || std::abs(pad.thumb_lx) > kStickInUse ||
         std::abs(pad.thumb_ly) > kStickInUse || std::abs(pad.thumb_rx) > kStickInUse ||
         std::abs(pad.thumb_ry) > kStickInUse;
}

bool ReadDevice(rex::input::InputSystem* input, rex::input::DeviceId id, X_INPUT_STATE* out) {
  if (auto* sdl = input->GetDriver<rex::input::sdl::SDLInputDriver>()) {
    if (sdl->GetDeviceState(id, out) == X_ERROR_SUCCESS) {
      return true;
    }
  }
#if REX_PLATFORM_WIN32
  if (auto* xi = input->GetDriver<rex::input::xinput::XinputInputDriver>()) {
    if (xi->GetDeviceState(id, out) == X_ERROR_SUCCESS) {
      return true;
    }
  }
#endif
  return false;
}

void PollPads() {
  static std::atomic<int64_t> next{0};
  const int64_t now = Now();
  if (now < next.load(std::memory_order_relaxed)) {
    return;
  }
  next.store(now + Ticks(kPollInterval), std::memory_order_relaxed);

  auto* input = g_input.load(std::memory_order_acquire);
  if (!input) {
    return;
  }
  ActiveInputKind kind = ActiveInputKind::kUnknown;
  bool used = false;
  for (const auto& view : input->SnapshotDevices()) {
    const auto& device = view.device;
    if (device.synthetic || device.kind != rex::input::DeviceKind::kController) {
      continue;
    }
    if (kind == ActiveInputKind::kUnknown) {
      kind = KindOf(device);
    }
    X_INPUT_STATE state{};
    if (ReadDevice(input, device.id, &state) && PadInUse(state.gamepad)) {
      kind = KindOf(device);
      used = true;
      break;
    }
  }
  g_pad_kind.store(kind, std::memory_order_release);
  if (used) {
    g_pad_used.store(now, std::memory_order_release);
  }
}

}  // namespace

class ActiveInputTrackerImpl final : public ActiveInputTracker,
                                     public rex::ui::WindowInputListener,
                                     public rex::ui::WindowListener {
 public:
  ActiveInputTrackerImpl(rex::ui::Window* window, rex::input::InputSystem* input)
      : window_(window) {
    g_input.store(input, std::memory_order_release);
    window_->AddInputListener(this, 1);
    window_->AddListener(this);
  }

  ~ActiveInputTrackerImpl() override {
    g_input.store(nullptr, std::memory_order_release);
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }

  void OnKeyDown(rex::ui::KeyEvent&) override { g_keys_used.store(Now(), std::memory_order_release); }
  void OnTouchEvent(rex::ui::TouchEvent&) override {
    g_touch_used.store(Now(), std::memory_order_release);
  }
  void OnMouseDown(rex::ui::MouseEvent&) override {
    g_keys_used.store(Now(), std::memory_order_release);
  }

  // Small drifts of the cursor do not count as using the mouse.
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    std::lock_guard<std::mutex> lock(mutex_);
    float motion = std::fabs(e.dx()) + std::fabs(e.dy());
    if (motion == 0.0f && have_position_) {
      motion = static_cast<float>(std::abs(e.x() - x_) + std::abs(e.y() - y_));
    }
    x_ = e.x();
    y_ = e.y();
    have_position_ = true;
    if (motion == 0.0f) {
      return;
    }
    const int64_t now = Now();
    if (now - last_motion_ > Ticks(kMotionWindow)) {
      motion_ = 0.0f;
    }
    motion_ += motion;
    last_motion_ = now;
    if (motion_ >= kMeantMotion) {
      g_keys_used.store(now, std::memory_order_release);
    }
  }

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    motion_ = 0.0f;
    have_position_ = false;
  }

 private:
  rex::ui::Window* window_;
  std::mutex mutex_;
  float motion_ = 0.0f;
  int64_t last_motion_ = 0;
  int32_t x_ = 0, y_ = 0;
  bool have_position_ = false;
};

std::unique_ptr<ActiveInputTracker> CreateActiveInputTracker(rex::ui::Window* window,
                                                             rex::input::InputSystem* input) {
  return window ? std::make_unique<ActiveInputTrackerImpl>(window, input) : nullptr;
}

ActiveInputKind ActiveInput() {
  PollPads();
  const ActiveInputKind pad = g_pad_kind.load(std::memory_order_acquire);
  const int64_t keys = g_keys_used.load(std::memory_order_acquire);
  const int64_t touch = g_touch_used.load(std::memory_order_acquire);
  // Touch controls are a virtual pad. Without a real one there is no pad kind to
  // report, so the generic one stands in until the keyboard is used again.
  if (touch > keys && touch > g_pad_used.load(std::memory_order_acquire)) {
    return pad == ActiveInputKind::kUnknown ? ActiveInputKind::kOtherPad : pad;
  }
#if REX_PLATFORM_ANDROID
  const bool keyboard = rex::cvar::GetFlagByName("mnk_mode") == "true" && keys > 0;
#else
  const bool keyboard = rex::cvar::GetFlagByName("mnk_mode") == "true";
#endif
  if (pad == ActiveInputKind::kUnknown) {
    return keyboard ? ActiveInputKind::kKeyboardMouse : ActiveInputKind::kUnknown;
  }
  if (keyboard &&
      g_keys_used.load(std::memory_order_acquire) > g_pad_used.load(std::memory_order_acquire)) {
    return ActiveInputKind::kKeyboardMouse;
  }
  return pad;
}

const char* ToString(ActiveInputKind kind) {
  switch (kind) {
    case ActiveInputKind::kKeyboardMouse:
      return "keyboard";
    case ActiveInputKind::kXbox:
      return "xbox";
    case ActiveInputKind::kPlayStation:
      return "playstation";
    case ActiveInputKind::kNintendo:
      return "nintendo";
    case ActiveInputKind::kOtherPad:
      return "pad";
    default:
      return "unknown";
  }
}

}  // namespace eternalsonata
