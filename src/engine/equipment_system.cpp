// eternalsonata - Equipment: what each character is wearing, and the
// mod-facing API.
//
// Everything here was derived from the retail xex; docs/equipment.md is the
// long-form write-up, and the short version is:
//
//   * A character's equipment ids live in its stat struct at +0x1C, four u16
//     slots, in both of the parallel 48-byte arrays party_system.cpp
//     documents: the character's own stats at 0x8243FEE8 and the
//     equipment-adjusted copy the screens draw at 0x8243FD08. Only the first
//     is authoritative; sub_821E7898 copies it across on every recompute.
//
//   * Four slots, not five. sub_821E7898 does loop five times, but the fifth
//     entry of the array it walks is hardwired to zero and only +0x1C..+0x22
//     are ever loaded into it. The halfwords after that (+0x24..+0x2A) are the
//     character's equipped magic, written by sub_82231F30's slots 4..7: a
//     separate id space with its own table, handled by the magic half of this
//     file.
//
//   * Equipped magic lives in the base array ONLY. sub_821E7898 copies
//     +0x1C..+0x22 across to the live struct and stops there, so the live
//     copy's +0x24..+0x2A are never maintained and must not be read. Four
//     slots: light and dark cast by pressing the button, then light and dark
//     cast by holding it, so the even slots take kind 2 and the odd ones kind
//     3 (sub_82230ED0 asks for `slot % 2 ? 3 : 2`).
//
//   * A magic record is 12 bytes at 0x82015380 keyed by id - 1, holding the
//     owning character, that character's ordinal for the entry, the kind, the
//     level it is learned at and the echo cost. That table and the display
//     order are static image data and stay readable for the whole run; the
//     record COUNT is not, because the object holding it (dword_824400E0) is
//     allocated when the menu opens and freed when it closes, so the count is
//     latched and falls back to a static bound. sub_821E93B0 builds the
//     game's own list from exactly those fields, in the display order held at
//     word_8202C8A8 (u16[11] per character), and that filter is reproduced by
//     EternalSonataGetAvailableMagic below.
//
//   * sub_82231F30 is the game's magic commit and is not called here: the
//     write itself is one halfword and the rest of that routine repaints the
//     equipment screen. Its one non-obvious behaviour is reproduced: while the
//     u32 at 0x8243F3E8 is below 3, writing a press slot mirrors into the
//     matching hold slot, so the two cannot differ.
//
//   * Whether a character may wear an item is the u32 flag word at +0x04 of
//     its master entity table record (0x82017630, stride 100, keyed by
//     id - 1): bit 2 means "this is equipment" and bit 2 + c means "character
//     c can wear it". Which slot it goes in is the record's category at +0x03,
//     the same field the Item API reports: 1 weapon, 2 armor, 3 accessory,
//     with both accessory slots taking the same items. sub_821FC9A0 asks
//     exactly these two questions when it builds the equipment screen's list.
//
//   * Equipping is sub_821E8390(character, slot, id): it writes the id into
//     the character's own stats, recomputes, hands whatever was there back to
//     the inventory with sub_821FBFC0, and takes the new one out of it with
//     sub_821FC330. So worn equipment is NOT in the inventory, which is the
//     one behavioural difference from the Item Set, where a registered item
//     stays in the table with its `reserved` byte bumped.
//
//   * Unequipping is done here rather than through the game's own
//     sub_822352F8, which acts on dword_8243F360 (the menu's shared "current
//     character" scratch that a mod has no business setting) and repaints the
//     equipment screen as its last step. The three steps that matter - give
//     the item back, zero the id in both structs, recompute - are reproduced
//     below in the same order.
//
// Threading: the exported entry points are called from mods, i.e. usually from
// the ImGui draw thread, where there is no guest ThreadState and a guest call
// crashes the game (see guest_main_thread.h). Reads are plain guest-memory
// loads and answer immediately; anything that has to run guest code is queued
// onto the guest main thread and reports ETERNALSONATA_EQUIPMENT_QUEUED.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "equipment_system.h"
#include "eternalsonata_equipment_api.h"
#include "eternalsonata_item_api.h"
#include "guest_main_thread.h"
#include "item_system.h"

// The Item API's text readers, implemented in item_system.cpp. Its header
// declares the call shapes as function pointers, for mods that resolve them
// out of the executable; inside the executable they are ordinary exports and
// the equipment API's own name lookups forward straight to them.
extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetItemName(int item_id);
extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetItemDescription(int item_id);

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// The two parallel 48-byte stat arrays, indexed by character number - 1. See
// docs/party-system.md; party_system.cpp names the same two.
constexpr uint32_t kBaseStatsAddr = 0x8243FEE8u;
constexpr uint32_t kLiveStatsAddr = 0x8243FD08u;
constexpr uint32_t kStatsStride = 48u;

// Offsets within a stat struct, all confirmed against sub_821E7898.
constexpr uint32_t kStatHp = 0x0Cu;      // u32, current
constexpr uint32_t kStatHpMax = 0x10u;   // u32
constexpr uint32_t kStatAttack = 0x14u;  // u16
constexpr uint32_t kStatMagic = 0x16u;   // u16
constexpr uint32_t kStatDefense = 0x18u;  // u16
constexpr uint32_t kStatSpeed = 0x1Au;   // u16
constexpr uint32_t kStatEquipment = 0x1Cu;  // u16[4]
constexpr uint32_t kStatLevel = 0x00u;      // u32
// Equipped magic, base array only. See the note at the top.
constexpr uint32_t kStatMagicSlots = 0x24u;  // u16[4]

// The magic table: 12-byte records keyed by magic id - 1.
constexpr uint32_t kMagicTableAddr = 0x82015380u;
constexpr uint32_t kMagicStride = 12u;
constexpr uint32_t kMagicCharacter = 0x00u;  // u16, 1..10
constexpr uint32_t kMagicOrdinal = 0x02u;    // u16
constexpr uint32_t kMagicKind = 0x04u;       // u16, 1 none, 2 light, 3 dark
constexpr uint32_t kMagicLevel = 0x06u;      // u16
constexpr uint32_t kMagicCost = 0x08u;       // u16, 0 means "not castable"
// How many records are actually loaded: u16 at +16 of the object dword_824400E0
// points at. Bounded by the hard cap the API advertises.
constexpr uint32_t kMagicCountPtrAddr = 0x824400E0u;
constexpr uint32_t kMagicCountOffset = 16u;

// The display order the game's list uses: u16[11] per character, magic ids in
// the order the screen lists them, zero padded.
constexpr uint32_t kMagicOrderAddr = 0x8202C8A8u;
constexpr uint32_t kMagicPerCharacter = 11u;

// The BTX text blocks for magic, both keyed by magic id - 1. Not the item
// blocks, and read through the item system's shared reader.
constexpr uint32_t kMagicNameBlockAddr = 0x8230F640u;
constexpr uint32_t kMagicDescriptionBlockAddr = 0x822FF598u;

// Party base +0, the word before the party level. sub_82231F30 mirrors the
// press slots into the hold slots while this is below 3. What it counts is
// unidentified; only the comparison matters here.
constexpr uint32_t kMagicIndependenceAddr = 0x8243F3E8u;
constexpr uint32_t kMagicIndependenceThreshold = 3u;

// The master entity table, shared with items and characters.
constexpr uint32_t kMasterTableAddr = 0x82017630u;
constexpr uint32_t kMasterStride = 100u;
constexpr uint32_t kMasterCount = ETERNALSONATA_EQUIPMENT_ITEM_ID_MAX;
constexpr uint32_t kMasterBytes = kMasterStride * kMasterCount;
constexpr uint32_t kMasterCategory = 0x03u;  // u8
constexpr uint32_t kMasterFlags = 0x04u;     // u32
// The additive half of an item's contribution. +0x16 and +0x18 feed the two
// stats at +0x2C / +0x2E that no screen draws, so they are read but not
// reported.
constexpr uint32_t kMasterHp = 0x10u;       // u32
constexpr uint32_t kMasterAttack = 0x14u;   // u16
constexpr uint32_t kMasterMagic = 0x1Au;    // u16
constexpr uint32_t kMasterDefense = 0x1Cu;  // u16
constexpr uint32_t kMasterSpeed = 0x1Eu;    // u16
// The multiplicative half: five floats, each contributing (value - 1.0) to a
// running total that starts at 1.0. An item that does not scale a stat stores
// 1.0, which is what makes the neutral case a no-op.
constexpr uint32_t kMasterHpScale = 0x44u;
constexpr uint32_t kMasterAttackScale = 0x48u;
constexpr uint32_t kMasterMagicScale = 0x4Cu;
constexpr uint32_t kMasterDefenseScale = 0x50u;
constexpr uint32_t kMasterSpeedScale = 0x54u;

// The inventory, and the object both inventory routines take as their first
// argument. Same table item_system.cpp reads.
constexpr uint32_t kInventoryObject = 0x8255EED8u;
constexpr uint32_t kInventoryAddr = 0x8255EF08u;
constexpr uint32_t kInventoryStride = 4u;
constexpr uint32_t kInventoryCapacity = 512u;
constexpr uint32_t kInventoryBytes = kInventoryStride * kInventoryCapacity;

// The BTX text blocks, both keyed by item id - 1. Resolved through the Item
// API's own exported readers rather than duplicating ReadBtxString here.
// (See EternalSonataGetEquipmentName below.)

// Guest routines. All are called through the typed imports below, never by
// address, so the recompiler resolves them at link time.
//   sub_821E8390(c, slot, id)   the equipment screen's commit: writes the id,
//                               recomputes, returns the old occupant to the
//                               inventory and takes the new one out of it
//   sub_821E7898(c, live)       recompute live stats from own stats plus
//                               equipment
//   sub_821FBFC0(db, id, n, r)  give N of an entity back to the inventory
REX_IMPORT(__imp__sub_821E8390, g_equip, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_821E7898, g_refresh_stats, u32(u32, u32));
REX_IMPORT(__imp__sub_821FBFC0, g_own_entity, u32(u32, u32, u32, u32));

constexpr int kCharacterMin = ETERNALSONATA_EQUIPMENT_CHARACTER_MIN;
constexpr int kCharacterMax = ETERNALSONATA_EQUIPMENT_CHARACTER_MAX;
constexpr int kSlotCount = ETERNALSONATA_EQUIPMENT_SLOT_COUNT;
constexpr int kMagicSlotCount = ETERNALSONATA_MAGIC_SLOT_COUNT;

// The category an item must have to fit each slot, from sub_821FC9A0.
constexpr int kSlotCategory[kSlotCount] = {1, 2, 3, 3};

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Last observed state, for the event poll. `g_have_snapshot` is false before
// the first tick and after a load, which is what makes a restored save adopt
// silently instead of republishing everything it holds.
bool g_have_snapshot = false;
std::array<std::array<uint16_t, kSlotCount>, kCharacterMax + 1> g_worn{};
std::array<std::array<uint16_t, kMagicSlotCount>, kCharacterMax + 1> g_magic{};

// Both are bounds on the magic id space, cached because the live one comes and
// goes with a menu and the static one never changes. See MagicCountBound.
uint32_t g_magic_count_seen = 0;
uint32_t g_magic_order_bound = 0;

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

float ReadGuestFloat(uint32_t address) {
  const uint32_t bits = ReadGuest<uint32_t>(address);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

bool Bound() { return g_runtime != nullptr; }

uint32_t BaseStruct(int character) {
  return kBaseStatsAddr + kStatsStride * static_cast<uint32_t>(character - 1);
}

uint32_t LiveStruct(int character) {
  return kLiveStatsAddr + kStatsStride * static_cast<uint32_t>(character - 1);
}

uint32_t MasterRecord(int item_id) {
  return kMasterTableAddr + static_cast<uint32_t>(item_id - 1) * kMasterStride;
}

uint32_t InventoryRecord(uint32_t slot) {
  return kInventoryAddr + slot * kInventoryStride;
}

// "Available" means a save is actually loaded, the same test party_system.cpp
// makes: the tables are mapped but blank at the title screen, and a loaded
// save always has a level on the first character.
bool Available() {
  return Bound() && Readable(kBaseStatsAddr, kStatsStride * kCharacterMax) &&
         Readable(kLiveStatsAddr, kStatsStride * kCharacterMax) &&
         Readable(kMasterTableAddr, kMasterBytes) && Readable(kInventoryAddr, kInventoryBytes) &&
         ReadGuest<uint32_t>(BaseStruct(1)) != 0;
}

bool ValidCharacter(int character) {
  return character >= kCharacterMin && character <= kCharacterMax;
}

bool ValidSlot(int slot) { return slot >= 0 && slot < kSlotCount; }

bool ValidItem(int item_id) {
  return item_id >= ETERNALSONATA_EQUIPMENT_ITEM_ID_MIN &&
         item_id <= ETERNALSONATA_EQUIPMENT_ITEM_ID_MAX;
}

uint32_t SlotAddr(uint32_t stat_struct, int slot) {
  return stat_struct + kStatEquipment + 2u * static_cast<uint32_t>(slot);
}

int WornItem(int character, int slot) {
  return ReadGuest<uint16_t>(SlotAddr(BaseStruct(character), slot));
}

int CategoryOf(int item_id) { return ReadGuestByte(MasterRecord(item_id) + kMasterCategory); }

// The eligibility test sub_821E8458 makes: bit 2 says "this is equipment",
// bit 2 + c says "character c may wear it". sub_821FAF08 writes the same test
// as (8 << (c - 1)) & flags, which is the same bit.
bool CanEquipLocked(int character, int item_id) {
  const uint32_t flags = ReadGuest<uint32_t>(MasterRecord(item_id) + kMasterFlags);
  return (flags & 4u) != 0 && (flags & (4u << character)) != 0;
}

// How many of `item_id` the player is carrying. The inventory is compacted, so
// this walks it the way the game's own scans do.
int HeldCount(int item_id) {
  for (uint32_t i = 0; i < kInventoryCapacity; ++i) {
    if (ReadGuest<uint16_t>(InventoryRecord(i)) == static_cast<uint16_t>(item_id)) {
      return ReadGuestByte(InventoryRecord(i) + 2u);
    }
  }
  return 0;
}

// The whole up-front half of an equip, made without running guest code so a
// mod gets a real answer instead of QUEUED followed by a silent refusal.
int CanEquipInSlotLocked(int character, int slot, int item_id) {
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  if (!ValidItem(item_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM;
  }
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  if (!CanEquipLocked(character, item_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE;
  }
  if (CategoryOf(item_id) != kSlotCategory[slot]) {
    return ETERNALSONATA_EQUIPMENT_ERR_WRONG_SLOT;
  }
  // Worn equipment is out of the inventory, so "the player holds one" is also
  // "it is not already on somebody".
  if (HeldCount(item_id) <= 0) {
    return ETERNALSONATA_EQUIPMENT_ERR_NOT_OWNED;
  }
  return ETERNALSONATA_EQUIPMENT_OK;
}

void ReadEquipment(int character, int slot, EternalSonataEquipment* out) {
  std::memset(out, 0, sizeof(*out));
  out->character = character;
  out->slot = slot;

  const int item_id = WornItem(character, slot);
  out->item_id = item_id;
  if (!ValidItem(item_id)) {
    out->item_id = 0;
    return;
  }

  const uint32_t record = MasterRecord(item_id);
  out->name_text_id = item_id - 1;
  out->description_text_id = item_id - 1;
  out->category = ReadGuestByte(record + kMasterCategory);
  out->hp = static_cast<int32_t>(ReadGuest<uint32_t>(record + kMasterHp));
  out->attack = ReadGuest<uint16_t>(record + kMasterAttack);
  out->magic = ReadGuest<uint16_t>(record + kMasterMagic);
  out->defense = ReadGuest<uint16_t>(record + kMasterDefense);
  out->speed = ReadGuest<uint16_t>(record + kMasterSpeed);
}

// ---------------------------------------------------------------------------
// Magic
// ---------------------------------------------------------------------------

uint32_t MagicRecord(int magic_id) {
  return kMagicTableAddr + static_cast<uint32_t>(magic_id - 1) * kMagicStride;
}

// The record count the game itself consults, or 0 when the object holding it
// is not up.
//
// That object is NOT permanent: it is allocated when the menu that needs it
// opens and freed when the menu comes down, so anything gated on it answers
// "unavailable" for most of the game. Everything else the magic half reads is
// static image data (the record table, the display order) or lives in the stat
// struct (the equipped ids), all of which stay readable, so the count is the
// only thing that has to survive the object going away. See MagicCountBound.
uint32_t LiveMagicCount() {
  const uint32_t object = ReadGuest<uint32_t>(kMagicCountPtrAddr);
  if (object == 0 || !Readable(object, kMagicCountOffset + 2u)) {
    return 0;
  }
  const uint32_t count = ReadGuest<uint16_t>(object + kMagicCountOffset);
  return std::min<uint32_t>(count, ETERNALSONATA_MAGIC_ID_MAX);
}

// The highest id the display-order table names, computed once off static image
// data. Every castable magic has a row there (that table is what the game's
// own list is built from, so an equipped id is always one of them), which
// makes this a safe upper bound to validate against when the live count is not
// readable.
uint32_t MagicOrderBound() {
  if (g_magic_order_bound != 0) {
    return g_magic_order_bound;
  }
  const uint32_t span = 2u * kMagicPerCharacter * kCharacterMax;
  if (!Readable(kMagicOrderAddr, span)) {
    return 0;
  }
  uint32_t highest = 0;
  for (uint32_t i = 0; i < kMagicPerCharacter * kCharacterMax; ++i) {
    const uint32_t id = ReadGuest<uint16_t>(kMagicOrderAddr + 2u * i);
    if (id > highest && id <= ETERNALSONATA_MAGIC_ID_MAX) {
      highest = id;
    }
  }
  g_magic_order_bound = highest;
  return highest;
}

// What ValidMagic range-checks against: the live count while the menu that
// owns it is up, the last live count seen once it is gone, and the static
// display-order bound before one has ever been seen. The record table itself
// never moves, so a bound is all that is needed to read it safely.
uint32_t MagicCountBound() {
  const uint32_t live = LiveMagicCount();
  if (live != 0) {
    g_magic_count_seen = live;
    return live;
  }
  if (g_magic_count_seen != 0) {
    return g_magic_count_seen;
  }
  return MagicOrderBound();
}

bool MagicAvailable() {
  const uint32_t count = MagicCountBound();
  return count != 0 && Readable(kMagicTableAddr, count * kMagicStride) &&
         Readable(kMagicOrderAddr, 2u * kMagicPerCharacter * kCharacterMax);
}

bool ValidMagicSlot(int slot) { return slot >= 0 && slot < kMagicSlotCount; }

// In range of the hard cap AND of the table that is actually loaded.
bool ValidMagic(int magic_id) {
  return magic_id >= ETERNALSONATA_MAGIC_ID_MIN &&
         magic_id <= ETERNALSONATA_MAGIC_ID_MAX &&
         static_cast<uint32_t>(magic_id) <= MagicCountBound();
}

// The even slots take light, the odd ones dark. sub_82230ED0 writes the same
// test as `slot % 2 ? 3 : 2`.
int KindForMagicSlot(int slot) {
  return (slot % 2) != 0 ? ETERNALSONATA_MAGIC_KIND_DARK : ETERNALSONATA_MAGIC_KIND_LIGHT;
}

uint32_t MagicSlotAddr(int character, int slot) {
  return BaseStruct(character) + kStatMagicSlots + 2u * static_cast<uint32_t>(slot);
}

int EquippedMagic(int character, int slot) {
  return ReadGuest<uint16_t>(MagicSlotAddr(character, slot));
}

// The three questions sub_821E93B0 asks of a record: is it this character's,
// is it castable, and is the character high enough level for it. The level
// comes from the LIVE struct, which is where that routine reads it.
bool MagicIsCastable(int magic_id) {
  const uint32_t record = MagicRecord(magic_id);
  return ReadGuest<uint16_t>(record + kMagicKind) != ETERNALSONATA_MAGIC_KIND_NONE &&
         ReadGuest<uint16_t>(record + kMagicCost) != 0;
}

bool MagicBelongsTo(int character, int magic_id) {
  return ReadGuest<uint16_t>(MagicRecord(magic_id) + kMagicCharacter) ==
         static_cast<uint16_t>(character);
}

bool HasLearnedMagicLocked(int character, int magic_id) {
  if (!MagicBelongsTo(character, magic_id) || !MagicIsCastable(magic_id)) {
    return false;
  }
  const uint32_t level = ReadGuest<uint32_t>(LiveStruct(character) + kStatLevel);
  return ReadGuest<uint16_t>(MagicRecord(magic_id) + kMagicLevel) <= level;
}

// True once the second pair of slots can differ from the first.
bool MagicSlotsIndependent() {
  return ReadGuest<uint32_t>(kMagicIndependenceAddr) >= kMagicIndependenceThreshold;
}

// The whole up-front half of a magic write, made without running guest code.
int CanSetMagicLocked(int character, int slot, int magic_id) {
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidMagicSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  if (!Available() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  // 0 empties the slot and is always allowed.
  if (magic_id == 0) {
    return ETERNALSONATA_EQUIPMENT_OK;
  }
  if (!ValidMagic(magic_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_MAGIC;
  }
  if (!MagicBelongsTo(character, magic_id) || !MagicIsCastable(magic_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_MAGIC_NOT_OWNED;
  }
  if (!HasLearnedMagicLocked(character, magic_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_MAGIC_NOT_LEARNED;
  }
  if (ReadGuest<uint16_t>(MagicRecord(magic_id) + kMagicKind) != KindForMagicSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_WRONG_MAGIC_KIND;
  }
  return ETERNALSONATA_EQUIPMENT_OK;
}

// Fills `out` from a magic record. `slot` is -1 for a table entry.
void ReadMagic(int character, int slot, int magic_id, EternalSonataMagic* out) {
  std::memset(out, 0, sizeof(*out));
  out->character = character;
  out->slot = slot;
  if (!ValidMagic(magic_id)) {
    return;
  }
  const uint32_t record = MagicRecord(magic_id);
  out->magic_id = magic_id;
  out->name_text_id = magic_id - 1;
  out->description_text_id = magic_id - 1;
  out->kind = ReadGuest<uint16_t>(record + kMagicKind);
  out->unlock_level = ReadGuest<uint16_t>(record + kMagicLevel);
  out->echo_cost = ReadGuest<uint16_t>(record + kMagicCost);
  out->ordinal = ReadGuest<uint16_t>(record + kMagicOrdinal);
}

// ---------------------------------------------------------------------------
// The preview
// ---------------------------------------------------------------------------
//
// A host-side reimplementation of sub_821E8458, which is the routine the
// game's own comparison arrows are drawn from. Reimplemented rather than
// called because the guest routine would have to be queued onto the guest
// thread and could not answer a mod's UI on the frame it asks, and because
// this is pure arithmetic over tables that are already being read here.
// Everything below - the order of the multiplies, the *10 / 10 rounding, the
// current-HP rescale and its round-half-up, the two different clamps - mirrors
// the disassembly instruction for instruction.

// The current-HP slot is index 5, and it and maximum HP clamp differently from
// the other four.
constexpr int kPreviewHpMax = 0;
constexpr int kPreviewHp = 5;

void ReadLiveStats(int character, EternalSonataEquipStats* out) {
  const uint32_t live = LiveStruct(character);
  std::memset(out, 0, sizeof(*out));
  out->hp_max = static_cast<int32_t>(ReadGuest<uint32_t>(live + kStatHpMax));
  out->attack = ReadGuest<uint16_t>(live + kStatAttack);
  out->magic = ReadGuest<uint16_t>(live + kStatMagic);
  out->defense = ReadGuest<uint16_t>(live + kStatDefense);
  out->speed = ReadGuest<uint16_t>(live + kStatSpeed);
  out->hp = static_cast<int32_t>(ReadGuest<uint32_t>(live + kStatHp));
}

// Applies the game's own two clamps: index 0 and 5 are HP and hold 1..999999,
// the other four are the capped stats and hold 0..999.
int32_t ClampPreview(int index, int32_t value) {
  if (index == kPreviewHpMax || index == kPreviewHp) {
    return std::clamp<int32_t>(value, 1, 999999);
  }
  return std::clamp<int32_t>(value, 0, 999);
}

// Fills `out` with the stats `character` would have with `ids` worn. Callers
// hold g_mutex.
void ComputePreviewLocked(int character, const uint16_t ids[kSlotCount],
                          EternalSonataEquipStats* out) {
  const uint32_t base = BaseStruct(character);

  uint32_t hp_max = ReadGuest<uint32_t>(base + kStatHpMax);
  auto attack = ReadGuest<uint16_t>(base + kStatAttack);
  auto magic = ReadGuest<uint16_t>(base + kStatMagic);
  auto defense = ReadGuest<uint16_t>(base + kStatDefense);
  auto speed = ReadGuest<uint16_t>(base + kStatSpeed);

  float hp_scale = 1.0f;
  float attack_scale = 1.0f;
  float magic_scale = 1.0f;
  float defense_scale = 1.0f;
  float speed_scale = 1.0f;

  for (int i = 0; i < kSlotCount; ++i) {
    const int item_id = ids[i];
    if (!ValidItem(item_id)) {
      continue;
    }
    const uint32_t record = MasterRecord(item_id);
    hp_max += ReadGuest<uint32_t>(record + kMasterHp);
    attack += ReadGuest<uint16_t>(record + kMasterAttack);
    magic += ReadGuest<uint16_t>(record + kMasterMagic);
    defense += ReadGuest<uint16_t>(record + kMasterDefense);
    speed += ReadGuest<uint16_t>(record + kMasterSpeed);
    hp_scale += ReadGuestFloat(record + kMasterHpScale) - 1.0f;
    attack_scale += ReadGuestFloat(record + kMasterAttackScale) - 1.0f;
    magic_scale += ReadGuestFloat(record + kMasterMagicScale) - 1.0f;
    defense_scale += ReadGuestFloat(record + kMasterDefenseScale) - 1.0f;
    speed_scale += ReadGuestFloat(record + kMasterSpeedScale) - 1.0f;
  }

  // The game scales by 10, truncates to an integer and divides by 10 again,
  // which rounds the product down to a tenth and then to a whole number.
  auto scaled = [](float sum, float scale) {
    return static_cast<int32_t>(static_cast<int32_t>(sum * scale * 10.0f) / 10);
  };

  std::memset(out, 0, sizeof(*out));
  out->hp_max = scaled(static_cast<float>(hp_max), hp_scale);
  out->attack = scaled(static_cast<float>(attack), attack_scale);
  out->magic = scaled(static_cast<float>(magic), magic_scale);
  out->defense = scaled(static_cast<float>(defense), defense_scale);
  out->speed = scaled(static_cast<float>(speed), speed_scale);

  // Current HP keeps its share of maximum HP. The denominator is the maximum
  // the character has right now, i.e. the live struct's, not the base one's.
  const auto base_hp = static_cast<float>(ReadGuest<uint32_t>(base + kStatHp));
  const auto live_max = static_cast<float>(ReadGuest<uint32_t>(LiveStruct(character) + kStatHpMax));
  float hp = live_max > 0.0f ? (base_hp / live_max) * static_cast<float>(out->hp_max) : base_hp;
  const float fraction = hp - static_cast<float>(static_cast<int32_t>(hp));
  if (fraction >= 0.5f) {
    hp += 1.0f - fraction;
  }
  out->hp = static_cast<int32_t>(hp);
  if (out->hp == 0) {
    out->hp = 1;
  }

  out->hp_max = ClampPreview(kPreviewHpMax, out->hp_max);
  out->attack = ClampPreview(1, out->attack);
  out->magic = ClampPreview(2, out->magic);
  out->defense = ClampPreview(3, out->defense);
  out->speed = ClampPreview(4, out->speed);
  out->hp = ClampPreview(kPreviewHp, out->hp);
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
  return ETERNALSONATA_EQUIPMENT_QUEUED;
}

int EquipOnGuestThread(int character, int slot, int item_id) {
  g_equip(static_cast<u32>(character), static_cast<u32>(slot), static_cast<u32>(item_id));
  return ETERNALSONATA_EQUIPMENT_OK;
}

// sub_822352F8 without the parts that belong to the equipment screen: give the
// item back, zero the id in both structs, recompute. The live struct is
// written as well as the base one even though the recompute copies the base
// across, so that a reader that looks between the two sees a consistent pair.
int UnequipOnGuestThread(int character, int slot) {
  int old_id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    old_id = WornItem(character, slot);
    if (old_id == 0) {
      return ETERNALSONATA_EQUIPMENT_ERR_SLOT_EMPTY;
    }
  }
  g_own_entity(kInventoryObject, static_cast<u32>(old_id), 1, 1);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteGuest<uint16_t>(SlotAddr(BaseStruct(character), slot), 0);
    WriteGuest<uint16_t>(SlotAddr(LiveStruct(character), slot), 0);
  }
  g_refresh_stats(static_cast<u32>(character), LiveStruct(character));
  return ETERNALSONATA_EQUIPMENT_OK;
}

// sub_82231F30 without its screen repaint: one halfword into the base array,
// plus the mirror it applies while the pairs are still locked together.
int SetMagicOnGuestThread(int character, int slot, int magic_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  WriteGuest<uint16_t>(MagicSlotAddr(character, slot), static_cast<uint16_t>(magic_id));
  if (slot < 2 && !MagicSlotsIndependent()) {
    WriteGuest<uint16_t>(MagicSlotAddr(character, slot + 2), static_cast<uint16_t>(magic_id));
  }
  return ETERNALSONATA_EQUIPMENT_OK;
}

int ClearMagicOnGuestThread(int character) {
  for (int slot = 0; slot < kMagicSlotCount; ++slot) {
    SetMagicOnGuestThread(character, slot, 0);
  }
  return ETERNALSONATA_EQUIPMENT_OK;
}

int UnequipAllOnGuestThread(int character) {
  for (int slot = 0; slot < kSlotCount; ++slot) {
    UnequipOnGuestThread(character, slot);
  }
  return ETERNALSONATA_EQUIPMENT_OK;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void PublishEquipmentEvent(const char* event_name, int character, int item_id) {
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
  payload.u64 = static_cast<uint64_t>(character);
  payload.f64 = static_cast<double>(item_id);
  registry->Publish(event_name, payload);
}

struct PendingEvent {
  const char* name;
  int character;
  int item_id;
};

// Runs once per guest frame off the mod registry's tick. Ten characters times
// four slots is forty halfword loads, which is nothing.
void Tick() {
  std::vector<PendingEvent> events;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Available()) {
      // Guest memory went away under us (shutdown, or back to the title
      // screen); start clean next time.
      g_have_snapshot = false;
      return;
    }

    std::array<std::array<uint16_t, kSlotCount>, kCharacterMax + 1> worn{};
    std::array<std::array<uint16_t, kMagicSlotCount>, kCharacterMax + 1> magic{};
    for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
      for (int slot = 0; slot < kSlotCount; ++slot) {
        worn[character][slot] = static_cast<uint16_t>(WornItem(character, slot));
      }
      for (int slot = 0; slot < kMagicSlotCount; ++slot) {
        magic[character][slot] = static_cast<uint16_t>(EquippedMagic(character, slot));
      }
    }

    if (g_have_snapshot) {
      for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
          const uint16_t before = g_worn[character][slot];
          const uint16_t after = worn[character][slot];
          if (before == after) {
            continue;
          }
          // A swap is both: the old item came off and the new one went on.
          if (before != 0) {
            events.push_back(
                {ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED, character, before});
          }
          if (after != 0) {
            events.push_back({ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED, character, after});
          }
        }
        // Magic is one event per slot, with the new id, or 0 when a slot was
        // emptied: nothing leaves an inventory, so there is no "came off"
        // half to report.
        for (int slot = 0; slot < kMagicSlotCount; ++slot) {
          if (g_magic[character][slot] != magic[character][slot]) {
            events.push_back({ETERNALSONATA_EQUIPMENT_EVENT_MAGIC_SET, character,
                              magic[character][slot]});
          }
        }
      }
    }

    g_magic = magic;
    g_worn = worn;
    g_have_snapshot = true;
  }

  for (const auto& event : events) {
    PublishEquipmentEvent(event.name, event.character, event.item_id);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindEquipmentSystem(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

void NotifyEquipmentSaveLoaded() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_equipment_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataEquipmentAbiVersion(void) {
  return ETERNALSONATA_EQUIPMENT_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsEquipmentSystemAvailable(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return Available() ? 1 : 0;
}

// --- Reading -----------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEquipment(
    int character, int slot, EternalSonataEquipment* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  ReadEquipment(character, slot, out);
  return ETERNALSONATA_EQUIPMENT_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAllEquipment(
    int character, EternalSonataEquipment* out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  if (max == 0) {
    return kSlotCount;
  }
  int written = 0;
  for (int slot = 0; slot < kSlotCount && written < max; ++slot) {
    ReadEquipment(character, slot, &out[written]);
    ++written;
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEquippedItem(int character, int slot) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  const int item_id = WornItem(character, slot);
  return ValidItem(item_id) ? item_id : 0;
}

// The names and descriptions are the Item API's, forwarded rather than
// duplicated: they are the same blocks keyed the same way, and item_system.cpp
// already caches every string it has resolved.
extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetEquipmentName(int item_id) {
  return EternalSonataGetItemName(item_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetEquipmentDescription(int item_id) {
  return EternalSonataGetItemDescription(item_id);
}

// --- What fits where ---------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCanEquip(int character, int item_id) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidItem(item_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound() || !Readable(kMasterTableAddr, kMasterBytes)) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  return CanEquipLocked(character, item_id) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEquipmentSlotForItem(int item_id) {
  using namespace eternalsonata;
  if (!ValidItem(item_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound() || !Readable(kMasterTableAddr, kMasterBytes)) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  if ((ReadGuest<uint32_t>(MasterRecord(item_id) + kMasterFlags) & 4u) == 0) {
    return ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE;
  }
  const int category = CategoryOf(item_id);
  for (int slot = 0; slot < kSlotCount; ++slot) {
    if (kSlotCategory[slot] == category) {
      return slot;
    }
  }
  return ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCanEquipInSlot(int character, int slot,
                                                                 int item_id) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return CanEquipInSlotLocked(character, slot, item_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEquippableItems(int character, int slot,
                                                                     int* ids_out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !ids_out)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  // One pass over the inventory, in the game's own order, asking the same two
  // questions sub_821FC9A0 asks: does this character's bit stand in the item's
  // flag word, and is its category the one this slot takes.
  int written = 0;
  for (uint32_t i = 0; i < kInventoryCapacity; ++i) {
    const auto item_id = static_cast<int>(ReadGuest<uint16_t>(InventoryRecord(i)));
    if (!ValidItem(item_id) || ReadGuestByte(InventoryRecord(i) + 2u) == 0) {
      continue;
    }
    if (!CanEquipLocked(character, item_id) || CategoryOf(item_id) != kSlotCategory[slot]) {
      continue;
    }
    if (max == 0) {
      ++written;
      continue;
    }
    if (written >= max) {
      break;
    }
    ids_out[written++] = item_id;
  }
  return written;
}

// --- Previewing --------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataPreviewEquip(int character, int slot,
                                                               int item_id,
                                                               EternalSonataEquipStats* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  if (item_id != 0 && !ValidItem(item_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  // Same shape as the guest routine: a preview of something the character
  // cannot wear answers with the stats it has now.
  if (item_id != 0 && !CanEquipLocked(character, item_id)) {
    ReadLiveStats(character, out);
    return ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE;
  }
  uint16_t ids[kSlotCount];
  for (int i = 0; i < kSlotCount; ++i) {
    ids[i] = static_cast<uint16_t>(WornItem(character, i));
  }
  ids[slot] = static_cast<uint16_t>(item_id);
  ComputePreviewLocked(character, ids, out);
  return ETERNALSONATA_EQUIPMENT_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEquipStats(int character,
                                                                EternalSonataEquipStats* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  ReadLiveStats(character, out);
  return ETERNALSONATA_EQUIPMENT_OK;
}

// --- Equipping ---------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataEquip(int character, int slot, int item_id) {
  using namespace eternalsonata;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int allowed = CanEquipInSlotLocked(character, slot, item_id);
    if (allowed != ETERNALSONATA_EQUIPMENT_OK) {
      return allowed;
    }
  }
  return RunOnGuestThread(
      [character, slot, item_id] { return EquipOnGuestThread(character, slot, item_id); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataUnequip(int character, int slot) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Available()) {
      return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
    }
    if (WornItem(character, slot) == 0) {
      return ETERNALSONATA_EQUIPMENT_ERR_SLOT_EMPTY;
    }
  }
  return RunOnGuestThread([character, slot] { return UnequipOnGuestThread(character, slot); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataUnequipAll(int character) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Available()) {
      return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
    }
  }
  return RunOnGuestThread([character] { return UnequipAllOnGuestThread(character); });
}

// --- Magic -------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetMagic(int character, int slot,
                                                            EternalSonataMagic* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidMagicSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  ReadMagic(character, slot, EquippedMagic(character, slot), out);
  return ETERNALSONATA_EQUIPMENT_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAllMagic(int character,
                                                               EternalSonataMagic* out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  if (max == 0) {
    return kMagicSlotCount;
  }
  int written = 0;
  for (int slot = 0; slot < kMagicSlotCount && written < max; ++slot) {
    ReadMagic(character, slot, EquippedMagic(character, slot), &out[written]);
    ++written;
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEquippedMagic(int character, int slot) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidMagicSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  const int magic_id = EquippedMagic(character, slot);
  return ValidMagic(magic_id) ? magic_id : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetMagicInfo(int magic_id,
                                                                EternalSonataMagic* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  if (!ValidMagic(magic_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_MAGIC;
  }
  const int owner = ReadGuest<uint16_t>(MagicRecord(magic_id) + kMagicCharacter);
  ReadMagic(owner, -1, magic_id, out);
  return ETERNALSONATA_EQUIPMENT_OK;
}

// Magic has text blocks of its own, so unlike the equipment names these cannot
// forward to the Item API; they go through the same reader on other blocks.
extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetMagicName(int magic_id) {
  using namespace eternalsonata;
  if (magic_id < ETERNALSONATA_MAGIC_ID_MIN || magic_id > ETERNALSONATA_MAGIC_ID_MAX) {
    return "";
  }
  return LookupBtxString(kMagicNameBlockAddr, magic_id - 1);
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetMagicDescription(int magic_id) {
  using namespace eternalsonata;
  if (magic_id < ETERNALSONATA_MAGIC_ID_MIN || magic_id > ETERNALSONATA_MAGIC_ID_MAX) {
    return "";
  }
  return LookupBtxString(kMagicDescriptionBlockAddr, magic_id - 1);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataHasLearnedMagic(int character, int magic_id) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  if (!ValidMagic(magic_id)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_MAGIC;
  }
  return HasLearnedMagicLocked(character, magic_id) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCanSetMagic(int character, int slot,
                                                               int magic_id) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return CanSetMagicLocked(character, slot, magic_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAvailableMagic(int character, int slot,
                                                                    int* ids_out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !ids_out)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT;
  }
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  if (!ValidMagicSlot(slot)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available() || !MagicAvailable()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  // sub_821E93B0's filter, walked in word_8202C8A8's display order so a mod's
  // list comes out in the same order the game's own does.
  const int kind = KindForMagicSlot(slot);
  const uint32_t row =
      kMagicOrderAddr + 2u * kMagicPerCharacter * static_cast<uint32_t>(character - 1);
  int written = 0;
  for (uint32_t i = 0; i < kMagicPerCharacter; ++i) {
    const auto magic_id = static_cast<int>(ReadGuest<uint16_t>(row + 2u * i));
    if (!ValidMagic(magic_id) || !HasLearnedMagicLocked(character, magic_id)) {
      continue;
    }
    if (ReadGuest<uint16_t>(MagicRecord(magic_id) + kMagicKind) != kind) {
      continue;
    }
    if (max == 0) {
      ++written;
      continue;
    }
    if (written >= max) {
      break;
    }
    ids_out[written++] = magic_id;
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataAreMagicSlotsIndependent(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
  }
  return MagicSlotsIndependent() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetMagic(int character, int slot, int magic_id) {
  using namespace eternalsonata;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int allowed = CanSetMagicLocked(character, slot, magic_id);
    if (allowed != ETERNALSONATA_EQUIPMENT_OK) {
      return allowed;
    }
  }
  // The write itself runs no guest code, but it is still queued onto the guest
  // thread so that it cannot land between the frame's reads of the struct it
  // writes into.
  return RunOnGuestThread(
      [character, slot, magic_id] { return SetMagicOnGuestThread(character, slot, magic_id); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataClearMagic(int character) {
  using namespace eternalsonata;
  if (!ValidCharacter(character)) {
    return ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Available() || !MagicAvailable()) {
      return ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE;
    }
  }
  return RunOnGuestThread([character] { return ClearMagicOnGuestThread(character); });
}
