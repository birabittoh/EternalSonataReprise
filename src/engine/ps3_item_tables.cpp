// eternalsonata - The PS3's item and magic tables.
//
// The PS3 keeps the 360's item master (0x82017630) and magic (0x82015380)
// record layouts, filled for twelve characters: Crescendo's and Serenade's
// gear (items 402..431), their magic (119..132), and a rebalance of many
// shared records. PS3 mode writes its records over the 360's in place, so
// every reader sees them. The magic display order and the four item and magic
// text blocks have room for ten characters only, so their readers are pointed
// at guest copies instead.

#include "ps3_item_tables.h"

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <cstring>
#include <iterator>
#include <mutex>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

struct Ps3TextBlock {
  uint32_t block;
  const uint8_t* data;
  uint32_t size;
};

// Generated from the PS3 EBOOT at build time (scripts/ps3_item_tables.py).
#include "ps3_item_tables.generated.inc"

constexpr uint32_t kMasterTable = 0x82017630u;
constexpr uint32_t kMagicTable = 0x82015380u;
constexpr int kRetailBaseItemMax = 402;
constexpr int kPs3BaseItemMax = 431;
constexpr uint32_t kPs3MagicCount = 132;

#if ETERNALSONATA_PS3_ITEM_TABLES
constexpr bool kHaveTables = true;
#else
constexpr bool kHaveTables = false;
constexpr uint8_t kPs3MagicOrder[1] = {};
constexpr Ps3TextBlock kPs3TextBlocks[1] = {};
#endif

bool g_applied = false;

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

rex::memory::Memory* Memory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

std::once_flag g_copies_once;
uint32_t g_order = 0;
uint32_t g_text[std::size(kPs3TextBlocks)] = {};

uint32_t Copy(rex::memory::Memory* memory, const uint8_t* data, uint32_t size) {
  const uint32_t guest = memory->SystemHeapAlloc(size, 16);
  if (guest)
    std::memcpy(memory->TranslateVirtual(guest), data, size);
  return guest;
}

void MakeCopies() {
  auto* memory = Memory();
  if (!memory || !g_applied)
    return;
  g_order = Copy(memory, kPs3MagicOrder, sizeof(kPs3MagicOrder));
  for (size_t i = 0; i < std::size(kPs3TextBlocks); ++i)
    g_text[i] = Copy(memory, kPs3TextBlocks[i].data, kPs3TextBlocks[i].size);
}

}  // namespace

namespace eternalsonata {

void ApplyPs3ItemTables() {
  if (!IsPs3Target())
    return;
  if (!kHaveTables) {
    REXLOG_ERROR("ps3 items: built without the PS3 EBOOT, keeping the 360 tables");
    return;
  }
#if ETERNALSONATA_PS3_ITEM_TABLES
  auto* memory = Memory();
  if (!memory)
    return;
  if (!WriteReadOnly(memory, kMasterTable, kPs3MasterRecords, sizeof(kPs3MasterRecords)) ||
      !WriteReadOnly(memory, kMagicTable, kPs3MagicRecords, sizeof(kPs3MagicRecords))) {
    REXLOG_ERROR("ps3 items: could not write the item and magic tables");
    return;
  }
  g_applied = true;
#endif
}

uint32_t Ps3TextBlockFor(uint32_t block) {
  if (!g_applied)
    return 0;
  std::call_once(g_copies_once, MakeCopies);
  for (size_t i = 0; i < std::size(kPs3TextBlocks); ++i)
    if (kPs3TextBlocks[i].block == block)
      return g_text[i];
  return 0;
}

int BaseItemIdMax() {
  return g_applied ? kPs3BaseItemMax : kRetailBaseItemMax;
}

}  // namespace eternalsonata

// sub_821E51B0 sizes the magic list at the 360's 118 records.
extern "C++" void EternalSonataPs3MagicCount(PPCRegister& count);

void EternalSonataPs3MagicCount(PPCRegister& count) {
  if (g_applied)
    count.u64 = kPs3MagicCount;
}

// sub_821E93B0 orders a character's magic by word_8202C8A8, ten rows long.
extern "C++" void EternalSonataPs3MagicOrder(PPCRegister& table);

void EternalSonataPs3MagicOrder(PPCRegister& table) {
  if (!g_applied)
    return;
  std::call_once(g_copies_once, MakeCopies);
  if (g_order)
    table.u64 = g_order;
}
