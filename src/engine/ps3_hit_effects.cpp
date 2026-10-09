// eternalsonata - The PS3's hit effect table.
//
// sub_8218E480 picks the effects an attack shows on a hit from record n of
// unk_82078B00 (308 bytes: 25 AppKeep or BattleKeep slot pairs, then two
// floats, which sub_8218E558 reads). The 360 has 111 records, the PS3 119,
// so a new enemy's hit read past the table and indexed the AppKeep model
// table with a float. PS3 mode points both readers at a guest copy of the
// PS3's, its slots already in the 360's numbering.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <cstdint>
#include <iterator>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

constexpr uint32_t kRecordWords = 77;

// Extracted from the PS3 EBOOT by scripts/ps3_hit_effect_table.py.
constexpr uint32_t kPs3HitEffects[][kRecordWords] = {
#include "ps3_hit_effect_table.inc"
};
constexpr size_t kRecordCount = std::size(kPs3HitEffects);
static_assert(kRecordCount == 119);

std::once_flag g_once;
uint32_t g_table = 0;

void BuildTable() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  const uint32_t size = static_cast<uint32_t>(4 * kRecordWords * kRecordCount);
  const uint32_t table = memory->SystemHeapAlloc(size, 16);
  if (!table) {
    REXLOG_ERROR("ps3 hit effects: could not allocate the table");
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(table);
  for (size_t r = 0; r < kRecordCount; ++r)
    for (uint32_t w = 0; w < kRecordWords; ++w)
      rex::memory::store_and_swap<uint32_t>(host + 4 * (kRecordWords * r + w),
                                            kPs3HitEffects[r][w]);
  g_table = table;
}

uint32_t Ps3Table() {
  if (!eternalsonata::IsPs3Target())
    return 0;
  std::call_once(g_once, BuildTable);
  return g_table;
}

}  // namespace

// sub_8218E480 (twice) and sub_8218E558 build the table's address before
// indexing it.
extern "C++" void EternalSonataPs3HitEffectsR10(PPCRegister& r10);
extern "C++" void EternalSonataPs3HitEffectsR11(PPCRegister& r11);

void EternalSonataPs3HitEffectsR10(PPCRegister& r10) {
  if (const uint32_t table = Ps3Table())
    r10.u64 = table;
}

void EternalSonataPs3HitEffectsR11(PPCRegister& r11) {
  if (const uint32_t table = Ps3Table())
    r11.u64 = table;
}
