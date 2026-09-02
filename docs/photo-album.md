# The photo album

Reverse-engineering notes for the in-game camera's album, the Photos tab of the
status menu. The mod-facing surface built on top of this is
`src/eternalsonata_photo_api.h`, implemented in `src/photo_system.cpp`.

Addresses are guest addresses in the retail `default.xex`.

## Storage

The album is a fixed twelve records and cannot grow. Everything about it is
sized twelve: the record array, the display-order table, the per-slot texture
handles, and the album screen's own widget arrays. `sub_821E6FD8`, the add,
returns 0 outright when the count is already 12.

| Address      | Type        | Meaning |
| ------------ | ----------- | ------- |
| `0x8255E500` | object      | Owner of the album. Vtable at +0; constructed by `sub_822E5310`, which runs `sub_821F9CD8` over exactly twelve records. |
| `0x8255E508` | record[12]  | The photographs, stride 184 bytes. |
| `0x8255EDA8` | u8[12]      | Record-slot occupancy; `sub_821E6FD8` scans it for the first free slot. |
| `0x8255EDB4` | u8          | Photograph count, 0..12. |
| `0x8255EDB5` | i8[12]      | Display order: record slot per display position, -1 past the end. |
| `0x8255EDC8` | u8          | Detail/aspect flag, consulted by the sale-price computation. |
| `0x8255EDD0` | u8[12]      | Per-slot "needs redraw" flags, set wholesale on album open. |
| `0x8255EDDC` | u8[12]      | Per-slot "thumbnail surface allocated". |
| `0x8255EDE8` | u32[12]     | Thumbnail texture handles, -1 for none. |
| `0x8255EE18` | u8          | Index of the blank plate used for empty slots. |
| `0x8255EE1C` | float[12]   | Cached development per record slot. Saved and restored. |
| `0x8255EE4C` | u8[12]      | "Marked for removal", consumed by `sub_821FA300`'s compaction. |
| `0x82565780` | u32         | The game clock a photograph's capture timestamp is taken from. |

New photographs are unshifted onto the front of the display order, so the
newest is always display index 0. Selling or trashing sets the removal flag and
`sub_821FA300` compacts the order table and refills the tail with -1.

Because the display order moves, the record slot (0..11) is the only stable
identity for a photograph. That is what the API's `record` field and both
events carry.

## Record layout (184 bytes)

`sub_821C4248` is the reset, and `sub_821FA570` / `sub_821FA6C0` are the save
and load of exactly the persisted fields.

| Offset | Type      | Meaning |
| ------ | --------- | ------- |
| +12    | u32       | Handle of the 416x256 R5G6B5 surface holding the picture. Locked through `sub_8225DE58`, so only guest code can reach the pixels. |
| +36    | entry[3]  | Subjects, 20 bytes each. |
| +96    | entry[3]  | Three more subjects, same shape. |
| +156   | u32       | Flags. Bit 1 means the record holds no picture; the album draws those as a black plate with a caption (BTX id 207). |
| +160   | u8        | Runtime only, not saved. |
| +164   | float     | Quality score, 0..1. The album prints `score * 100` and picks its icon tier at 0.5 and 0.2. |
| +168   | float     | Sale price before development scaling. |
| +172   | u32       | The clock value when the photograph was taken. |
| +176   | i32       | The party-progress counter at the same moment; -1 on a record that was never filled. |

A subject entry is `u16 id` (-1 when empty), `u16 attribute`, then four u32 of
framing data whose individual meanings are not decoded. The API passes those
four through raw rather than naming them wrongly.

Not saved: +0..+35 and +160. Everything else round-trips, with the picture
itself appended as 212992 bytes (416 x 256 x 2) at +148 of the save record.

## Development

Development is not a countdown. `sub_82209478` evaluates it whenever the album
binds a slot, and caches the result in `flt_8255EE1C[record]`. The value is the
amount of haze left over the picture: 1 freshly taken, 0 fully developed. The
album draws the picture at alpha `1 - haze`.

With `now` = `0x82565780 / 300`, `taken` = `record[+172] / 300`, `progress` =
`record[+176]`, and `party` = the highest character level across the ten
48-byte stat blocks at `0x8243FEE8`:

```
time_term     = 1 - 1 / (80  * exp(-0.03 * (now - taken))       + 1)
progress_term = 1 - 0.7 / (100 * exp( 0.4  * (party - progress)) + 1)
haze          = progress_term * time_term * 0.9
if (haze < 0.05) haze = 0
```

Both `exp` calls go through `sub_822CDE38`, which clamps its argument to
[-708.396418532265, 709.782712893385] first; `Development()` in
`src/photo_system.cpp` clamps the same way.

Two consequences worth knowing:

* `progress_term` is bounded below by 0.3, so the elapsed-time term is the only
  one that can drive the result to zero. A photograph on a save whose clock has
  barely run cannot be made fully developed by rewriting the record's snapshots
  alone. This is why `EternalSonataDevelopPhoto` writes the fully-aged
  snapshots *and* pins the record for the session, with a hook on
  `sub_82209478` writing the pinned zero through.
* Because the game only evaluates the formula when the album screen is open,
  polling its cached copy would report a photograph as finished whenever the
  player next happened to look at it. `src/photo_system.cpp` therefore reruns
  the formula itself once per frame, which is what lets
  `eternalsonata.photo.developed` land when development actually completes.

## The album screen

The Photos tab lives in the status-menu screen module at `0x82203540` ..
`0x82207840`. `sub_822037B8` is the screen entry, which runs `sub_82205200`
(frame, page counter, per-slot labels), `sub_822057D8` (the twelve slot objects
and the "PhotoHeap" allocation they come out of), and `sub_82207A88` (bind one
record to one slot: corners, caption, icon tier, and the haze alpha). The
per-slot objects are 176 bytes with a vtable at `off_82087C94`; their +168 is
the haze `sub_82209478` writes.

`sub_822076A8` is the tab's state machine, an eleven-arm jump table on
`dword_824400EC + 16`. `sub_82206028` builds the trash/sell confirmation.

Ruled out during the search, recorded so they are not reopened:
`sub_821F9F38` ("CampPhoto") is the capture-time render target;
`sub_822129C8` is the capture state machine; `sub_821EC050` is the
camera-mode submenu dispatcher; `sub_82203968` is the character Status page,
despite sitting immediately next to all of this.

## Text

The album's strings live in the packed UI text block at `0x82031a00`, reached
by numeric id through `sub_8223B780("BTX ", id)` and never by pointer, which is
why none of them have code xrefs. The blob is a `BTX ` header (offset to the
first language block at +4, block count at +0xC) followed by per-language
blocks chained through their own +8; each block has its entry table at +0x14
and its entry count at +0x10, and an entry is `{u32 id, u32 offset from the
block's base}`. The English block ("USA ") starts at `0x820329a3`.

Resolved ids, English:

| Id  | String |
| --- | ------ |
| 22..30 | The status menu's tabs: Items, Status, Special Attack, Equipment, Score Pieces, **Photos** (27), Switch Character, Party Level, Save |
| 153 | "Sell Photo" |
| 163 | "Select Photo" |
| 166 | "Trash the selected photo?" |
| 180 | "You have no photos." |
| 194 | "\<g>\<m2>Photos" |
| 207 | Caption drawn over an undeveloped photograph |
| 209 | Unit suffix used by the album's counters |
