// eternalsonata - The PS3's starting events.
//
// sub_820FEDC0 preloads a start's event, E%04d.e, and sub_820FEC48 loads its
// map, both indexed by start kind, 0 being a new game. dword_82081BC8 holds
// each start's event number, which sub_820FEC48 also reads as the spawn
// position; off_8240C7A4 the map, except that a new game's map is the
// hardcoded "tnt01.e". The PS3 opens a new game with its own prologue, E0005
// on tnt03, and changed the other starts too; its tables are at 0x517EBC
// (maps) and 0x47C1C8 (events) in the EBOOT. PS3 mode puts the PS3's values
// in place before the first start.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <array>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

constexpr uint32_t kMapTable = 0x8240C7A4u;     // off_8240C7A4
constexpr uint32_t kEventTable = 0x82081BC8u;   // dword_82081BC8, .rdata
constexpr uint32_t kNewGameMap = 0x82082C28u;   // "tnt01.e", .rdata; also kMapTable[0]

struct Start {
  const char* map;
  uint32_t event;
};

constexpr std::array<Start, 4> kPs3Starts = {{
    {"tnt03.e", 5},
    {"kts01.e", 1141},
    {"cbs60.e", 2231},
    {"agg02.e", 1231},
}};

bool g_patched = false;

// Writes over read-only guest data, restoring its protection after.
bool WriteReadOnly(rex::memory::Memory* memory, uint32_t addr, const void* data, uint32_t size) {
  auto* heap = memory->LookupHeap(addr);
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(addr, size,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect))
    return false;
  std::memcpy(memory->TranslateVirtual(addr), data, size);
  heap->Protect(addr, size, old_protect, nullptr);
  return true;
}

void PatchTables() {
  g_patched = true;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;

  // Same length as "tnt01.e", so the string is rewritten in place.
  if (!WriteReadOnly(memory, kNewGameMap, kPs3Starts[0].map, std::strlen(kPs3Starts[0].map) + 1))
    REXLOG_ERROR("ps3 starts: could not rewrite the new game map");

  std::array<uint8_t, 4 * kPs3Starts.size()> events{};
  for (uint32_t i = 0; i < kPs3Starts.size(); ++i) {
    const uint32_t event = kPs3Starts[i].event;
    events[4 * i + 0] = static_cast<uint8_t>(event >> 24);
    events[4 * i + 1] = static_cast<uint8_t>(event >> 16);
    events[4 * i + 2] = static_cast<uint8_t>(event >> 8);
    events[4 * i + 3] = static_cast<uint8_t>(event);
    if (i == 0)
      continue;
    const char* map = kPs3Starts[i].map;
    const uint32_t size = static_cast<uint32_t>(std::strlen(map)) + 1;
    const uint32_t name = memory->SystemHeapAlloc(size, 4);
    if (!name) {
      REXLOG_ERROR("ps3 starts: could not allocate a map name");
      return;
    }
    std::memcpy(memory->TranslateVirtual(name), map, size);
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(kMapTable + 4 * i), name);
  }
  if (!WriteReadOnly(memory, kEventTable, events.data(), static_cast<uint32_t>(events.size())))
    REXLOG_ERROR("ps3 starts: could not rewrite the event table");
}

}  // namespace

REX_EXTERN(__imp__sub_820FEDC0);
REX_EXTERN(__imp__sub_820FEC48);

REX_HOOK_RAW(sub_820FEDC0) {
  if (!g_patched && eternalsonata::IsPs3Target())
    PatchTables();
  __imp__sub_820FEDC0(ctx, base);
}

REX_HOOK_RAW(sub_820FEC48) {
  if (!g_patched && eternalsonata::IsPs3Target())
    PatchTables();
  __imp__sub_820FEC48(ctx, base);
}
