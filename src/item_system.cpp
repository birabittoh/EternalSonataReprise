// eternalsonata - Items: the player's inventory, the Item Set, and the
// mod-facing API.
//
// Everything here was derived from the retail xex; docs/items.md is the
// long-form write-up, and the short version is:
//
//   * The inventory is 512 fixed-size records at 0x8255EF08, four bytes each:
//     {u16 id, u8 count, u8 reserved}. `reserved` is how many of that stack
//     are spoken for by entries in the Item Set. The game keeps the table
//     compacted, so a released slot is closed up rather than left as a hole, and a
//     slot index is not a stable name for an item; the id is.
//
//   * Static per-item data lives in the master entity table at 0x82017630,
//     512 records of 100 bytes indexed by id - 1: category at +0x03, buy and
//     sell price at +0x08 and +0x0C, and the Item Set cost at +0x36. It is the
//     same table the party code reads, because characters and items share one
//     id space.
//
//   * The Item Set is word_8243FC3E, 32 u16 item ids kept compacted. It is
//     what the battle item menu is built from: sub_82189BA0 asks sub_821E6AF8
//     for a compacted copy and turns it straight into menu rows. Registering
//     an item is sub_821E6740, which charges the item's cost to the party
//     level's point budget (byte_8243FCC4 free / byte_8243FCC5 spent, capped
//     by word_8202CA70[level - 1] = 10, 10, 20, 30, 40, 50) and reserves one
//     held copy. sub_821E6D68 removes by id and sub_821E6958 by slot.
//
//   * dword_8243FCC0 is the entry count the screens read. sub_821E6740 does
//     not maintain it. The Item Set screen recounts the array afterwards
//     (sub_82225FE0), and so does every mutation here.
//
//   * Names and descriptions are BTX text blocks in the xex image, at
//     0x82376400 and 0x8233A3A8, both keyed by id - 1. They are read here
//     directly rather than through the guest's own sub_8223B780, which keeps
//     name lookups off the guest thread.
//
// Threading: the exported entry points are called from mods, i.e. usually from
// the ImGui draw thread, where there is no guest ThreadState and a guest call
// crashes the game (see guest_main_thread.h). Reads are plain guest-memory
// loads and answer immediately; anything that has to run guest code is queued
// onto the guest main thread and reports ETERNALSONATA_ITEM_QUEUED.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_item_api.h"
#include "guest_main_thread.h"
#include "item_system.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// The object both inventory routines take as their first argument. The table
// itself sits at +0x30 inside it, which is why the two addresses are 0x30
// apart.
constexpr uint32_t kInventoryObject = 0x8255EED8u;
// 512 records of {u16 id, u8 count, u8 reserved}.
constexpr uint32_t kInventoryAddr = 0x8255EF08u;
constexpr uint32_t kInventoryStride = 4u;
constexpr uint32_t kInventoryCapacity = ETERNALSONATA_INVENTORY_CAPACITY;
constexpr uint32_t kInventoryBytes = kInventoryStride * kInventoryCapacity;

// The master entity table: 512 records of 100 bytes, indexed by id - 1.
constexpr uint32_t kMasterTableAddr = 0x82017630u;
constexpr uint32_t kMasterStride = 100u;
constexpr uint32_t kMasterCount = ETERNALSONATA_ITEM_ID_MAX;
constexpr uint32_t kMasterBytes = kMasterStride * kMasterCount;
// Offsets within a master record, all confirmed against the routines that read
// them: sub_821FC5C8 buckets the item screen's tabs by +0x03, sub_8222E9C0
// moves money by +0x08 (buy) and +0x0C (sell), sub_8220EEE0 draws the icon
// from +0x02, and sub_821E6740 charges +0x36 to the budget.
constexpr uint32_t kMasterId = 0x00u;         // u16
constexpr uint32_t kMasterIcon = 0x02u;       // u8
constexpr uint32_t kMasterCategory = 0x03u;   // u8
constexpr uint32_t kMasterBuyPrice = 0x08u;   // u32
constexpr uint32_t kMasterSellPrice = 0x0Cu;  // u32
constexpr uint32_t kMasterCost = 0x36u;       // u16

// The Item Set: 32 u16 item ids, kept compacted.
constexpr uint32_t kItemSetAddr = 0x8243FC3Eu;
constexpr uint32_t kItemSetCapacity = ETERNALSONATA_ITEM_SET_CAPACITY;
constexpr uint32_t kItemSetBytes = 2u * kItemSetCapacity;
// The entry count the screens read. Recounted, never trusted as an input.
constexpr uint32_t kItemSetCountAddr = 0x8243FCC0u;

// The party level and its point budget.
constexpr uint32_t kPartyLevelAddr = 0x8243F3ECu;  // u32, 1..6
constexpr uint32_t kBudgetFreeAddr = 0x8243FCC4u;  // u8
constexpr uint32_t kBudgetUsedAddr = 0x8243FCC5u;  // u8
constexpr uint32_t kBudgetCapTableAddr = 0x8202CA70u;  // u16[6]
constexpr uint32_t kPartyLevelMax = 6u;

// BTX text blocks in the xex image, both keyed by item id - 1.
constexpr uint32_t kNameBlockAddr = 0x82376400u;
constexpr uint32_t kDescriptionBlockAddr = 0x8233A3A8u;
// u32 language index into a block's own language chain, 0..6 (JPN USA GBR FRA
// ITA DEU ESP).
constexpr uint32_t kLanguageIndexAddr = 0x8243D370u;

// Guest routines. All are called through the typed imports below, never by
// address, so the recompiler resolves them at link time.
//   sub_821FC1E8(db, id, count)   acquire: the game's own "you got an item"
//                                 path. Takes a slot, rebuilds the screens'
//                                 category lists, clamps the stack at 99
//   sub_821FC330(db, id, count)   discard: releases the slot when the last one
//                                 goes. Returns what is left, or -1 if the
//                                 player held none
//   sub_821E6740(id)              Item Set register. 0 ok / 1 set full /
//                                 2 budget too small / 3 none free to reserve
//   sub_821E6D68(id)              Item Set remove by id, refunding the cost
//   sub_821E6958(slot)            Item Set remove by slot, closing the gap
REX_IMPORT(__imp__sub_821FC1E8, g_acquire_item, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_821FC330, g_discard_item, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_821E6740, g_set_add, u32(u32));
REX_IMPORT(__imp__sub_821E6D68, g_set_remove_id, u32(u32));
REX_IMPORT(__imp__sub_821E6958, g_set_remove_slot, u32(u32));

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Last observed state, for the event poll. `g_have_snapshot` is false before
// the first tick and after a load, which is what makes a restored save adopt
// silently instead of republishing everything it holds.
bool g_have_snapshot = false;
std::array<uint8_t, kMasterCount + 1> g_held{};      // count per item id
std::array<uint8_t, kMasterCount + 1> g_in_set{};    // set entries per item id

// Resolved BTX strings, keyed by {block, text id}. Populated on demand and
// never evicted: the API hands out the char* and promises it stays valid.
std::mutex g_text_mutex;
std::map<std::pair<uint32_t, int>, std::string> g_text;

rex::memory::Memory* Mem() { return g_runtime ? g_runtime->memory() : nullptr; }

// ---------------------------------------------------------------------------
// Guest memory access
// ---------------------------------------------------------------------------

bool Readable(uint32_t address, uint32_t span) {
  auto* memory = Mem();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + span - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

template <typename T>
T ReadGuest(uint32_t address, T fallback = T{}) {
  auto* memory = Mem();
  if (!memory) {
    return fallback;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? rex::memory::load_and_swap<T>(host) : fallback;
}

template <typename T>
void WriteGuest(uint32_t address, T value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    rex::memory::store_and_swap<T>(host, value);
  }
}

uint8_t ReadGuestByte(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : uint8_t{0};
}

// Reads a NUL-terminated guest string, stopping at `limit` bytes so a blob
// that has been trampled cannot run away.
std::string ReadGuestString(uint32_t address, size_t limit = 1024) {
  auto* memory = Mem();
  if (!memory || address == 0) {
    return {};
  }
  auto* host = memory->TranslateVirtual<const char*>(address);
  if (!host) {
    return {};
  }
  size_t length = 0;
  while (length < limit && host[length] != '\0') {
    ++length;
  }
  return std::string(host, length);
}

// ---------------------------------------------------------------------------
// BTX text
// ---------------------------------------------------------------------------
//
// Layout, from sub_8223B780 and scripts/btx.py. All fields are u32 big-endian
// and every offset is relative to the struct it was read from:
//
//   header  +0x00 'BTX '   +0x04 -> first language block   +0x0C block count
//   block   +0x00 fourcc   +0x04 -> entry table   +0x08 -> next block
//                          +0x10 entry count
//   entry   +0x00 text id  +0x04 -> string
//
// The block chain does not self-terminate, so the count is what bounds it.

// Resolves `text_id` in `block` for the language the game is running in.
// Returns "" for anything the block does not have. Caller holds no lock.
std::string ReadBtxString(uint32_t block, int text_id) {
  if (!Readable(block, 0x10u) || ReadGuest<uint32_t>(block) != 0x42545820u /* 'BTX ' */) {
    return {};
  }
  const uint32_t block_count = ReadGuest<uint32_t>(block + 0x0Cu);
  if (block_count == 0 || block_count > 16u) {
    return {};
  }

  uint32_t language = ReadGuest<uint32_t>(kLanguageIndexAddr);
  if (language >= block_count) {
    // A mod language borrows a shipped language's slot, so this should not
    // happen; fall back to the first block rather than reading past the chain.
    language = 0;
  }

  uint32_t entry_block = block + ReadGuest<uint32_t>(block + 0x04u);
  for (uint32_t i = 0; i < language; ++i) {
    if (!Readable(entry_block, 0x14u)) {
      return {};
    }
    entry_block += ReadGuest<uint32_t>(entry_block + 0x08u);
  }
  if (!Readable(entry_block, 0x14u)) {
    return {};
  }

  const uint32_t table = entry_block + ReadGuest<uint32_t>(entry_block + 0x04u);
  const uint32_t count = ReadGuest<uint32_t>(entry_block + 0x10u);
  if (count == 0 || count > 0x10000u || !Readable(table, 8u * count)) {
    return {};
  }

  // Every block observed so far stores its entries in id order with no gaps,
  // so try the direct index before walking.
  auto entry_at = [&](uint32_t index) { return table + 8u * index; };
  const auto wanted = static_cast<uint32_t>(text_id);
  if (wanted < count && ReadGuest<uint32_t>(entry_at(wanted)) == wanted) {
    return ReadGuestString(entry_block + ReadGuest<uint32_t>(entry_at(wanted) + 4u));
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (ReadGuest<uint32_t>(entry_at(i)) == wanted) {
      return ReadGuestString(entry_block + ReadGuest<uint32_t>(entry_at(i) + 4u));
    }
  }
  return {};
}

// Never null. The string is owned by g_text and outlives every caller.
const char* CachedBtxString(uint32_t block, int text_id) {
  const auto key = std::make_pair(block, text_id);
  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    const auto it = g_text.find(key);
    if (it != g_text.end()) {
      return it->second.c_str();
    }
  }
  // Read outside the text lock: it touches guest memory, and a miss is cheap
  // enough that racing two readers onto the same id is not worth serialising.
  std::string value = ReadBtxString(block, text_id);
  std::lock_guard<std::mutex> lock(g_text_mutex);
  return g_text.emplace(key, std::move(value)).first->second.c_str();
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

bool Bound() { return g_runtime != nullptr; }

bool ItemsReadable() {
  return Bound() && Readable(kInventoryAddr, kInventoryBytes) &&
         Readable(kMasterTableAddr, kMasterBytes) && Readable(kItemSetAddr, kItemSetBytes) &&
         Readable(kBudgetFreeAddr, 2u) && Readable(kPartyLevelAddr, 4u);
}

bool ValidId(int item_id) {
  return item_id >= ETERNALSONATA_ITEM_ID_MIN && item_id <= ETERNALSONATA_ITEM_ID_MAX;
}

uint32_t MasterRecord(int item_id) {
  return kMasterTableAddr + static_cast<uint32_t>(item_id - 1) * kMasterStride;
}

uint32_t InventoryRecord(int slot) {
  return kInventoryAddr + static_cast<uint32_t>(slot) * kInventoryStride;
}

// The inventory slot holding `item_id`, or -1. The table is compacted, so the
// first empty id ends the search the same way the game's own scans end it -
// except that the game scans the whole table, and so does this, to stay
// correct if something ever leaves a hole.
int SlotOf(int item_id) {
  for (uint32_t i = 0; i < kInventoryCapacity; ++i) {
    if (ReadGuest<uint16_t>(InventoryRecord(static_cast<int>(i))) ==
        static_cast<uint16_t>(item_id)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int HeldCount(int item_id) {
  const int slot = SlotOf(item_id);
  return slot < 0 ? 0 : ReadGuestByte(InventoryRecord(slot) + 2u);
}

int ReservedCount(int item_id) {
  const int slot = SlotOf(item_id);
  return slot < 0 ? 0 : ReadGuestByte(InventoryRecord(slot) + 3u);
}

int ItemCost(int item_id) {
  return ReadGuest<uint16_t>(MasterRecord(item_id) + kMasterCost);
}

// How many entries of the set name `item_id`.
int SetEntryCount(int item_id) {
  int found = 0;
  for (uint32_t i = 0; i < kItemSetCapacity; ++i) {
    if (ReadGuest<uint16_t>(kItemSetAddr + 2u * i) == static_cast<uint16_t>(item_id)) {
      ++found;
    }
  }
  return found;
}

int SetCount() {
  int found = 0;
  for (uint32_t i = 0; i < kItemSetCapacity; ++i) {
    if (ReadGuest<uint16_t>(kItemSetAddr + 2u * i) != 0) {
      ++found;
    }
  }
  return found;
}

int PartyLevel() {
  const auto level = static_cast<int>(ReadGuest<uint32_t>(kPartyLevelAddr));
  return std::clamp(level, 1, static_cast<int>(kPartyLevelMax));
}

int BudgetTotal() {
  return ReadGuest<uint16_t>(kBudgetCapTableAddr + 2u * static_cast<uint32_t>(PartyLevel() - 1));
}

void ReadItem(int item_id, EternalSonataItem* out) {
  std::memset(out, 0, sizeof(*out));

  const uint32_t record = MasterRecord(item_id);
  const int slot = SlotOf(item_id);

  out->id = item_id;
  out->slot = slot;
  out->count = slot < 0 ? 0 : ReadGuestByte(InventoryRecord(slot) + 2u);
  out->reserved_count = slot < 0 ? 0 : ReadGuestByte(InventoryRecord(slot) + 3u);
  out->free_count = std::max(0, out->count - out->reserved_count);
  out->set_entry_count = SetEntryCount(item_id);

  out->category = ReadGuestByte(record + kMasterCategory);
  out->cost = ReadGuest<uint16_t>(record + kMasterCost);
  out->buy_price = static_cast<int32_t>(ReadGuest<uint32_t>(record + kMasterBuyPrice));
  out->sell_price = static_cast<int32_t>(ReadGuest<uint32_t>(record + kMasterSellPrice));
  out->icon_id = ReadGuestByte(record + kMasterIcon);

  out->name_text_id = item_id - 1;
  out->description_text_id = item_id - 1;
}

// The check sub_821E6740 makes, made up front so a mod gets a real answer
// instead of ETERNALSONATA_ITEM_QUEUED followed by a silent refusal. Same
// order as the guest routine, so the two always agree.
int CanAddToSetLocked(int item_id) {
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  if (SetCount() >= static_cast<int>(kItemSetCapacity)) {
    return ETERNALSONATA_ITEM_ERR_SET_FULL;
  }
  if (ReadGuestByte(kBudgetFreeAddr) < ItemCost(item_id)) {
    return ETERNALSONATA_ITEM_ERR_BUDGET;
  }
  if (HeldCount(item_id) - ReservedCount(item_id) <= 0) {
    return ETERNALSONATA_ITEM_ERR_NOT_OWNED;
  }
  return ETERNALSONATA_ITEM_OK;
}

// dword_8243FCC0 is what the screens read, and the routines that change the
// set do not all maintain it. The Item Set screen recounts the array after
// every change (sub_82225FE0); so does this, after every mutation.
void RecountSetLocked() {
  WriteGuest<uint32_t>(kItemSetCountAddr, static_cast<uint32_t>(SetCount()));
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

// Runs `work` on the guest main thread, inline if that is already where we
// are. See guest_main_thread.h.
int RunOnGuestThread(std::function<int()> work) {
  if (OnGuestMainThread()) {
    return work();
  }
  PostToGuestMainThread([work] { work(); });
  return ETERNALSONATA_ITEM_QUEUED;
}

// Guest thread only, g_mutex held by the caller for the memory reads.
int AddToSetOnGuestThread(int item_id) {
  const u32 result = g_set_add(static_cast<u32>(item_id));
  std::lock_guard<std::mutex> lock(g_mutex);
  RecountSetLocked();
  switch (result) {
    case 1:
      return ETERNALSONATA_ITEM_ERR_SET_FULL;
    case 2:
      return ETERNALSONATA_ITEM_ERR_BUDGET;
    case 3:
      return ETERNALSONATA_ITEM_ERR_NOT_OWNED;
    default:
      return ETERNALSONATA_ITEM_OK;
  }
}

int RemoveFromSetOnGuestThread(int item_id) {
  g_set_remove_id(static_cast<u32>(item_id));
  std::lock_guard<std::mutex> lock(g_mutex);
  RecountSetLocked();
  return ETERNALSONATA_ITEM_OK;
}

int RemoveSetSlotOnGuestThread(int slot) {
  const u32 result = g_set_remove_slot(static_cast<u32>(slot));
  std::lock_guard<std::mutex> lock(g_mutex);
  RecountSetLocked();
  return result == 1 ? ETERNALSONATA_ITEM_ERR_NOT_IN_SET : ETERNALSONATA_ITEM_OK;
}

int ClearSetOnGuestThread() {
  // Always slot 0: each removal closes the gap behind it, so the set walks
  // itself down to empty. Bounded by the capacity rather than by the count in
  // case a slot refuses to go.
  for (uint32_t i = 0; i < kItemSetCapacity; ++i) {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (SetCount() == 0) {
        break;
      }
    }
    g_set_remove_slot(0);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  RecountSetLocked();
  return ETERNALSONATA_ITEM_OK;
}

int GiveItemOnGuestThread(int item_id, int count) {
  g_acquire_item(kInventoryObject, static_cast<u32>(item_id), static_cast<u32>(count));
  return ETERNALSONATA_ITEM_OK;
}

int TakeItemOnGuestThread(int item_id, int count) {
  const auto remaining = static_cast<int>(
      g_discard_item(kInventoryObject, static_cast<u32>(item_id), static_cast<u32>(count)));
  if (remaining < 0) {
    return ETERNALSONATA_ITEM_ERR_NOT_OWNED;
  }

  // sub_821FC330 leaves the Item Set alone, so entries can outlive the copies
  // that backed them. The game's own sell path (sub_8222E9C0) drops one entry
  // per copy that went missing; do the same, or the set would claim to hold
  // items the player no longer has.
  int excess = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    excess = ReservedCount(item_id) - remaining;
  }
  for (int i = 0; i < excess; ++i) {
    g_set_remove_id(static_cast<u32>(item_id));
  }
  if (excess > 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    RecountSetLocked();
  }
  return ETERNALSONATA_ITEM_OK;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void PublishItemEvent(const char* event_name, int item_id, int value) {
  rex::Runtime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    runtime = g_runtime;
  }
  if (!runtime) {
    return;
  }
  auto* registry = runtime->mod_registry();
  if (!registry) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = static_cast<uint64_t>(item_id);
  payload.f64 = static_cast<double>(value);
  registry->Publish(event_name, payload);
}

// One pending event: which bus name, the item, and the count that goes with it.
struct PendingEvent {
  const char* name;
  int item_id;
  int value;
};

// Runs once per guest frame off the mod registry's tick.
void Tick() {
  std::vector<PendingEvent> events;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ItemsReadable()) {
      // Guest memory went away under us (shutdown, or back to the title
      // screen); start clean next time.
      g_have_snapshot = false;
      return;
    }

    std::array<uint8_t, kMasterCount + 1> held{};
    std::array<uint8_t, kMasterCount + 1> in_set{};
    for (uint32_t i = 0; i < kInventoryCapacity; ++i) {
      const uint32_t record = InventoryRecord(static_cast<int>(i));
      const uint16_t id = ReadGuest<uint16_t>(record);
      if (id >= ETERNALSONATA_ITEM_ID_MIN && id <= ETERNALSONATA_ITEM_ID_MAX) {
        held[id] = ReadGuestByte(record + 2u);
      }
    }
    for (uint32_t i = 0; i < kItemSetCapacity; ++i) {
      const uint16_t id = ReadGuest<uint16_t>(kItemSetAddr + 2u * i);
      if (id >= ETERNALSONATA_ITEM_ID_MIN && id <= ETERNALSONATA_ITEM_ID_MAX &&
          in_set[id] < 0xFFu) {
        ++in_set[id];
      }
    }

    if (g_have_snapshot) {
      for (int id = ETERNALSONATA_ITEM_ID_MIN; id <= ETERNALSONATA_ITEM_ID_MAX; ++id) {
        const auto slot = static_cast<size_t>(id);
        if (held[slot] != g_held[slot]) {
          events.push_back({held[slot] > g_held[slot] ? ETERNALSONATA_ITEM_EVENT_GAINED
                                                      : ETERNALSONATA_ITEM_EVENT_LOST,
                            id, held[slot]});
        }
        if (in_set[slot] != g_in_set[slot]) {
          events.push_back({in_set[slot] > g_in_set[slot] ? ETERNALSONATA_ITEM_EVENT_SET_ADDED
                                                          : ETERNALSONATA_ITEM_EVENT_SET_REMOVED,
                            id, in_set[slot]});
        }
      }
    }

    g_held = held;
    g_in_set = in_set;
    g_have_snapshot = true;
  }

  for (const auto& event : events) {
    PublishItemEvent(event.name, event.item_id, event.value);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

const char* LookupBtxString(uint32_t block, int text_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound()) {
    return "";
  }
  return CachedBtxString(block, text_id);
}

void BindItemSystem(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

void NotifyItemSaveLoaded() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_item_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataItemAbiVersion(void) {
  return ETERNALSONATA_ITEM_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsItemSystemAvailable(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return ItemsReadable() ? 1 : 0;
}

// --- Reading -----------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetItem(int item_id,
                                                          EternalSonataItem* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  ReadItem(item_id, out);
  return ETERNALSONATA_ITEM_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetOwnedItemCount(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  int count = 0;
  for (uint32_t i = 0; i < kInventoryCapacity; ++i) {
    if (ReadGuest<uint16_t>(InventoryRecord(static_cast<int>(i))) != 0) {
      ++count;
    }
  }
  return count;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetOwnedItems(EternalSonataItem* out,
                                                                int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  int written = 0;
  for (uint32_t i = 0; i < kInventoryCapacity; ++i) {
    const auto id = static_cast<int>(ReadGuest<uint16_t>(InventoryRecord(static_cast<int>(i))));
    if (!ValidId(id)) {
      continue;
    }
    if (max == 0) {
      ++written;
      continue;
    }
    if (written >= max) {
      break;
    }
    ReadItem(id, &out[written]);
    ++written;
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetItemCount(int item_id) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  return HeldCount(item_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetItemName(int item_id) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return "";
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound()) {
    return "";
  }
  return CachedBtxString(kNameBlockAddr, item_id - 1);
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetItemDescription(int item_id) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return "";
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound()) {
    return "";
  }
  return CachedBtxString(kDescriptionBlockAddr, item_id - 1);
}

// --- Giving and taking -------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGiveItem(int item_id, int count) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  if (count <= 0 || count > ETERNALSONATA_ITEM_STACK_MAX) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ItemsReadable()) {
      return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
    }
  }
  return RunOnGuestThread([item_id, count] { return GiveItemOnGuestThread(item_id, count); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataTakeItem(int item_id, int count) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  if (count <= 0 || count > ETERNALSONATA_ITEM_STACK_MAX) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ItemsReadable()) {
      return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
    }
    if (HeldCount(item_id) == 0) {
      return ETERNALSONATA_ITEM_ERR_NOT_OWNED;
    }
  }
  return RunOnGuestThread([item_id, count] { return TakeItemOnGuestThread(item_id, count); });
}

// --- The Item Set ------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetItemSet(EternalSonataItemSet* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  std::memset(out, 0, sizeof(*out));
  int written = 0;
  for (uint32_t i = 0; i < kItemSetCapacity; ++i) {
    const auto id = static_cast<int>(ReadGuest<uint16_t>(kItemSetAddr + 2u * i));
    if (id != 0) {
      out->entries[written++] = id;
    }
  }
  out->count = written;
  out->party_level = PartyLevel();
  out->budget_total = BudgetTotal();
  out->budget_used = ReadGuestByte(kBudgetUsedAddr);
  out->budget_free = ReadGuestByte(kBudgetFreeAddr);
  return ETERNALSONATA_ITEM_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetItemSetCount(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  return SetCount();
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetItemSetSlot(int slot) {
  using namespace eternalsonata;
  if (slot < 0 || slot >= static_cast<int>(kItemSetCapacity)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  return ReadGuest<uint16_t>(kItemSetAddr + 2u * static_cast<uint32_t>(slot));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsItemInSet(int item_id) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ItemsReadable()) {
    return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
  }
  return SetEntryCount(item_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCanAddItemToSet(int item_id) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return CanAddToSetLocked(item_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataAddItemToSet(int item_id) {
  using namespace eternalsonata;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int allowed = CanAddToSetLocked(item_id);
    if (allowed != ETERNALSONATA_ITEM_OK) {
      return allowed;
    }
  }
  return RunOnGuestThread([item_id] { return AddToSetOnGuestThread(item_id); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRemoveItemFromSet(int item_id) {
  using namespace eternalsonata;
  if (!ValidId(item_id)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ITEM;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ItemsReadable()) {
      return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
    }
    if (SetEntryCount(item_id) == 0) {
      return ETERNALSONATA_ITEM_ERR_NOT_IN_SET;
    }
  }
  return RunOnGuestThread([item_id] { return RemoveFromSetOnGuestThread(item_id); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRemoveItemSetSlot(int slot) {
  using namespace eternalsonata;
  if (slot < 0 || slot >= static_cast<int>(kItemSetCapacity)) {
    return ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ItemsReadable()) {
      return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
    }
    if (ReadGuest<uint16_t>(kItemSetAddr + 2u * static_cast<uint32_t>(slot)) == 0) {
      return ETERNALSONATA_ITEM_ERR_NOT_IN_SET;
    }
  }
  return RunOnGuestThread([slot] { return RemoveSetSlotOnGuestThread(slot); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataClearItemSet(void) {
  using namespace eternalsonata;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ItemsReadable()) {
      return ETERNALSONATA_ITEM_ERR_UNAVAILABLE;
    }
  }
  return RunOnGuestThread([] { return ClearSetOnGuestThread(); });
}
