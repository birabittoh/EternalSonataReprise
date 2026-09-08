// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's items: what the player is carrying, what each
// item is, and the Item Set, the short list of items that can actually be
// used in battle, paid for out of the party level's point budget.
//
// This exists so a mod never has to know a single guest address. The
// reverse-engineering behind it is written up in docs/items.md.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options API is used (see eternalsonata_options_api.h):
//
//     auto add = reinterpret_cast<EternalSonataAddItemToSetFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataAddItemToSet"));
//     if (add) { add(209); }  // Floral Powder
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataItemAbiVersion() before using anything added
// after version 1.
//
// Events. Changes are published on the shared mod registry bus
// (rex::system::ModRegistry, reached via runtime->mod_registry()), so a mod
// subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_ITEM_EVENT_GAINED      "eternalsonata.item.gained"
//     ETERNALSONATA_ITEM_EVENT_LOST        "eternalsonata.item.lost"
//     ETERNALSONATA_ITEM_EVENT_SET_ADDED   "eternalsonata.itemset.added"
//     ETERNALSONATA_ITEM_EVENT_SET_REMOVED "eternalsonata.itemset.removed"
//
// In all four the payload's `u64` is the item id and `f64` is how many the
// player holds (gained/lost) or how many entries in the set name that id
// (added/removed), after the change. `bytes` is empty. They fire for the
// game's own changes as well as for a mod's, on the frame after the change.
// Loading a save republishes nothing: whatever the save restores is adopted
// silently.
//
// Threading. Every entry point here is safe to call from any thread, including
// the ImGui draw thread. Reads answer from guest memory immediately. Writes
// have to run guest code, so they are queued onto the guest main thread and
// applied on its next frame, because guest calls need a live ThreadState that
// the draw thread does not have, and calling them from a draw hook crashes the
// game. Those functions therefore return ETERNALSONATA_ITEM_QUEUED rather than
// a final result, unless they are called from work already running on the
// guest main thread, in which case they run inline and return the real
// outcome. Everything that can be decided without running guest code (unknown
// item, nothing loaded, set full, budget too small, not owned) is still
// reported immediately, before anything is queued; use
// EternalSonataCanAddItemToSet to ask that question on its own.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_ITEM_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_ITEM_EVENT_GAINED "eternalsonata.item.gained"
#define ETERNALSONATA_ITEM_EVENT_LOST "eternalsonata.item.lost"
#define ETERNALSONATA_ITEM_EVENT_SET_ADDED "eternalsonata.itemset.added"
#define ETERNALSONATA_ITEM_EVENT_SET_REMOVED "eternalsonata.itemset.removed"

// Item ids are 1-based and index the game's master entity table, which has
// exactly 512 records. Characters share this id space with items.
#define ETERNALSONATA_ITEM_ID_MIN 1
#define ETERNALSONATA_ITEM_ID_MAX 512

// The inventory holds up to 512 distinct items, 99 of each.
#define ETERNALSONATA_INVENTORY_CAPACITY 512
#define ETERNALSONATA_ITEM_STACK_MAX 99

// The Item Set has 32 slots and the game offers no way to grow it. In practice
// the point budget runs out long before the slots do.
#define ETERNALSONATA_ITEM_SET_CAPACITY 32

// EternalSonataItem::category, the master table's own grouping. It is what
// decides which tab of the item screen an item appears under.
enum {
  ETERNALSONATA_ITEM_CATEGORY_CONSUMABLE = 0,
  ETERNALSONATA_ITEM_CATEGORY_WEAPON = 1,
  ETERNALSONATA_ITEM_CATEGORY_ARMOR = 2,
  ETERNALSONATA_ITEM_CATEGORY_ACCESSORY = 3
};

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_ITEM_OK = 0,
  // The change was accepted and will be applied on the guest thread's next
  // frame. See the threading note at the top.
  ETERNALSONATA_ITEM_QUEUED = 1,

  // Nothing is loaded yet, or the tables are not mapped (e.g. at the title
  // screen before a save is loaded). Every mutation refuses in this state.
  ETERNALSONATA_ITEM_ERR_UNAVAILABLE = -1,
  // The id is outside 1..512.
  ETERNALSONATA_ITEM_ERR_INVALID_ITEM = -2,
  // The player holds none of this item, or holds none that are not already
  // spoken for by an entry in the set.
  ETERNALSONATA_ITEM_ERR_NOT_OWNED = -3,
  // All 32 set slots are taken.
  ETERNALSONATA_ITEM_ERR_SET_FULL = -4,
  // The item costs more points than the party level's budget has left.
  ETERNALSONATA_ITEM_ERR_BUDGET = -5,
  // No entry in the set names this item.
  ETERNALSONATA_ITEM_ERR_NOT_IN_SET = -6,
  ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT = -10
};

// Everything the game knows about one item: the static half from the master
// entity table, and the live half from the player's inventory and Item Set.
// Filled for an item the player does not hold too, in which case the live half
// is zero and `slot` is -1.
typedef struct EternalSonataItem {
  // 1..512, the item's identity everywhere in the game.
  int32_t id;

  // Where the item sits in the 512-slot inventory table, or -1 if the player
  // holds none. Not stable: the game compacts the table whenever the contents
  // change, so use `id` to refer to an item and treat this as a debugging aid.
  int32_t slot;

  // How many the player holds, 0..99.
  int32_t count;
  // How many of those `count` are spoken for by entries in the Item Set. One
  // set entry reserves one copy, so an item held twice can be registered
  // twice.
  int32_t reserved_count;
  // count - reserved_count: how many could still be registered.
  int32_t free_count;
  // How many entries of the Item Set name this item. Normally equal to
  // reserved_count.
  int32_t set_entry_count;

  // Which tab of the item screen the item belongs to, one of the
  // ETERNALSONATA_ITEM_CATEGORY_* values above.
  int32_t category;

  // What one entry in the Item Set costs out of the party level's point
  // budget. Consumables cost 1..several points; weapons, armor and accessories
  // cost 0, since they are equipped rather than registered.
  int32_t cost;

  // Shop prices, in the game's own currency.
  int32_t buy_price;
  int32_t sell_price;

  // The item's icon, as the master table stores it (an index into the game's
  // icon id table, not a texture handle).
  int32_t icon_id;

  // Ids into the game's packed BTX text blocks, both equal to id minus 1. The
  // strings themselves are answered by EternalSonataGetItemName and
  // EternalSonataGetItemDescription; these are here for a mod that would
  // rather read the blocks itself.
  int32_t name_text_id;
  int32_t description_text_id;

  // 1 if this id names a real item (not a character or unused slot), 0 otherwise.
  int32_t is_real;
  // 1 if this id names a character (1..10), 0 otherwise.
  int32_t is_character;

  int32_t reserved[6];  // zero-filled; room for later additions
} EternalSonataItem;

// Custom item data: the static properties a mod provides when registering a new item.
// The game handles id assignment and allocation, so mods don't provide the id.
typedef struct EternalSonataCustomItemData {
  // Item name and description (copies are made, caller retains ownership).
  const char* name;
  const char* description;

  // Master table fields (icon, category, prices, cost).
  // `icon_id` indexes the game's own icon table and is clamped to 0..67;
  // easiest is to copy it from an existing item of the same kind.
  int32_t icon_id;
  int32_t category;
  int32_t buy_price;
  int32_t sell_price;
  int32_t cost;
} EternalSonataCustomItemData;

// The Item Set: the list of items usable in battle, and the party-level point
// budget that pays for it.
typedef struct EternalSonataItemSet {
  // The registered items in set order, `count` of them, zero-filled after
  // that. The order is the order the battle menu lists them in, and
  // EternalSonataAddItemToSet puts a new entry at the front.
  int32_t entries[ETERNALSONATA_ITEM_SET_CAPACITY];
  int32_t count;

  // The party level, 1..6, and its point budget. `budget_total` is the cap for
  // that level (10, 10, 20, 30, 40, 50: the "20P" the Item Set screen shows
  // at party level 3), `budget_used` is what the current entries cost and
  // `budget_free` is what is left to spend.
  int32_t party_level;
  int32_t budget_total;
  int32_t budget_used;
  int32_t budget_free;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataItemSet;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataItemAbiVersionFn)(void);

// True once the inventory, the Item Set and the master table are all readable.
// False before a save has been loaded or a new game started.
typedef int (*EternalSonataIsItemSystemAvailableFn)(void);

// ---------------------------------------------------------------------------
// Reading items
// ---------------------------------------------------------------------------

// Fills `out` with everything known about `item_id`, whether or not the player
// holds any. Returns ETERNALSONATA_ITEM_OK or a negative error.
typedef int (*EternalSonataGetItemFn)(int item_id, EternalSonataItem* out);

// How many distinct items the player is carrying, or a negative error.
typedef int (*EternalSonataGetOwnedItemCountFn)(void);

// Fills `out` with up to `max` of the items the player is carrying, in the
// game's own inventory order, and returns how many were written, or a negative
// error. Pass max = 0 to just count.
typedef int (*EternalSonataGetOwnedItemsFn)(EternalSonataItem* out, int max);

// How many of `item_id` the player holds, or a negative error. Shorthand for
// reading EternalSonataItem::count.
typedef int (*EternalSonataGetItemCountFn)(int item_id);

// The item's name and description in the language the game is running in,
// read out of its packed text blocks. Never null: an id the blocks have
// nothing for answers "". The returned pointer stays valid for the life of the
// process, so it can be held on to.
typedef const char* (*EternalSonataGetItemNameFn)(int item_id);
typedef const char* (*EternalSonataGetItemDescriptionFn)(int item_id);

// Whether `item_id` is a real item (as opposed to a character or an unused
// slot in the master table). Returns 1, 0, or a negative error.
typedef int (*EternalSonataIsRealItemFn)(int item_id);

// How many real items the master table holds (excluding characters and unused
// records), or a negative error.
typedef int (*EternalSonataGetItemCatalogCountFn)(void);

// Fills `out` with up to `max` real items in ascending id order and returns
// how many were written, or a negative error. Pass max = 0 to just count.
// Pass category = -1 to include every category, or an ETERNALSONATA_ITEM_CATEGORY_*
// value to filter to just that category.
typedef int (*EternalSonataGetItemCatalogFn)(EternalSonataItem* out, int max, int category);

// Registers a new custom item, which then behaves like any other item: it can
// be given, held, sold, listed under its category and put in the Item Set.
// An id is allocated from the master table's blank tail (403..510, so 108 at
// once) and returned, so mods do not have to worry about conflicts. Returns
// the allocated item id (> 0), or a negative error if registration fails.
typedef int (*EternalSonataRegisterCustomItemFn)(const EternalSonataCustomItemData* data);

// Unregisters a custom item, removing it from the catalog and making it
// unavailable for giving to the player. Returns ETERNALSONATA_ITEM_OK or a
// negative error.
typedef int (*EternalSonataUnregisterCustomItemFn)(int item_id);

// ---------------------------------------------------------------------------
// Giving and taking items
// ---------------------------------------------------------------------------

// Gives the player `count` of `item_id` through the game's own acquire path,
// so a new item takes an inventory slot, the screens' category lists are
// rebuilt and the stack clamps at 99. Returns ETERNALSONATA_ITEM_QUEUED, or a
// negative error decided up front.
typedef int (*EternalSonataGiveItemFn)(int item_id, int count);

// Takes `count` of `item_id` away, exactly as selling does: the inventory
// slot is released when the last one goes, and any Item Set entry that is left
// without a copy to back it is removed. Returns ETERNALSONATA_ITEM_QUEUED, or
// a negative error decided up front.
typedef int (*EternalSonataTakeItemFn)(int item_id, int count);

// ---------------------------------------------------------------------------
// The Item Set
// ---------------------------------------------------------------------------

// Fills `out` with the whole set and its budget. Returns
// ETERNALSONATA_ITEM_OK or a negative error.
typedef int (*EternalSonataGetItemSetFn)(EternalSonataItemSet* out);

// How many entries the set holds, 0..32, or a negative error.
typedef int (*EternalSonataGetItemSetCountFn)(void);

// The item id in set slot `slot` (0..31), 0 for an empty slot, or a negative
// error.
typedef int (*EternalSonataGetItemSetSlotFn)(int slot);

// How many entries of the set name `item_id`, or a negative error. 0 means the
// item cannot be used in battle.
typedef int (*EternalSonataIsItemInSetFn)(int item_id);

// Whether EternalSonataAddItemToSet would be accepted right now, without
// changing anything: ETERNALSONATA_ITEM_OK, or the error it would fail with
// (NOT_OWNED, SET_FULL, BUDGET, ...).
typedef int (*EternalSonataCanAddItemToSetFn)(int item_id);

// Registers one copy of `item_id` in the set, making it usable in battle. One
// held copy is reserved and the item's cost is charged to the party level's
// budget; the new entry goes to the front of the list, which is where the
// game's own Item Set screen puts it. Returns ETERNALSONATA_ITEM_QUEUED, or a
// negative error decided up front.
typedef int (*EternalSonataAddItemToSetFn)(int item_id);

// Removes one entry naming `item_id` from the set, refunding its cost and
// releasing the copy it reserved. The item stays in the inventory. Returns
// ETERNALSONATA_ITEM_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataRemoveItemFromSetFn)(int item_id);

// The same by position rather than by id: removes set slot `slot` (0..31) and
// closes the gap behind it. Returns ETERNALSONATA_ITEM_QUEUED, or a negative
// error decided up front.
typedef int (*EternalSonataRemoveItemSetSlotFn)(int slot);

// Empties the set, refunding the whole budget. Returns
// ETERNALSONATA_ITEM_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataClearItemSetFn)(void);

#ifdef __cplusplus
}  // extern "C"
#endif
