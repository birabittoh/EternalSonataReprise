#include "generated/eternalsonata_init.h"

#include <chrono>
#include <unordered_map>

#include <rex/hook.h>

namespace {

using LipClock = std::chrono::steady_clock;

constexpr u32 kGameFrameRate = 0x82465F90;
constexpr u32 kLipActiveOffset = 11;
constexpr float kLipTicksPerSecond = 300.0f;
constexpr float kMaximumLipSampleSeconds = 0.1f;

thread_local std::unordered_map<u32, LipClock::time_point> g_lip_update_times;
thread_local bool g_has_lip_elapsed = false;
thread_local float g_lip_elapsed_ticks = 0.0f;

} // namespace

REX_EXTERN(__imp__sub_82140728);

REX_HOOK_RAW(sub_82140728) {
  const u32 lip_state = ctx.r3.u32;
  const u8 frame_rate = REX_LOAD_U8(kGameFrameRate);
  const bool use_wall_time = lip_state != 0 && frame_rate > 30 &&
                             REX_LOAD_U8(lip_state + kLipActiveOffset) != 0;

  const bool previous_has_lip_elapsed = g_has_lip_elapsed;
  const float previous_lip_elapsed_ticks = g_lip_elapsed_ticks;
  g_has_lip_elapsed = false;

  if (use_wall_time) {
    const auto now = LipClock::now();
    const auto [it, inserted] = g_lip_update_times.try_emplace(lip_state, now);
    if (!inserted) {
      const float elapsed =
          std::chrono::duration<float>(now - it->second).count();
      it->second = now;
      if (elapsed > 0.0f && elapsed <= kMaximumLipSampleSeconds) {
        g_lip_elapsed_ticks = elapsed * kLipTicksPerSecond;
        g_has_lip_elapsed = true;
      }
    }
  } else if (lip_state != 0) {
    g_lip_update_times.erase(lip_state);
  }

  __imp__sub_82140728(ctx, base);
  g_has_lip_elapsed = previous_has_lip_elapsed;
  g_lip_elapsed_ticks = previous_lip_elapsed_ticks;
}

REX_EXTERN(__imp__sub_82181728);

REX_HOOK_RAW(sub_82181728) {
  __imp__sub_82181728(ctx, base);

  if (!g_has_lip_elapsed) {
    return;
  }

  const u8 frame_rate = REX_LOAD_U8(kGameFrameRate);
  if (frame_rate == 0) {
    return;
  }

  const float nominal_ticks = kLipTicksPerSecond / frame_rate;
  ctx.f1.f64 *= static_cast<double>(g_lip_elapsed_ticks / nominal_ticks);
}
