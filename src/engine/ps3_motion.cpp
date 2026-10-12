// eternalsonata - PS3 map object motions.
//
// sub_82118EE0 plays a motion only when its NMTN is version 0x82. The PS3
// stores map object motions (butterflies, grass, flowers) as 0x83 in the same
// layout, so they never ran. The 360 sampler already honours the 5x time flag
// at +41 those files set, so the version byte is the only difference.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <cstdint>

#include <rex/hook.h>

namespace {
constexpr uint32_t kNmtnTag = 0x4E4D544E;
}

REX_EXTERN(__imp__sub_82118EE0);

REX_HOOK_RAW(sub_82118EE0) {
  if (eternalsonata::IsPs3Target()) {
    const uint32_t table = REX_LOAD_U32(ctx.r3.u32 + 20);
    const int32_t index = static_cast<int32_t>(ctx.r4.u32);
    if (table && index >= 0 && static_cast<uint32_t>(index) < REX_LOAD_U32(ctx.r3.u32 + 24)) {
      const uint32_t motion = REX_LOAD_U32(table + 4u * static_cast<uint32_t>(index));
      if (motion && REX_LOAD_U32(motion) == kNmtnTag && REX_LOAD_U8(motion + 8) == 0x83)
        REX_STORE_U8(motion + 8, 0x82u);
    }
  }
  __imp__sub_82118EE0(ctx, base);
}
