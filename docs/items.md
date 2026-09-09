# Items and the Item Set

Reverse-engineering notes for the player's inventory and for the **Item Set**,
the short list of items that can actually be used in battle. The mod-facing
surface built on top of this is `src/eternalsonata_item_api.h`, implemented in
`src/item_system.cpp`.

Addresses are guest addresses in the retail `default.xex`.

## The id space

Everything the player can own is an *entity*, numbered 1..512, and the static
data for all of them lives in one master table.

| Address | Type | Meaning |
| --- | --- | --- |
| `0x82017630` | 100-byte records, indexed by id - 1 | The master entity table. Ends at `0x82023E30`. |

Fields inside a record, each confirmed against a routine that reads it:

| Offset | Type | Field | Read by |
| --- | --- | --- | --- |
| `+0x00` | u16 | the id itself (record *k* holds id *k+1*, so the table is dense and sorted) | everything |
| `+0x02` | u8 | icon, an index into `word_8202C9C8` | `sub_8220EEE0` |
| `+0x03` | u8 | category: 0 consumable, 1 weapon, 2 armor, 3 accessory | `sub_821FC5C8` |
| `+0x08` | u32 | buy price | `sub_8222E9C0` |
| `+0x0C` | u32 | sell price | `sub_8222E9C0` |
| `+0x36` | u16 | Item Set cost in party-level points | `sub_821E6740` |

### Validity and real items

A record is **real** (not a character or unused slot) if:
1. The id is not in the character range (1..10)
2. The id is in the base game range 11..402, OR
3. The id has been registered as a custom item by a mod via `EternalSonataRegisterCustomItem`

### Custom items (mod registration)

Records **403..510** ship blank: each carries its own id at `+0x00` and zero
everywhere else, and no shipped text block has an entry for it. Records 511 and
512 run into the float table that follows, so they are left alone.

`EternalSonataRegisterCustomItem` takes one of those slots and fills it in, so
the item is real to the game rather than to the API only. Two things make that
work:

* The master record is written in place. The xex image is mapped read-only, so
  `item_system.cpp` reprotects the record's pages first. Everything the game
  reads about an item then answers from the table it already reads: the icon
  (`+0x02`, clamped to the 68 entries `word_8202C9C8` has), the category
  (`+0x03`) that buckets the item screen's tabs, the prices and the Item Set
  cost.
* The name and description come from the `sub_8223B780` hook in
  `eternalsonata_options.cpp`, which answers ids 402..509 on the two item text
  blocks from guest string buffers instead of letting the stock lookup return
  null. Returning null is what used to crash the item screen: `sub_8220EEE0`
  hands the result straight to the string painter.

`EternalSonataUnregisterCustomItem` puts the record back to blank and frees the
id for the next registration.

Ids are allocated at runtime and the save file stores them like any other, so a
save made while a mod's items were in the inventory needs that mod present, and
registering in a stable order, to read back as the same items.

The category byte is what buckets the item screen's four tabs: `sub_821FC5C8`
walks the inventory and copies each entry into one of four parallel lists,
`0x8256362C` (cat 0), `0x82563E2C` (1), `0x8256462C` (2), `0x82564E2C` (3),
with the counts at `0x8256361C`, `0x82563620`, `0x82563624`, `0x82563628`.

Worked example, id 209 "Floral Powder": icon 16, category 0, buy 100, sell 25,
cost 2. Id 1 is "Hunting Knife" (category 1, cost 0), id 141 "Handmade
Clothes" (2), id 281 "Brisingamen" (3).

Note that `docs/party-system.md` has the party's join path calling
`sub_821E6740` with a character number 1..10, i.e. into the same id space,
where this table holds weapons. The two readings have not been reconciled; the
item API deliberately does not depend on the answer.

## Names and descriptions

Both are packed BTX text blocks inside the xex image, keyed by **id - 1**:

| Address | Contents |
| --- | --- |
| `0x82376400` | item names, 402 entries per language |
| `0x8233A3A8` | item descriptions |

Format is the one `scripts/btx.py` decodes: a `BTX ` header (offset to the
first language block at `+0x04`, block count at `+0x0C`), language blocks
chained through their own `+0x08`, each with its entry table at `+0x04` and
entry count at `+0x10`, and entries of `{u32 text id, u32 offset from the
block's base}`. The seven languages are JPN USA GBR FRA ITA DEU ESP and
`dword_8243D370` selects between them. Entries are stored in id order with no
gaps, so entry *n* is text id *n*.

`sub_8220EEE0` is the item row painter and resolves the name as
`sub_8223B780(0x82376400, id - 1)`. `src/item_system.cpp` walks the blocks in
host code instead, so a name lookup does not have to queue onto the guest
thread.

## The inventory

| Address | Type | Meaning |
| --- | --- | --- |
| `0x8255EED8` | object | what every inventory routine takes as its first argument |
| `0x8255EF08` | 512 records of `{u16 id, u8 count, u8 reserved}` | the inventory itself, ending at `0x8255F708` |
| `0x8255FF08` | u16 | how many records are in use |

`reserved` (`+0x03`) is how many of that stack are spoken for by entries in the
Item Set. `sub_821FBF20` is the "can I still register one of these" gate and
returns `count - reserved`; `sub_821FBE88` returns the raw `count`.

The table is kept **compacted**: releasing a slot closes the gap rather than
leaving a hole, so a slot index is not a stable name for an item.
`sub_821FC5C8` is the compaction plus the category-list rebuild plus the
screens' sort, and `sub_821FC478` is the same with a "drop anything with a
count of zero" pass in front.

| Routine | What |
| --- | --- |
| `sub_821FBFC0(db, id, count, refresh)` | give: bump an existing stack or take a free slot, clamping at 99 |
| `sub_821FC1E8(db, id, count)` | the game's own acquire: `sub_821FBFC0` with `refresh` set, then push the id to the front of the recently-acquired list |
| `sub_821FC330(db, id, count)` | take: returns what is left, releases the slot at zero, returns -1 if the player held none |

Ids **350..381** are score pieces and are not in this table at all; see the
section below.

`word_825600D4` is a separate 32-entry u16 list, the recently-acquired items,
maintained by `sub_821FC1E8` and pruned by `sub_821FC330`. It is *not* the Item
Set.

## Score pieces

The 32 score pieces are items with real master records (id 350..381, icon 21,
category 0, no prices and no Item Set cost) that never enter the inventory.
`sub_821FBFC0` (give) and `sub_821FBE88` / `sub_821FBF20` (how many held / how
many free) all branch that id range onto a collection of their own:

| Address | Type | Meaning |
| --- | --- | --- |
| `0x8243FCD0` | u32 | Bit *number - 1* per piece, set when the piece is found. Written by the give path and saved, but nothing ever reads it back. |
| `0x8243FCD4` | u8[32] | The piece numbers 1..32 in the order they were found, 0 for a free slot. This is the real ownership record and the order the menu lists them in. |
| `0x8243FCF4` | u8 | The menu's Mark: a piece number, or 0 for nothing marked. |

Piece **number** is 1..32 and item id is number + 349. `sub_821FBFC0` sets the
bit with `1 << (id - 94)`; PowerPC `slw` takes the shift modulo 64, so that is
bit `id - 350` and not an out-of-range shift.

All three fields sit inside the 2324 bytes `sub_82241190` writes from
`0x8243F3E8`, so the collection and the Mark both survive a save.

Two asymmetries worth knowing:

* **A score piece can never be taken away by the game.** `sub_821FC330` has no
  branch for the range at all, so it just fails to find one and returns -1.
  `EternalSonataTakeItem` removes it from the list in host code instead, closing
  the gap so the give path's "first zero is the end" scan stays right.
* **A score piece must not go in the Item Set.** `sub_821E6740` would accept
  one, because `sub_821FBF20` reports a collected piece as owned, but there is
  no inventory record behind it to reserve. `EternalSonataCanAddItemToSet`
  refuses the range up front.

### The Score Pieces menu

`sub_8220A588` builds it. It copies `byte_8243FCF4` into its own state at
`+0x184`, then fills a row list at `+0x190` from `byte_8243FCD4` (storing
`number - 1` per row) with the count at `+0x210`. A row is highlighted when
`state[0x184] - 1` equals it. With no pieces collected the screen draws text id
168 instead of a list.

Rows are sheet music images out of the resource table at `dword_824409DC+376`,
not text, which is why the pieces look nameless in game; the shipped item name
for id 350 is only the placeholder "Score Piece 01".

`sub_8220E550(screen, index)` is Mark: it sets `state[0x184]` to `index + 1`, or
back to 0 when the same row was already marked, which is the press-again-to-
unmark behaviour. The state is only written back to `byte_8243FCF4` when the
screen closes (`sub_8220DEC8`), so a mod that moves the Mark while the menu is
open has its change overwritten on exit.

## The Item Set

| Address | Type | Meaning |
| --- | --- | --- |
| `0x8243FC3E` | u16[32] | the registered item ids, compacted, last slot at `0x8243FC7C` |
| `0x8243FC7E` | u16[33] | scratch: the compacted copy `sub_821E6AF8` builds for its callers |
| `0x8243FCC0` | u32 | entry count, as the screens read it |
| `0x8243F3EC` | u32 | party level, 1..6 |
| `0x8202CA70` | u16[6] | budget cap per level: 10, 10, 20, 30, 40, 50 |
| `0x8243FCC4` | u8 | budget points left |
| `0x8243FCC5` | u8 | budget points spent |

The budget is the "P" the Item Set screen shows: at party level 3 the cap is 20
and the screen reads "20P". Each registered item spends its master-table
`+0x36` cost.

The four routines that maintain it (they sit next to each other in `.pdata` at
`0x820B1490`, which is an unwind table rather than a dispatch table, so do not
read it as one):

| Routine | What |
| --- | --- |
| `sub_821E6740(id)` | register: 0 ok, 1 set full, 2 budget too small, 3 nothing free to reserve. Charges the cost, reserves one held copy, appends at the first empty slot, then recomputes spent from the whole array |
| `sub_821E6958(slot)` | remove by slot: closes the gap, refunds the cost, releases the reserved copy |
| `sub_821E6BA8(id)` | remove by id **and consume one**, i.e. what using the item in battle does. Reached from the battle dispatcher `sub_821C9FE0` |
| `sub_821E6D68(id)` | remove by id without consuming |

This is what the battle item menu is built from: `sub_82189BA0` asks
`sub_821E6AF8` for the compacted copy, zero-terminates it and hands it to the
menu constructor. An item that is not in the set cannot be used in a fight.

Two things to know before writing to any of this.

* **`dword_8243FCC0` is not maintained by the register/remove routines.** The
  Item Set screen recounts the array itself after every change
  (`sub_82225FE0`), so anything that changes the set has to recount it too.
* **`sub_821FC330` leaves the set alone.** Selling the last copy of a
  registered item would otherwise leave an entry with nothing behind it; the
  shop's own sell path (`sub_8222E9C0`) calls `sub_821E6D68` once per copy that
  went missing. `EternalSonataTakeItem` does the same.

### Screen and script entry points

| Routine | What |
| --- | --- |
| `sub_82225FE0(id)` | the Item Set screen's register action: `sub_821E6740`, then recount, then rescroll |
| `sub_82225C28(delta)` | reorder: swaps two entries of `word_8243FC3E` |
| `sub_82224490` | the screen itself; `sub_82225578` repaints its rows |
| `sub_821E5D68` | new game: clears everything, sets the budget to the level cap, then gives and registers the starting set from `word_8202CA7C` (four copies of id 209, Floral Powder, costing 8 of the 10 points party level 1 allows) |
| `sub_8222C190(shop, ...)` | opens a shop: fills `word_82560114` (32 records of `{u16 id, u16}`) from the 68-byte-per-shop stock table at `0x82015CE4` |

## The item catalog

Mods can enumerate all real items in the game without a host change:
`EternalSonataGetItemCatalog` fills an array with real items in ascending id
order and optionally filters by category. `EternalSonataGetItemCatalogCount`
returns the total count. `EternalSonataIsRealItem` checks a single id.

All three are read-only and answer from the master table immediately (guarded by
`ItemsReadable()`, which checks that the tables are mapped). None of them queue
work onto the guest thread.

## Gold

The party purse is the `u32` at `0x8243F3F0`. The shop routine
`sub_8222E9C0` reads and writes it for purchases and sales. Battle results add
enemy gold to the same field and saturate it at 99999999. It lies eight bytes
into the saved party block.

## Saving

`sub_82241190` writes and `sub_82240AF8` reads back:

* 2324 bytes from `0x8243F3E8`, which covers the party level, the Item Set and
  the budget;
* 2048 bytes from `0x8255EF08`, the whole inventory table.

So both halves survive a save and reload, and `src/item_system.cpp` hangs its
"adopt what the save restored, do not announce it" reset off the same
`sub_82240AF8` hook the photo album and piano music use.
