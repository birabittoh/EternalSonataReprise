# Piano Music

Reverse-engineering notes for the Piano Music menu, the gallery of Chopin piano
pieces. The mod-facing surface built on top of this is
`src/eternalsonata_piano_music_api.h`, implemented in
`src/piano_music_system.cpp`.

Addresses are guest addresses in the retail `default.xex`.

## Storage

There are exactly seven pieces and the list cannot grow: `sub_8222B348` walks a
seven-entry id table and lays the rows out from that same walk.

| Address      | Type     | Meaning |
| ------------ | -------- | ------- |
| `0x8255EE70` | u8[100]  | Collectible flags, 1 for "obtained". Entries 80..86 are the seven pieces; the rest belong to other collectibles. Saved and restored whole. |
| `0x82016134` | u32[7]   | Flag id per piece in menu order: 80, 81, ... 86. Ends at `0x82016150`. |
| `0x82029D30` | u8[7]    | History page count per piece: 8, 5, 5, 6, 7, 7, 9. |
| `0x8243F35C` | u32      | Session-only "available anyway" bitmask, bit *i* per piece. Not saved. |
| `0x8243F360` | u32      | The piece the player has opened, 0..6. |
| `0x8243F364` | u32      | Current history page, 1-based. |

The flag number doubles as the number in the piece's own music file,
`sound\cxs\MP1<n>.wav`. That is what lets `sub_821FBBD0` take a filename,
recover `n` from its two digits, and light the matching flag: below 80 it sets
`flags[n]`, at 80 and above it sets both `flags[80 + n % 10]` and
`flags[90 + n % 10]`. `sub_820E8030` is the script VM's single-flag setter,
which range-checks against 100.

`sub_82241190` writes the 100 bytes into the save and `sub_82240AF8` reads them
back, both only when the save's version field is at least 2.

## The unlock test

`sub_8222B348` builds the list. For row *i* it draws the real title when

```
flags[flag_id[i]] != 0 || (session_mask & (1 << i)) != 0
```

and the "???" placeholder otherwise. `sub_8222A168`, the row activation, tests
exactly the same pair before letting the piece be played, so a piece that shows
a title is always playable.

`sub_8222B260` is the only thing that ORs bits into the session mask, from what
the `PIANO_CHECK` screen (`sub_82209758`, screen id 13) found. Because nothing
saves it, `src/piano_music_system.cpp` reports the two halves separately as
`unlocked_saved` and `unlocked_session`, and clears the session bit as well as
the saved flag when a mod locks a piece.

## Text

The menu's strings live in the packed UI text block at `0x8203DD60`, reached by
numeric id through `sub_8223B780("BTX ", id)`. The blob is a `BTX ` header
(offset to the first language block at +4, block count at +0xC) followed by
per-language blocks chained through their own +8; each block has its entry
count at +0x10 and its entry table of `{u32 id, u32 offset from the block's
base}` at +0x14. The English block's entry 0 is the first piece's title.

| Id | String |
| --- | --- |
| 0..6 | The piece titles: Raindrops, Revolution, Fantaisie-Impromptu, Grande Valse Brillante, Nocturne, Tristesse, Heroic |
| 9 | `<g>???`, drawn in place of a locked piece's title |
| `10 * i + 9 + p` | History page `p` (1-based) of piece `i`, so the pages of piece `i` run from `10 * (i + 1)` for `flags_pages[i]` ids |

`sub_8222A898` opens a piece: it sets the title from `10 * (index + 1)`, takes
the page count from `0x82029D30[index]`, and starts
`sound\cxs\MP1<n>.wav`, adding 10 to `n` for one audio configuration.
`sub_8222B8D8` is the page turn.
