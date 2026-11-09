// eternalsonata - The PS3's ending credits speed.
//
// sub_82132100 spawns the next ED credits line: it scrolls up at a fixed
// 0.32 pixels per 1/300 s, and the next spawn waits for the line's height
// plus 8 at that speed. The PS3 (EBOOT 0x3237B0, 0x323D38) reads the speed
// from the task instead, picked once by the scenario counter, because its
// longer ending scenes need slower credits to fill them.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <bit>
#include <cstdint>

#include <rex/hook.h>
#include <rex/memory/utils.h>

namespace {

constexpr uint32_t kRecord = 1396;  // next 12 byte record: x, height, text
constexpr uint32_t kTimer = 1400;   // time left until the next spawn
constexpr uint32_t kSlots = 372;    // 32 lines of 32 bytes; free when negative
constexpr uint32_t kSlotStride = 32;
constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kSlotLife = 4;
constexpr uint32_t kSlotVelocityY = 28;
constexpr uint32_t kScreenHeight = 0x82465F4Eu;
constexpr uint32_t kScenario = 0x8243CB69u;  // big endian, unaligned

float Ps3Speed(uint32_t scenario) {
  switch (scenario) {
    case 8021: return 0.20333333f;
    case 8022: return 0.211f;
    case 8023: return 0.15f;
    default: return 0.37333333f;
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82132100);

REX_HOOK_RAW(sub_82132100) {
  const uint32_t task = ctx.r3.u32;
  const uint32_t record = REX_LOAD_U32(task + kRecord);
  if (!eternalsonata::IsPs3Target() || !record || !REX_LOAD_U32(record)) {
    __imp__sub_82132100(ctx, base);
    return;
  }
  const auto height = static_cast<int32_t>(REX_LOAD_U32(record + 4));
  const uint32_t text = REX_LOAD_U32(record + 8);
  uint32_t slot = 0;
  for (uint32_t i = 0; i < kSlotCount && !slot; ++i) {
    const uint32_t candidate = task + kSlots + i * kSlotStride;
    if (static_cast<int32_t>(REX_LOAD_U32(candidate)) < 0)
      slot = candidate;
  }

  __imp__sub_82132100(ctx, base);

  uint32_t scenario = 0;
  for (uint32_t i = 0; i < 4; ++i)
    scenario = (scenario << 8) | REX_LOAD_U8(kScenario + i);
  const float speed = Ps3Speed(scenario);
  REX_STORE_U32(task + kTimer, std::bit_cast<uint32_t>(float(height + 8) / speed));
  if (!text || !slot || static_cast<int32_t>(REX_LOAD_U32(slot)) < 0)
    return;
  const int32_t screen = REX_LOAD_U16(kScreenHeight);
  const auto life = static_cast<int32_t>(float(screen + height) / speed);
  REX_STORE_U32(slot + kSlotLife, std::bit_cast<uint32_t>(float(life)));
  REX_STORE_U32(slot + kSlotVelocityY, std::bit_cast<uint32_t>(-speed));
}
