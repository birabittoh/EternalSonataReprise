# Music

Reverse-engineering notes for the status menu's Music entry, the three tab
gallery of the game's OST. The mod-facing surface built on top of this is
`src/eternalsonata_music_api.h`, implemented in `src/music_system.cpp`.

Addresses are guest addresses in the retail `default.xex`.

## Storage

| Address      | Type       | Meaning |
| ------------ | ---------- | ------- |
| `0x8255EE70` | u8[100]    | Collectible flags, 1 for "obtained". Entries 1..66 are the OST tracks; 80..86 are the piano music (`piano-music.md`) and the rest belong to other collectibles. Saved and restored whole, only for save version >= 2. |
| `0x8238E128` | u8[3][32]  | Track id per row, per tab, in menu order. Rows past the tab's count are zero padding. |
| `0x822FF594` | u8[3]      | Rows per tab: 22, 31, 9. |
| `0x82053E10` | `BTX ` blob| Track titles. Text id == track id; id 0 is `???`. |

Track ids run 1..66: `sub_821FEBC8` range-checks `track_id - 1 <= 0x41` before
building a row. 22 + 31 + 9 = 62 of them are placed in tabs, and the union of
the three rows has no duplicates.

The id doubles as the number in the track's own `sound\cxs\MP1<nn>.cxs`, the
same trick the piano music uses with `MP1<n>.wav`.

## The unlock test

`sub_821FEBC8(row_obj, track_id, number, tab, x, y, ...)` builds one row, and
the whole locked/unlocked decision is:

```c
if (byte_8255EE70[track_id])
  title = sub_8223B780(0x82053E10, track_id);  // the real title
else
  title = sub_8223B780(0x82053E10, 0);         // "???"
```

`sub_8223B780`'s first argument is a pointer to a `BTX ` blob, not a fourcc;
IDA renders `0x82053E10` as the string `"BTX "` only because the blob starts
with that magic.

The `No. NN` in the left column is not the track id. `sub_822273A0` passes
`scroll + i + 1`, the 1-based position within the tab, and `sub_821FEBC8`
formats it two digits wide with `sub_821DC238`. Text id 210 of the *other*
blob, `0x82031A00`, is the `No.` label itself.

`sub_821FEBC8`'s tab argument only picks the row icon: 288, 289, 290 for tabs
0, 1, 2 (and 230 for 3 and up, which this screen never passes).

Playback, from the input handler `sub_82226858`:

```c
v12 = byte_8238E128[32 * current_tab + selected_row];
if (!byte_8255EE70[v12]) return;              // locked rows do nothing
sprintf(buf, "MP1%02d.cxs", v12);
sub_821F77F0(&unk_824251E8, buf);             // prepends sound\cxs\
```

`sub_821F77F0` ignores its first argument and starts the stream as the current
BGM, so it works outside this screen too.

## Screen state

`sub_822265C8` is the screen's init. It zeroes the eight dwords at
`0x8243F358`, which is **shared scratch every status menu screen reuses and
zeroes on entry**, so none of these mean anything once Music is closed.

| Address        | Meaning while Music is open |
| -------------- | --------------------------- |
| `0x8243F35C`   | selected row within the tab, init -1 |
| `0x8243F368`   | scroll offset within the tab |
| `0x8243F36C`   | init -1 |
| `0x8243F370`   | current tab, 0..2, cycled `% 3` by LB / RB |
| `0x8243F374`   | `(tab << 16) \| row` of the track playing, init 0xFFFF |
| `0x82440120`   | u8[3], saved scroll per tab |
| `0x82440124`   | u8[3], saved cursor per tab |
| `0x82440128`   | row count of the current tab, from `byte_822FF594[tab]` |

`0x8243F35C` is the same slot `piano-music.md` describes as the piano music's
session-only mask. Both readings are correct: the piano screen
(`sub_82229FC0`) zeroes the block on entry and then `sub_8222B260` ORs bits
into it, and the Music screen zeroes the block and parks a row index there. The
mod API cannot read that slot at an arbitrary moment, which is why
`piano_music_system.cpp` mirrors the mask host side instead.

Other routines worth knowing:

| Routine | What |
| --- | --- |
| `sub_822265C8` | screen init; allocates, zeroes the scratch block, draws tab 0 |
| `sub_822273A0(tab, animate)` | draws four rows of one tab, sets `dword_82440128` |
| `sub_82226858(a1, a2)` | input handler: tab switching, scrolling, play |
| `sub_82227630(tab, delta)` | scroll by one row |
| `sub_822278A0` / `sub_82227F08` | start playback / back out |
| `sub_821F2890` | per row helper, calls `sub_821FEBC8` |

## The tracks

The tab labels were not read out of the UI objects; the names below are what
the contents obviously are. Rows are 1-based, matching the menu's `No. NN`.

Ids **2, 6, 51 and 66** are in no tab and their entries in the title blob are
empty strings, so they are drawable but nameless: the menu never shows them.
The API reports them with `tab == -1` and refuses to play them.

Id 45's string is `<#201>tudes of the Spirit`; `<#nnn>` is the blob's escape
for a character code, here `É`.

### Tab 0, event sequence (22)

| No. | Id | Title |
| --- | --- | --- |
| 1 | 1 | Remember Me |
| 2 | 4 | Can You Recall Your Dream? |
| 3 | 54 | It's Up to You |
| 4 | 57 | From Tomorrow |
| 5 | 63 | Time Together |
| 6 | 64 | Strolling Hearts |
| 7 | 65 | A Light in the Palm of Your Hand |
| 8 | 56 | Journey of the Mind |
| 9 | 61 | Repeating Tide |
| 10 | 62 | Constant Embarrassment |
| 11 | 53 | Salsa's Theme |
| 12 | 49 | Strategy |
| 13 | 50 | Pressure |
| 14 | 52 | Rapid Fire |
| 15 | 34 | Close Call |
| 16 | 3 | Jewel of the Heart |
| 17 | 7 | Fact, Faith, and Truth |
| 18 | 8 | Light |
| 19 | 59 | Someone Special |
| 20 | 60 | Nightfall and Daybreak |
| 21 | 9 | Heaven's Mirror |
| 22 | 10 | Shape of Life |

### Tab 1, field (31)

| No. | Id | Title |
| --- | --- | --- |
| 1 | 19 | Reflect the Sky, Blossom of Life |
| 2 | 20 | Mediocrity for All |
| 3 | 21 | Different, but the Same |
| 4 | 23 | Uncertain Homefront |
| 5 | 24 | Resist and Endure |
| 6 | 25 | Quiet Defender |
| 7 | 26 | Peace Valued |
| 8 | 27 | White Mirror |
| 9 | 28 | Imprisoned Phantoms |
| 10 | 22 | A Relaxing Place |
| 11 | 30 | Underground for the Underhanded |
| 12 | 31 | Illuminated Lives |
| 13 | 29 | First Step |
| 14 | 58 | Animals Everywhere |
| 15 | 32 | Dive into the Vast Expanse |
| 16 | 33 | Wall with No Front or Back |
| 17 | 55 | Conduct the Breeze |
| 18 | 40 | Take a Stand |
| 19 | 35 | From Cruelty to Kindness |
| 20 | 36 | Silence and Life |
| 21 | 37 | Wonderland Wanderer |
| 22 | 38 | Who Wants to Die |
| 23 | 39 | The Ultimate Treasure |
| 24 | 41 | Rock and Burn You |
| 25 | 42 | Snow and Ice Boundary |
| 26 | 43 | Grim Will |
| 27 | 44 | Limitless Divide |
| 28 | 45 | Études of the Spirit |
| 29 | 46 | End of the Journey |
| 30 | 47 | Twisted Spiral |
| 31 | 48 | Illogical Theory |

### Tab 2, battle (9)

| No. | Id | Title |
| --- | --- | --- |
| 1 | 11 | Make the Leap |
| 2 | 12 | Prepared to Fight |
| 3 | 14 | Between Light and Darkness |
| 4 | 13 | I Believe |
| 5 | 15 | Your Truth Is My Lie |
| 6 | 16 | Unbalanced |
| 7 | 5 | Rebuilding Ourselves |
| 8 | 17 | Well Done |
| 9 | 18 | The End of My Days |

## Traps

* Never clear `byte_8255EE70` wholesale. The piano music owns 80..86 and other
  collectibles own the rest; only 1..66 belong here.
* The array is only saved for save version >= 2. A version 1 save loads with
  every track locked, which is the game's own behaviour.
* `byte_8238E128` and `byte_822FF594` live in the xex image, which is mapped
  read only. Reordering or extending a tab needs the `EnsureWritable` reprotect
  dance from `item_system.cpp`.
* Unlocking one of the four unlisted ids sets a flag nothing displays. The API
  allows it and reports `tab == -1`.
