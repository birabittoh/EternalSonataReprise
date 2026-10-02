// eternalsonata - The PS3's enemy table.
//
// The battle reads enemy n (1 based) from the 80 byte records at
// off_82024100: its model names, its ep%03d.bop and its voices. The 360 has
// 281, the PS3 336 (0x51052C in the EBOOT), with the new enemies of its new
// maps appended and 25 of the shared ones changed, so an encounter on a PS3
// only map reads past the 360 table. PS3 mode points the three readers at a
// guest copy of the PS3's.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

struct Enemy {
  uint32_t head;
  std::array<const char*, 6> models;
  std::array<uint32_t, 13> rest;
};

// Generated from the PS3 EBOOT at build time (scripts/ps3_enemy_table.py);
// empty without it. The last element only keeps the array valid then.
constexpr Enemy kPs3Enemies[] = {
#include "ps3_enemy_table.generated.inc"
    {},
};
constexpr size_t kEnemyCount = std::size(kPs3Enemies) - 1;
static_assert(kEnemyCount == 0 || kEnemyCount == 336);
constexpr uint32_t kRecordSize = 80;

std::once_flag g_once;
uint32_t g_table = 0;

void Store(uint8_t* at, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(at, value);
}

void BuildTable() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  if (!kEnemyCount) {
    REXLOG_ERROR("ps3 enemies: built without the PS3 EBOOT, keeping the 360 table");
    return;
  }

  std::unordered_map<std::string_view, uint32_t> names;
  uint32_t names_size = 0;
  for (size_t i = 0; i < kEnemyCount; ++i)
    for (const char* model : kPs3Enemies[i].models)
      if (names.emplace(model, names_size).second)
        names_size += static_cast<uint32_t>(std::strlen(model)) + 1;

  const uint32_t table_size = kRecordSize * static_cast<uint32_t>(kEnemyCount);
  const uint32_t table = memory->SystemHeapAlloc(table_size + names_size, 16);
  if (!table) {
    REXLOG_ERROR("ps3 enemies: could not allocate the enemy table");
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(table);
  for (const auto& [name, offset] : names) {
    std::memcpy(host + table_size + offset, name.data(), name.size());
    host[table_size + offset + name.size()] = 0;
  }
  for (size_t i = 0; i < kEnemyCount; ++i) {
    const Enemy& enemy = kPs3Enemies[i];
    uint8_t* record = host + kRecordSize * i;
    Store(record, enemy.head);
    for (size_t m = 0; m < enemy.models.size(); ++m)
      Store(record + 4 + 4 * m, table + table_size + names[enemy.models[m]]);
    for (size_t w = 0; w < enemy.rest.size(); ++w)
      Store(record + 28 + 4 * w, enemy.rest[w]);
  }
  g_table = table;
}

}  // namespace

// sub_821A0628, sub_821BD480 and sub_821BD778 each build the table's address
// in r11 before indexing it.
extern "C++" void EternalSonataPs3EnemyTable(PPCRegister& r11);

void EternalSonataPs3EnemyTable(PPCRegister& r11) {
  if (!eternalsonata::IsPs3Target())
    return;
  std::call_once(g_once, BuildTable);
  if (g_table)
    r11.u64 = g_table;
}
