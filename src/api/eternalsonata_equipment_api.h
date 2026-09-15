// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's equipment: what each character is wearing in
// each of its four slots, what it could wear there, what a swap would do to
// its stats, and how to equip and unequip from a mod.
//
// It also covers the character's equipped magic, which the game stores in the
// same struct, immediately after the four equipment ids, and which its own
// equipment screen edits on a second page. Magic has its own id space and its
// own table, so it gets its own calls below rather than being folded into the
// equipment slots, but it is the same system from a mod's point of view and
// lives behind the same header and the same availability test.
//
// This is the companion to eternalsonata_item_api.h and shares its id space:
// a piece of equipment is an item, with a record in the same master entity
// table and a name in the same text blocks. The reverse-engineering behind it
// is written up in docs/equipment.md.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Item API is used (see eternalsonata_item_api.h):
//
//     auto equip = reinterpret_cast<EternalSonataEquipFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataEquip"));
//     if (equip) { equip(1, 0, 1); }  // Allegretto, weapon slot, Hunting Knife
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataEquipmentAbiVersion() before using anything
// added after version 1.
//
// Events. Changes are published on the shared mod registry bus
// (rex::system::ModRegistry, reached via runtime->mod_registry()), so a mod
// subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED   "eternalsonata.equipment.equipped"
//     ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED "eternalsonata.equipment.unequipped"
//     ETERNALSONATA_EQUIPMENT_EVENT_MAGIC_SET  "eternalsonata.equipment.magic_set"
//
// In all three the payload's `u64` is the character number and `f64` is the id
// that went into or came out of the slot: an item id for the first two and a
// magic id for the third, 0 when a magic slot was cleared. `bytes` is empty.
// The slot is not in the payload; read it back with
// EternalSonataGetAllEquipment or EternalSonataGetAllMagic if it matters.
// They fire for the game's own changes as well as for a mod's, on the frame
// after the change. Loading a save republishes nothing: whatever the save
// restores is adopted silently.
//
// Threading. Every entry point here is safe to call from any thread, including
// the ImGui draw thread. Reads answer from guest memory immediately, and so
// does EternalSonataPreviewEquip, which is host-side arithmetic over the same
// tables the game reads. Writes have to run guest code, so they are queued
// onto the guest main thread and applied on its next frame, because guest
// calls need a live ThreadState that the draw thread does not have, and
// calling them from a draw hook crashes the game. Those functions therefore
// return ETERNALSONATA_EQUIPMENT_QUEUED rather than a final result, unless
// they are called from work already running on the guest main thread, in which
// case they run inline and return the real outcome. Everything that can be
// decided without running guest code (unknown character, unknown item, wrong
// slot for the item, the character cannot use it, the player does not hold
// one) is still reported immediately, before anything is queued; use
// EternalSonataCanEquip to ask that question on its own.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_EQUIPMENT_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED "eternalsonata.equipment.equipped"
#define ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED "eternalsonata.equipment.unequipped"
#define ETERNALSONATA_EQUIPMENT_EVENT_MAGIC_SET "eternalsonata.equipment.magic_set"

// Characters are the same 1..10 the Party API uses.
#define ETERNALSONATA_EQUIPMENT_CHARACTER_MIN 1
#define ETERNALSONATA_EQUIPMENT_CHARACTER_MAX 10

// Item ids are the same 1..512 the Item API uses.
#define ETERNALSONATA_EQUIPMENT_ITEM_ID_MIN 1
#define ETERNALSONATA_EQUIPMENT_ITEM_ID_MAX 512

// Four slots per character, and the game offers no fifth. The stat struct has
// room for more halfwords after the four ids, but the recompute
// (sub_821E7898) and the commit (sub_821E8390) both only ever see these four.
// What follows them is the character's equipped magic, which is a separate id
// space with its own table and has its own four slots below.
#define ETERNALSONATA_EQUIPMENT_SLOT_COUNT 4

// Four magic slots per character, at +0x24 of the same struct.
#define ETERNALSONATA_MAGIC_SLOT_COUNT 4

// Magic ids are their own space, nothing to do with item ids. The live upper
// bound is whatever the loaded magic table holds; this is the hard cap every
// entry point range-checks against first.
#define ETERNALSONATA_MAGIC_ID_MIN 1
#define ETERNALSONATA_MAGIC_ID_MAX 512

// The slots, in the order the game's own equipment screen lists them. Which
// items fit which slot is decided by the item's category (the same
// ETERNALSONATA_ITEM_CATEGORY_* value the Item API reports), so the two
// accessory slots take exactly the same items as each other.
enum {
  ETERNALSONATA_EQUIPMENT_SLOT_WEAPON = 0,     // category 1
  ETERNALSONATA_EQUIPMENT_SLOT_ARMOR = 1,      // category 2
  ETERNALSONATA_EQUIPMENT_SLOT_ACCESSORY1 = 2, // category 3
  ETERNALSONATA_EQUIPMENT_SLOT_ACCESSORY2 = 3  // category 3
};

// The magic slots, in the order the game stores them. Each is bound to one
// kind: the even slots take light magic and the odd ones dark, and a slot will
// not accept the other kind. The first pair is cast by pressing the attack
// button and the second by holding it.
enum {
  ETERNALSONATA_MAGIC_SLOT_LIGHT_PRESS = 0,
  ETERNALSONATA_MAGIC_SLOT_DARK_PRESS = 1,
  ETERNALSONATA_MAGIC_SLOT_LIGHT_HOLD = 2,
  ETERNALSONATA_MAGIC_SLOT_DARK_HOLD = 3
};

// A magic record's kind. Kind 1 is a placeholder the table carries for entries
// that are not castable magic (Allegretto's first three records are the only
// ones observed), and it never appears in a slot.
enum {
  ETERNALSONATA_MAGIC_KIND_NONE = 1,
  ETERNALSONATA_MAGIC_KIND_LIGHT = 2,
  ETERNALSONATA_MAGIC_KIND_DARK = 3
};

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_EQUIPMENT_OK = 0,
  // The change was accepted and will be applied on the guest thread's next
  // frame. See the threading note at the top.
  ETERNALSONATA_EQUIPMENT_QUEUED = 1,

  // Nothing is loaded yet, or the tables are not mapped (e.g. at the title
  // screen before a save is loaded). Every mutation refuses in this state.
  ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE = -1,
  // The character number is outside 1..10.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER = -2,
  // The slot is outside 0..3.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT = -3,
  // The item id is outside 1..512.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM = -4,
  // The item is not equipment at all, or is equipment this character is not
  // allowed to wear.
  ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE = -5,
  // The item is equipment this character can wear, but not in this slot: its
  // category names a different one.
  ETERNALSONATA_EQUIPMENT_ERR_WRONG_SLOT = -6,
  // The player is not carrying one. Equipment is taken out of the inventory
  // while it is worn, so an item already on somebody cannot be equipped again
  // until it is removed.
  ETERNALSONATA_EQUIPMENT_ERR_NOT_OWNED = -7,
  // The slot is already empty (unequip only).
  ETERNALSONATA_EQUIPMENT_ERR_SLOT_EMPTY = -8,
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT = -10,

  // The magic id is outside 1..512, or past the end of the loaded magic table.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_MAGIC = -11,
  // The magic belongs to another character, or is a table entry that is not
  // castable magic at all.
  ETERNALSONATA_EQUIPMENT_ERR_MAGIC_NOT_OWNED = -12,
  // The character is not high enough level to have learned it yet.
  ETERNALSONATA_EQUIPMENT_ERR_MAGIC_NOT_LEARNED = -13,
  // The magic is this character's and learned, but its kind does not match the
  // slot: light magic only goes in the even slots and dark only in the odd.
  ETERNALSONATA_EQUIPMENT_ERR_WRONG_MAGIC_KIND = -14
};

// One equipment slot: what is in it, and what that item contributes.
typedef struct EternalSonataEquipment {
  // 1..10, the character this slot belongs to.
  int32_t character;
  // 0..3, one of the ETERNALSONATA_EQUIPMENT_SLOT_* values.
  int32_t slot;
  // The item in the slot, or 0 when the slot is empty. All the fields below
  // are 0 in that case.
  int32_t item_id;
  // Into the same BTX blocks the Item API reads, equal to item_id - 1. The
  // strings themselves are answered by EternalSonataGetEquipmentName and
  // EternalSonataGetEquipmentDescription.
  int32_t name_text_id;
  int32_t description_text_id;
  // The item's category, which is what decides the slot it fits: 1 weapon,
  // 2 armor, 3 accessory. Same values as ETERNALSONATA_ITEM_CATEGORY_*.
  int32_t category;

  // The flat bonus this item contributes, straight from its master record.
  // This is the additive half of the game's stat maths only: each item also
  // carries five multipliers that are pooled across everything the character
  // is wearing and applied to the totals afterwards, so these numbers do not
  // add up to the difference the status screen shows. For the number a screen
  // would actually draw, use EternalSonataPreviewEquip.
  int32_t hp, attack, magic, defense, speed;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataEquipment;

// The six stats the equipment screen compares, in the order the game's own
// preview hands them back.
typedef struct EternalSonataEquipStats {
  int32_t hp_max;
  int32_t attack;
  int32_t magic;
  int32_t defense;
  int32_t speed;
  // Current HP, rescaled by however much maximum HP moved. Never below 1.
  int32_t hp;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataEquipStats;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataEquipmentAbiVersionFn)(void);

// True once the stat arrays, the master table and the inventory are all
// readable and a save has been loaded. False at the title screen.
typedef int (*EternalSonataIsEquipmentSystemAvailableFn)(void);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Fills `out` with what `character` has in `slot`. An empty slot is not an
// error: it answers OK with item_id 0. Returns ETERNALSONATA_EQUIPMENT_OK or a
// negative error.
typedef int (*EternalSonataGetEquipmentFn)(int character, int slot,
                                           EternalSonataEquipment* out);

// Fills `out` with up to `max` of the character's slots, in slot order,
// empty ones included, and returns how many were written, or a negative error.
// Pass max = 0 to just count. There are always
// ETERNALSONATA_EQUIPMENT_SLOT_COUNT of them.
typedef int (*EternalSonataGetAllEquipmentFn)(int character, EternalSonataEquipment* out,
                                              int max);

// The item id in one slot, 0 when the slot is empty, or a negative error.
// Shorthand for reading EternalSonataEquipment::item_id.
typedef int (*EternalSonataGetEquippedItemFn)(int character, int slot);

// The equipment's name and description in the language the game is running
// in. Identical to the Item API's answers for the same id, and repeated here
// so a mod that only wants equipment does not have to resolve both APIs.
// Never null: an id the blocks have nothing for answers "". The returned
// pointer stays valid for the life of the process.
typedef const char* (*EternalSonataGetEquipmentNameFn)(int item_id);
typedef const char* (*EternalSonataGetEquipmentDescriptionFn)(int item_id);

// ---------------------------------------------------------------------------
// What fits where
// ---------------------------------------------------------------------------

// Whether `character` is allowed to wear `item_id` at all, ignoring slots and
// ignoring whether the player holds one: 1 yes, 0 no, or a negative error.
// This is the game's own eligibility test and it answers without running any
// guest code.
typedef int (*EternalSonataCanEquipFn)(int character, int item_id);

// Which slot `item_id` goes in, one of the ETERNALSONATA_EQUIPMENT_SLOT_*
// values, or a negative error. An accessory answers
// ETERNALSONATA_EQUIPMENT_SLOT_ACCESSORY1, since either accessory slot takes
// it. Anything that is not equipment answers
// ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE.
typedef int (*EternalSonataGetEquipmentSlotForItemFn)(int item_id);

// Whether EternalSonataEquip would be accepted right now, without changing
// anything: ETERNALSONATA_EQUIPMENT_OK, or the error it would fail with
// (NOT_EQUIPPABLE, WRONG_SLOT, NOT_OWNED, ...).
typedef int (*EternalSonataCanEquipInSlotFn)(int character, int slot, int item_id);

// Fills `ids_out` with up to `max` item ids the player is carrying that
// `character` could put in `slot`, and returns how many were written, or a
// negative error. Pass max = 0 to just count. What is already worn is not in
// the list: the game takes equipment out of the inventory while it is on
// somebody.
typedef int (*EternalSonataGetEquippableItemsFn)(int character, int slot, int* ids_out,
                                                 int max);

// ---------------------------------------------------------------------------
// Previewing
// ---------------------------------------------------------------------------

// Fills `out` with the stats `character` would have with `item_id` in `slot`,
// changing nothing. Pass item_id 0 to preview emptying the slot. If the
// character cannot wear the item, `out` is filled with its current stats and
// the call returns ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE, which is what
// the game's own comparison does. Returns ETERNALSONATA_EQUIPMENT_OK on a
// preview that would be allowed.
typedef int (*EternalSonataPreviewEquipFn)(int character, int slot, int item_id,
                                           EternalSonataEquipStats* out);

// The stats `character` has right now, the same six values a preview answers
// with. Returns ETERNALSONATA_EQUIPMENT_OK or a negative error.
typedef int (*EternalSonataGetEquipStatsFn)(int character, EternalSonataEquipStats* out);

// ---------------------------------------------------------------------------
// Equipping
// ---------------------------------------------------------------------------

// Puts `item_id` in `character`'s `slot` through the game's own commit, so the
// item leaves the inventory, whatever was in the slot goes back into it, and
// the character's stats are recomputed. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataEquipFn)(int character, int slot, int item_id);

// Empties `character`'s `slot`, putting what was in it back in the inventory
// and recomputing the character's stats. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataUnequipFn)(int character, int slot);

// Empties all four of `character`'s slots. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front. A slot
// that is already empty is skipped rather than being an error.
typedef int (*EternalSonataUnequipAllFn)(int character);

// ---------------------------------------------------------------------------
// Magic
// ---------------------------------------------------------------------------
//
// A character's four equipped magic ids sit at +0x24 of the same stat struct
// the equipment ids are at, and everything about them comes out of a table of
// its own: which character owns a magic, what kind it is, what level it is
// learned at and what it costs to cast. A character never "acquires" magic the
// way it acquires an item; it learns every entry the table gives it as it
// levels, so the list a mod can choose from is a filter over that table rather
// than a walk of the inventory, and nothing here touches the inventory or
// recomputes a stat.

// One magic, either in a slot or on its own. EternalSonataGetMagicInfo answers
// with slot -1, since a table entry does not belong to a slot.
typedef struct EternalSonataMagic {
  // 1..10, the character that owns this magic. For a read of an empty slot,
  // the character whose slot was read.
  int32_t character;
  // 0..3, one of the ETERNALSONATA_MAGIC_SLOT_* values, or -1 when this is a
  // table entry rather than a slot.
  int32_t slot;
  // The magic in the slot, or 0 when the slot is empty. All the fields below
  // are 0 in that case.
  int32_t magic_id;
  // Into the magic text blocks, equal to magic_id - 1. These are NOT the item
  // blocks and the ids do not line up with item ids; resolve them with
  // EternalSonataGetMagicName and EternalSonataGetMagicDescription.
  int32_t name_text_id;
  int32_t description_text_id;
  // One of the ETERNALSONATA_MAGIC_KIND_* values.
  int32_t kind;
  // The character level this magic is learned at.
  int32_t unlock_level;
  // What casting it costs in echoes. A castable magic always costs at least 1.
  int32_t echo_cost;
  // The character's own ordinal for this entry, i.e. its index among that
  // character's magic. Carried through because the game's screens key off it.
  int32_t ordinal;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataMagic;

// Fills `out` with what `character` has in magic `slot`. An empty slot is not
// an error: it answers OK with magic_id 0. Returns ETERNALSONATA_EQUIPMENT_OK
// or a negative error.
typedef int (*EternalSonataGetMagicFn)(int character, int slot, EternalSonataMagic* out);

// Fills `out` with up to `max` of the character's magic slots, in slot order,
// empty ones included, and returns how many were written, or a negative error.
// Pass max = 0 to just count. There are always
// ETERNALSONATA_MAGIC_SLOT_COUNT of them.
typedef int (*EternalSonataGetAllMagicFn)(int character, EternalSonataMagic* out, int max);

// The magic id in one slot, 0 when the slot is empty, or a negative error.
typedef int (*EternalSonataGetEquippedMagicFn)(int character, int slot);

// Fills `out` with a magic table entry, whoever it belongs to, with slot -1.
// Returns ETERNALSONATA_EQUIPMENT_OK or a negative error.
typedef int (*EternalSonataGetMagicInfoFn)(int magic_id, EternalSonataMagic* out);

// The magic's name and description in the language the game is running in.
// Never null: an id the blocks have nothing for answers "". The returned
// pointer stays valid for the life of the process.
typedef const char* (*EternalSonataGetMagicNameFn)(int magic_id);
typedef const char* (*EternalSonataGetMagicDescriptionFn)(int magic_id);

// Whether `character` has learned `magic_id`: 1 yes, 0 no, or a negative
// error. Yes means the magic is this character's, is castable, and the
// character's level is at or above its unlock level, which is exactly the test
// the game's own list makes.
typedef int (*EternalSonataHasLearnedMagicFn)(int character, int magic_id);

// Whether EternalSonataSetMagic would be accepted right now, without changing
// anything: ETERNALSONATA_EQUIPMENT_OK, or the error it would fail with
// (MAGIC_NOT_OWNED, MAGIC_NOT_LEARNED, WRONG_MAGIC_KIND, ...).
typedef int (*EternalSonataCanSetMagicFn)(int character, int slot, int magic_id);

// Fills `ids_out` with up to `max` magic ids `character` could put in `slot`,
// in the order the game's own list draws them, and returns how many were
// written, or a negative error. Pass max = 0 to just count. Magic already in
// another slot is included: unlike equipment, the same magic may sit in more
// than one slot at once.
typedef int (*EternalSonataGetAvailableMagicFn)(int character, int slot, int* ids_out, int max);

// Whether the second pair of slots (LIGHT_HOLD and DARK_HOLD) can hold
// something different from the first: 1 yes, 0 no, or a negative error. Early
// in the game the game mirrors each press slot into the matching hold slot, so
// a write to slot 0 or 1 also lands in slot 2 or 3 and a write to slot 2 or 3
// is pointless. EternalSonataSetMagic reproduces that mirror rather than
// fighting it, so a mod that wants the four independent should check this
// first.
typedef int (*EternalSonataAreMagicSlotsIndependentFn)(void);

// Puts `magic_id` in `character`'s magic `slot`. Pass magic_id 0 to empty the
// slot. While the game is still mirroring the pairs (see
// EternalSonataAreMagicSlotsIndependent), writing slot 0 or 1 writes the
// matching hold slot too. Returns ETERNALSONATA_EQUIPMENT_QUEUED, or a
// negative error decided up front.
typedef int (*EternalSonataSetMagicFn)(int character, int slot, int magic_id);

// Empties all four of `character`'s magic slots. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front. A slot
// that is already empty is skipped rather than being an error.
typedef int (*EternalSonataClearMagicFn)(int character);

#ifdef __cplusplus
}  // extern "C"
#endif
