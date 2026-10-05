// eternalsonata: Characters 11 and 12 on the status page.

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "party_system.h"
#include "ps3_appkeep.h"
#include "target.h"

// AppKeep image ids are the slot plus one.
extern "C++" void PartyStatusPortrait(PPCRegister &image,
                                      PPCRegister &character) {
  if (character.u32 > 10 && character.u32 <= 12 && eternalsonata::IsPs3Target())
    image.u64 = eternalsonata::kPs3MenuPortraitSlot + character.u32 - 10;
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
