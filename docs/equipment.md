# Equipment

How Eternal Sonata stores what each character is wearing, and what this project
does with that.

Equipment is the meeting point of two systems that already have their own
write-ups: an item ([items.md](items.md)) worn by a character
([party-system.md](party-system.md)). The exe reads and writes it itself and
exposes it to mods through
[`src/eternalsonata_equipment_api.h`](../src/eternalsonata_equipment_api.h),
implemented in `src/equipment_system.cpp`. A mod should never need an address
from this page; it is here so the next person can check the implementation
against the binary.

## Four slots

A character's equipment ids live inside its 48-byte stat struct, at `+0x1C`,
four `u16`. Both of the parallel stat arrays carry them:

| Address | Array |
| --- | --- |
| `0x8243FEE8` | the character's own stats, what a save holds |
| `0x8243FD08` | the equipment-adjusted copy the screens draw |

So slot *s* of character *c* is at `0x8243FEE8 + 48 * (c - 1) + 0x1C + 2 * s`.
Only the first array is authoritative: `sub_821E7898` copies `+0x1C`..`+0x22`
across to the live struct on every recompute, so writing the base one and
recomputing is enough. The API writes both anyway on unequip, so a reader that
looks between the two never sees a mismatched pair.

Four, not five. `sub_821E7898` does run its equipment loop five times, but the
five-entry array it walks is filled `{[0] = +0x1C, [1] = +0x1E, [2] = 0,
[3] = +0x20, [4] = +0x22}`. The third entry is hardwired to zero and the loop
skips zeros, so the fifth iteration is always a no-op. `sub_821E8390` (the
commit) and `sub_821E8458` (the preview) both handle exactly slots 0..3 and
fall through to nothing for anything higher.

The halfwords after the four ids are a different system: they are the
character's **equipped magic**, four `u16` at `+0x24`, two of which are the
alternates cast by holding the button rather than pressing it.
`sub_82231F30` is the equipment screen's "put this in
that slot" entry point and it splits on its slot argument: 0..3 go to
`sub_821E8390`, while 4..7 write `+0x24`, `+0x26`, `+0x28` and `+0x2A`
directly and then recompute. Magic is a separate id space with its own table,
it never moves anything through the inventory, and `sub_821E7898` neither reads
it for stats nor even copies it to the live struct. It is covered by the same
API, on calls of its own; see [Magic](#magic) below.

The four slots, and which items go in them:

| Slot | What | Master category (`+0x03`) |
| --- | --- | --- |
| 0 | weapon | 1 |
| 1 | armor | 2 |
| 2 | accessory | 3 |
| 3 | accessory | 3 |

That mapping is `sub_821FC9A0`, which builds the screen's candidate list: for
slot 0 it asks `sub_821FCAD8` for sort mode 3, for slot 1 mode 4, and for slots
2 and 3 mode 5. Modes 3, 4 and 5 read the per-category caches
`sub_821FC5C8` fills from the inventory, which bucket by the master record's
`+0x03` into categories 1, 2 and 3. Both accessory slots therefore take exactly
the same items as each other.

## Who may wear what

The master entity table record for id *n* is at `0x82017630 + 100 * (n - 1)`.
`+0x04` is a `u32` flag word and it is the whole eligibility test.
`sub_821E8458` does:

```c
flags = master[id].u32_at_0x04;
equippable = (flags & 4) != 0 && (flags & (4 << character_number)) != 0;
```

Bit 2 means "this is equipment at all", and bit `2 + c` means "character *c*
can equip it". `sub_821FAF08` writes the same test as `(8 << (c - 1)) & flags`,
and `sub_821FC9A0` is handed `4 << c` by its caller and ANDs it straight in.

Spot checks: id 1 Hunting Knife has `0x0C` (equippable, Allegretto only, and it
is indeed his starting weapon); id 77 has `0x84` (equippable, character 5);
id 209 Floral Powder has `0x02`, no bit 2, so not equipment. Consumables carry
bit 1 instead, which `sub_821FCAD8`'s consumable path tests as "usable".

`EternalSonataCanEquip` is this test and nothing else, so it answers without
running any guest code.

## The stat maths

`sub_821E7898(character, live_struct)` rebuilds the live struct from the base
struct plus equipment. Per equipped item it adds, from the master record:

| Master offset | Type | Added to live struct offset | Meaning |
| --- | --- | --- | --- |
| `+0x10` | u32 | `+0x10` | max HP |
| `+0x14` | u16 | `+0x14` | attack |
| `+0x16` | u16 | `+0x2C` | a capped stat the screens do not draw |
| `+0x18` | u16 | `+0x2E` | the other one |
| `+0x1A` | u16 | `+0x16` | magic |
| `+0x1C` | u16 | `+0x18` | defense |
| `+0x1E` | u16 | `+0x1A` | speed |

Then five `float` multipliers, each contributing `value - 1.0` to a running
total that starts at 1.0 and is applied to the summed stat afterwards:

| Master offset | Scales |
| --- | --- |
| `+0x44` | max HP |
| `+0x48` | attack, and both hidden stats |
| `+0x4C` | magic |
| `+0x50` | defense |
| `+0x54` | speed |

Every master record that is not equipment holds 1.0 in all five, which is what
makes the neutral case a no-op. Hex-Rays mangles `sub_821E7898` badly (its
output opens with "local variable allocation has failed"); the disassembly at
`0x821E7A9C` and `0x821E7B78`, where the five accumulators are loaded and then
multiplied into the five sums in turn, is the authority for this table.

Each product is scaled by 10, truncated to an integer and divided by 10 again.
The four capped stats then clamp to 0..999 and the two HP figures to 1..999999.
Finally current HP is rescaled by however much maximum HP moved:
`new_hp = base_hp / live_max_hp * new_max_hp`, rounded half up, never below 1.
That rescale is the trap `party-system.md` documents: write both structs before
asking for a recompute, or the ratio moves current HP around.

`sub_821E8458(c, slot, id, out[6])` is a dry run of all of the above: it
returns 0 with the character's current stats if the character cannot wear the
item, or 1 with the previewed stats if it can, and changes nothing. The six
values are, in order, max HP, attack, magic, defense, speed and current HP.

`EternalSonataPreviewEquip` reimplements that routine in host code rather than
calling it. Calling it would mean queueing work onto the guest thread and
answering a mod's UI a frame late, when the whole point of a preview is to draw
comparison arrows now; and the routine is pure arithmetic over tables the
system is already reading. The reimplementation mirrors the disassembly step
for step, rounding included.

## Equipping and unequipping

**Equipping is `sub_821E8390(character, slot, item_id)`**. It computes its
address as `base + 0x1C + 2 * slot` rather than naming `unk_8243FF04` and
friends, so IDA produces no data xref to the named globals and it cannot be
found by cross-referencing them. It:

1. writes the id into the character's own stat struct,
2. calls `sub_821E7898` to recompute,
3. hands the old occupant back to the inventory with
   `sub_821FBFC0(&dword_8255EED8, old, 1, 1)` if there was one,
4. takes the new one out of the inventory with
   `sub_821FC330(&dword_8255EED8, id, 1)`.

Step 4 is the single most important difference between this and the Item Set:
**worn equipment is not in the inventory**. The Item Set leaves the item in
place and only bumps its `reserved` byte, so `EternalSonataGetOwnedItems` still
reports it; a worn weapon has no inventory record at all and will not appear
there. It also makes "the player holds one" and "it is not already on somebody"
the same question, which is what `EternalSonataEquip` checks up front.

**Unequipping** is `sub_822352F8(slot)` in the game, but the API does not call
it. That routine acts on whichever character `dword_8243F360` names, the menu's
shared "current character" scratch that several unrelated screens use including
the Piano Music screen ([piano-music.md](piano-music.md)), and it repaints the
equipment screen as its last step. The three steps that matter are reproduced
in `equipment_system.cpp` in the same order: give the item back with
`sub_821FBFC0`, zero the id in both structs, recompute with `sub_821E7898`.

The save covers all of this for free: `sub_82241190` writes 480 bytes from
`0x8243FEE8` and 480 from `0x8243FD08`, so equipment persists with no extra
work. The "adopt, do not announce" reset hangs off the existing `sub_82240AF8`
hook in `photo_system.cpp`, next to `NotifyItemSaveLoaded`.

## Magic

Four more `u16`, immediately after the four equipment ids:

```
0x8243FEE8 + 48 * (c - 1) + 0x24 + 2 * s
```

Unlike equipment this lives in the **base array only**. `sub_821E7898` copies
`+0x1C`..`+0x22` across to the live struct at `0x8243FD08` and stops there, so
the live copy's `+0x24`..`+0x2A` are never maintained and must not be read.
`sub_82231F30` writes only the base array too. The save covers both arrays
wholesale, so equipped magic persists like everything else.

The four slots are two pairs:

| Slot | Address | Kind | Cast by |
| --- | --- | --- | --- |
| 0 | `+0x24` | light | pressing the button |
| 1 | `+0x26` | dark | pressing the button |
| 2 | `+0x28` | light | holding the button |
| 3 | `+0x2A` | dark | holding the button |

The light/dark split is `sub_82230ED0`, which populates the screen for slot
`a1` and asks for kind `a1 % 2 ? 3 : 2`, so the even slots take kind 2 and the
odd ones kind 3. Which kind is light and which is dark comes from the data
rather than from any routine: Polka's kind-2 magic outnumbers her kind-3 seven
to four, and a save holding "Orange Glow", "Nether Wave", "Earth Heal" and
"Shade Comet" in slots 0..3 puts them in light, dark, light, dark order. Both
readings agree on kind 2 = light, kind 3 = dark.

**The second pair is not independently assignable until late.**
`sub_82231F30` opens by reading the `u32` at `0x8243F3E8` (party base +0, the
field immediately before the party level) and comparing it against 3. Below 3,
after writing slot 4 it writes slot 4 + 2 with the same id, and after slot 5 it
writes slot 7, so setting a primary magic mirrors it into the matching
alternate and the hold-to-cast slot cannot differ from the press-to-cast one.
At 3 and above the mirror stops and the four become independent. What that
`u32` counts is unidentified; it is not the party level, which is the next
word, at `0x8243F3EC`. `EternalSonataSetMagic` reproduces the mirror rather
than writing past it, and `EternalSonataAreMagicSlotsIndependent` is that
comparison, so a mod can tell why its write to slot 2 did not stick.

### The magic table

Magic has its own id space, nothing to do with the item/character master table.
The records are at `0x82015380`, 12 bytes each, and the id of a record is its
index plus one. The live record count is `*(u16*)(dword_824400E0 + 16)`.

**That count is not always readable.** The object `dword_824400E0` points at is
allocated when the menu that needs it opens and freed when the menu comes down,
so anything gated on it answers "unavailable" for most of the game. The record
table itself and the display order are static image data and stay readable
throughout, and the equipped ids live in the stat struct, so the count is the
only part that goes away. The API therefore latches the last live count it saw
and, before it has seen one, falls back to the highest id named by
`word_8202C8A8`: every castable magic has a row there, since that table is what
the game's own list is built from, so an equipped id is always within it. An id
past the end is still rejected rather than read; the bound just survives the
menu closing.

| Offset | Type | What |
| --- | --- | --- |
| `+0x00` | u16 | character number, 1..10 |
| `+0x02` | u16 | the character's own ordinal for this entry |
| `+0x04` | u16 | kind: 1 not magic, 2 light, 3 dark |
| `+0x06` | u16 | the character level it unlocks at |
| `+0x08` | u16 | echo cost; 0 means "not a castable magic" |
| `+0x0A` | u16 | unidentified, and shared across characters, so not a pair key |

The first three records are Allegretto's and carry kind 1 with a zero cost,
which is why his magic ids start at 4.

Polka's eleven records, ids 15..25, as `[kind, level, echoes]`:

| Id | Kind | Level | Echoes |
| --- | --- | --- | --- |
| 15 | light | 1 | 5 |
| 16 | light | 16 | 4 |
| 17 | light | 48 | 18 |
| 18 | light | 32 | 6 |
| 19 | dark | 48 | 6 |
| 20 | light | 20 | 7 |
| 21 | dark | 1 | 7 |
| 22 | light | 36 | 2 |
| 23 | dark | 8 | 2 |
| 24 | light | 28 | 8 |
| 25 | dark | 40 | 8 |

Id 15 (light, level 1) and id 21 (dark, level 1) are the two she starts with,
which is what a fresh save shows in slots 0 and 1.

`word_8202C8A8` is the display order: `u16[11]` per character, at
`11 * (c - 1)`, holding magic ids in the order the screen lists them, zero
padded. Polka's row is 15, 16, 20, 24, 18, 22, 17, 21, 23, 25, 19: every light
entry, then every dark one. `EternalSonataGetAvailableMagic` walks this row so
a mod's list comes out in the same order the game's own does.

### Learning and listing

`sub_821E93B0(ctx, c, out, kind)` is the candidate list, and its filter is the
whole of what "this character has learned this magic" means: every record whose
character is `c`, whose kind is `kind`, whose unlock level is at or below the
character's current level (read from the **live** struct `+0x00`) and whose
echo cost is nonzero, written out in `word_8202C8A8` order. Nothing is
acquired, dropped or spent: a character simply has every entry the table gives
it once it is high enough level, which is why the magic half of the API never
touches the inventory. `EternalSonataHasLearnedMagic` is that same test for one
id.

Two BTX blocks carry magic text, `0x8230F640` and `0x822FF598`, both keyed by
`id - 1` and both read through the item system's shared reader
(`LookupBtxString` in `item_system.h`) rather than through the guest's own
`sub_8223B780`. `sub_82234408` uses the first for its list rows, so that one is
the name block; `sub_82230ED0` resolves the second for the detail panel, so it
is presumably the description. That has not been confirmed.

### The routines

| Routine | What it is |
| --- | --- |
| `sub_821E93B0(ctx, c, out, kind)` | the candidate list described above. Returns how many |
| `sub_82234408(c, kind)` | draws that list, resolving each name with `sub_8223B780(0x8230F640, id - 1)` and paging at seven rows |
| `sub_82230ED0(slot)` | populates the magic page for screen slot 4..7, and paints the echo cost from the record's `+0x08` as a two-digit number |
| `sub_82231F30(slot, id)` | the commit: a direct halfword write plus `sub_821E7898`, with the mirror described above |

The API calls none of them. Reading a slot is a halfword load, the candidate
list is a filter over a 12-byte-record table that host code walks directly, and
the commit is one halfword write plus the mirror, so `sub_82231F30`'s screen
repaint is avoided the same way `sub_822352F8`'s is.

## The rest of the equipment screen

Not used by the API:

| Routine | What it is |
| --- | --- |
| `sub_8222FFE8(c, row)` | opens the equipment screen for a character |
| `sub_82230ED0(slot)` | populates the screen for a slot; calls `sub_821FC9A0` for slots 0..3 and `sub_821E93B0` for the magic pages 4..7 |
| `sub_82231F30(slot, id)` | commit: `sub_821E8390` for slots 0..3, a direct write for the magic slots 4..7 |
| `sub_82230658` | the screen's input handler; reaches `sub_822352F8` on the unequip button |
| `sub_821FC5C8(db, mode)` | recompacts the inventory and rebuilds the four per-category caches |
| `sub_821FCAD8(mode, out, arg)` | sorts one of those caches into a flat id list |
