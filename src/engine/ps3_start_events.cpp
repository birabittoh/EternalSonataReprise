// eternalsonata - The PS3's starting events.
//
// sub_820FEDC0 (and sub_820FEC48) start a map and its event from two tables
// indexed by start kind, 0 being a new game. The PS3 opens a new game with its
// own prologue, E0005 on tnt03, and changed the other entries too; its tables
// are at 0x517EBC (maps) and 0x47C1C8 (events) in the EBOOT. PS3 mode swaps
// the PS3's values into the 360's tables before the first start.

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <array>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace {

constexpr uint32_t kMapTable = 0x8240C7A4u;    // off_8240C7A4
constexpr uint32_t kEventTable = 0x82081BC8u;  // dword_82081BC8

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

void PatchTables(uint8_t* base) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  for (uint32_t i = 0; i < kPs3Starts.size(); ++i) {
    const char* map = kPs3Starts[i].map;
    const uint32_t size = static_cast<uint32_t>(std::strlen(map)) + 1;
    const uint32_t name = memory->SystemHeapAlloc(size, 4);
    if (!name) {
      REXLOG_ERROR("ps3 starts: could not allocate a map name");
      return;
    }
    std::memcpy(memory->TranslateVirtual(name), map, size);
    REX_STORE_U32(kMapTable + 4 * i, name);
    REX_STORE_U32(kEventTable + 4 * i, kPs3Starts[i].event);
  }
  g_patched = true;
}

}  // namespace

REX_EXTERN(__imp__sub_820FEDC0);
REX_EXTERN(__imp__sub_820FEC48);

REX_HOOK_RAW(sub_820FEDC0) {
  if (!g_patched && eternalsonata::IsPs3Target())
    PatchTables(base);
  __imp__sub_820FEDC0(ctx, base);
}

REX_HOOK_RAW(sub_820FEC48) {
  if (!g_patched && eternalsonata::IsPs3Target())
    PatchTables(base);
  __imp__sub_820FEC48(ctx, base);
}
