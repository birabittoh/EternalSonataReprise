// eternalsonata: Characters 11 and 12 on the status page, and X changing
// the shown character's costume (docs/costumes.md).

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "costume_system.h"
#include "generated/eternalsonata_init.h"
#include "party_system.h"
#include "ps3_appkeep.h"
#include "ps3_natives.h"
#include "target.h"

// AppKeep image ids are the slot plus one.
extern "C++" void PartyStatusPortrait(PPCRegister &image,
                                      PPCRegister &character) {
  if (character.u32 > 10 && character.u32 <= 12 && eternalsonata::IsPs3Target()) {
    image.u64 = eternalsonata::kPs3MenuPortraitSlot + character.u32 - 10;
  } else if (const uint32_t portrait = eternalsonata::WornCostumePortrait(
                 static_cast<int>(character.u32))) {
    image.u64 = portrait;
  }
}

extern "C++" void PartyStatusNameSid(PPCRegister &sid) {
  if (sid.u32 > 10 && sid.u32 <= 12)
    sid.u64 = eternalsonata::PartyNameSid(static_cast<int>(sid.u32), false);
}

extern "C++" void PartyStatusSelection(PPCRegister &manager) {
  auto *runtime = rex::Runtime::instance();
  auto *memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  const auto load_u32 = [&](uint32_t address) {
    return rex::memory::load_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t *>(address));
  };
  const auto load_u8 = [&](uint32_t address) {
    return *memory->TranslateVirtual<uint8_t *>(address);
  };

  const uint32_t character = load_u32(0x8243F360u);
  if (character <= 10 || character > 12)
    return;

  for (uint32_t control = load_u32(manager.u32 + 392); control;
       control = load_u32(control + 48)) {
    if (load_u32(control) != 0)
      continue;
    const uint8_t selected = load_u8(control + 44);
    const uint8_t count = load_u8(control + 12);
    if (selected >= count || selected > 5)
      *memory->TranslateVirtual<uint8_t *>(control + 44) = 0;
    return;
  }
}

// sub_82232C10(character): redraws the page for one character.
// sub_821425D8(sound manager, cue, 0, 0): a menu sound.
REX_EXTERN(sub_82232C10);
REX_EXTERN(sub_821425D8);
REX_EXTERN(__imp__sub_82230658);

namespace {

constexpr uint32_t kMenuState = 0x824400E8u;
constexpr uint32_t kShownCharacter = 0x8243F360u;
constexpr uint32_t kSoundManager = 0x8243D89Cu;
constexpr uint32_t kButtonX = 0x4000u;
constexpr uint32_t kCursorSound = 7u;

}  // namespace

// sub_82230658: the status page's input. Nothing there reads X.
REX_HOOK_RAW(sub_82230658) {
  const uint32_t menu = REX_LOAD_U32(kMenuState);
  const int character = static_cast<int>(REX_LOAD_U32(kShownCharacter));
  if (menu && static_cast<int32_t>(REX_LOAD_U32(menu + 444)) == -1 &&
      (REX_LOAD_U32(menu + 448) & kButtonX) &&
      !(eternalsonata::IsPs3Target() && eternalsonata::Ps3CostumesBlocked())) {
    const int next = eternalsonata::NextUnlockedCostume(character);
    if (next >= 0 && next != eternalsonata::WornCostume(character) &&
        eternalsonata::WearCostume(character, next) >= 0) {
      PPCContext call = ctx;
      call.r3.u64 = static_cast<uint32_t>(character);
      sub_82232C10(call, base);
      call = ctx;
      call.r3.u64 = REX_LOAD_U32(kSoundManager);
      call.r4.u64 = kCursorSound;
      call.r5.u64 = 0;
      call.r6.u64 = 0;
      sub_821425D8(call, base);
    }
  }
  __imp__sub_82230658(ctx, base);
}
