// eternalsonata - Finish the battle's loads before its first frame.
//
// The battle draws nothing until its loader (manager + 0x40) sets the scene
// ready byte, which waits for every enemy texture job on the loader worker.
// sub_821A51A8 polls that once a frame, so a heavier enemy (the PS3's, ~60 ms of
// textures) leaves the transition's first frames with a cleared, empty world.
// Polling it here until ready trades those frames for one short pause.

#include "generated/eternalsonata_init.h"

#include <chrono>
#include <thread>

#include <rex/hook.h>

#include "battle_layout.h"

namespace {

namespace battle = eternalsonata::battle;

constexpr u32 kAppliedMode = 0x824C74C0u;  // ChangeMode task + 48
constexpr u32 kModeBattle = 4u;
constexpr u32 kSwapEffect = 0x824C74D0u;   // ChangeMode task + 64, set while it plays
constexpr u32 kSceneReady = 675u;  // loader byte, manager + 0x2E3
constexpr auto kMaxWait = std::chrono::milliseconds(500);

}  // namespace

REX_EXTERN(__imp__sub_821A51A8);

REX_HOOK_RAW(sub_821A51A8) {
  const u32 loader = ctx.r3.u32;
  PPCContext saved = ctx;
  __imp__sub_821A51A8(ctx, base);
  // Only while the swap into battle plays: the FSM parks on the same wait state
  // when a battle ends, with nothing left to load.
  static bool waited = false;
  if (REX_LOAD_U32(kSwapEffect) == 0) {
    waited = false;
    return;
  }
  if (waited || REX_LOAD_U32(kAppliedMode) != kModeBattle ||
      REX_LOAD_U32(battle::kManager + battle::kFsmStateOffset) != battle::kFsmStateWaitScene)
    return;
  waited = true;
  const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
  while (!REX_LOAD_U8(loader + kSceneReady) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    PPCContext again = saved;
    __imp__sub_821A51A8(again, base);
  }
}
