# Options settings

The game's own Options settings: what each byte behind the menu rows means,
and how this project exposes them to mods. The surface mods build against is
`src/eternalsonata_settings_api.h`, implemented in `src/game_settings.cpp`.

Everything here is a set of plain bytes in one contiguous block that the game
itself writes, reads and persists across saves; a mod does not own a menu row
to reach any of it. This is deliberately separate from
`src/eternalsonata_options_api.h`, which lets a mod *add* a row of its own.
A mod may use either or neither.

## The settings block

`sub_821E7090` is the "reset every option to its default" routine and it is
the single best view of the whole block, because it touches all of it in
fifteen lines:

```c
byte_8243FBFD = 1;
byte_8243FBFC = 1;
byte_8243FBFE = 100;
byte_8243FBFF = 100;
byte_8243FC00 = 100;
byte_8243FC01 = 0;
*(u16 *)((char *)&dword_8243FC04 + 1) = 257;   // BYTE1 = 1, BYTE2 = 1
byte_8243FBF8 = dword_824408EC;
byte_8243FBF9 = dword_824408EC;
byte_8243FBFA = dword_824408EC;
sub_821E6EA8(0, 100);   // volumes go through the mixer, not the byte alone
sub_821E6EA8(1, 100);
sub_821E6EA8(2, 100);
```

The same block is written verbatim by `sub_821E5A38` (new game), `sub_821E5F58`
and `sub_821DD4D0` (both title-screen resets) and `sub_820FDFC0`, which is how
to be confident it is the complete set and not a subset.

| Address | Type | Default | What |
| --- | --- | --- | --- |
| `0x8243FBF8` | u8 | `dword_824408EC` | controller port of player 1, 0..3 |
| `0x8243FBF9` | u8 | `dword_824408EC` | controller port of player 2, 0..3 |
| `0x8243FBFA` | u8 | `dword_824408EC` | controller port of player 3, 0..3 |
| `0x8243FBFC` | u8 | 1 | ON/OFF. The Battle Camera row. |
| `0x8243FBFD` | u8 | 1 | a three-value setting, see Audio Output below |
| `0x8243FBFE` | u8 | 100 | a volume, channel 0 |
| `0x8243FBFF` | u8 | 100 | a volume, channel 1 |
| `0x8243FC00` | u8 | 100 | a volume, channel 2 |
| `0x8243FC01` | u8 | 0 | A/B, the Attack Button row |
| `0x8243FC05` | u8 | 1 | `BYTE1(dword_8243FC04)`; the Subtitles row |
| `0x8243FC06` | u8 | 1 | `BYTE2(dword_8243FC04)`; Voice, 0 = Japanese |

All of it sits inside the 2324 bytes `sub_82241190` writes from `0x8243F3E8`
(`0x8243FC06 - 0x8243F3E8 = 0x81E`), so every one of these survives a save.

## How the labels were matched

`sub_82200FE8` is the Options screen init and the only routine that reads all
of `byte_8243FBFC`, `byte_8243FBFD`, `byte_8243FBFE`, `byte_8243FBFF`,
`byte_8243FC00` and `byte_8243FC01` together. It places the row's bar from the
byte, then draws the value label next to it through `sub_82202358`:

| bar slot | byte read | label picked |
| --- | --- | --- |
| +84  | `byte_8243FBFC`  | index `102 - dword_8243F364` |
| +88  | `byte_8243FC01`  | index `dword_8243F368 + 104` |
| +92  | `BYTE2(FC04)` (= `byte_8243FC06`) | index `(BYTE2 ^ 1) + 111` |
| +120 | `BYTE1(FC04)` (= `byte_8243FC05`) | index `119 - BYTE1` |

Read against the executable's own text blob, those indices resolve to:

* +84: **Battle Camera**, values `ON` / `OFF`.
* +88: **Attack Button**, values `A` / `B`.
* +92: **Voice**, values drawn leftover then (English / Japanese; the byte is
  0 for Japanese, which is why the game reads `byte ^ 1`).
* +120: **Subtitles**, values `ON` / `OFF`.

The three volumes are drawn between and after these as three sliders reading
`byte_8243FBFE`, `byte_8243FBFF`, `byte_8243FC00` in address order, labelled
**Music**, **Sound Effects**, **Voice**. The channel correspondence (which byte
is Music, which is Sound Effects and which is Voice) is read from that label
order and matches `sub_821E6EA8`'s channel 1 being the one that touches five
mixer bytes.

## The odd one out: Audio Output

`byte_8243FBFD` has three values and an **Audio Output** label with the values
**Stereo**, **Mono**, **5.1ch Surround** (three consecutive strings in the text
blob). It round-trips through the same save region as the rest, so it is
exposed even though the retail Options screen draws no bar for it (the Xbox 360
configures audio output at the system level).

It is the one setting whose stored byte is not its menu column. `sub_82200FE8`
turns the byte into a column with `0 -> 1, 1 -> 0, 2 -> 2, else -> 1`, and the
close handler inverts that on the way back out. The API speaks the column, so
a mod reads/writes `0 = Stereo, 1 = Mono, 2 = Surround` and never sees the
byte's permutation.

The three controller bytes (`0x8243FBF8..0x8243FBFA`) are not a menu row either
on this Options screen; they are written by the player-controls screen.
`sub_82202550` reads them to place that screen's three rows, selecting the port
name from a list at `byte + 98 / 103 / 108` per player, and reassigns any player
whose port has since gone away. So a value written there does not necessarily
survive the next visit to that screen.

## The apply path for volumes is not optional

`sub_821E6EA8(channel, percent)` is what actually changes the mixer. Writing
the `0x8243FBFE..0x8243FC00` bytes on their own only changes what the menu
displays:

```c
level = (percent * 0.01f) * 127.0f;      // 0..100 in, 0..127 out
channel 0 -> audio + 75676,  and byte_8243FBFE = percent
channel 1 -> audio + 75672, +1, +5, +6, +7, and byte_8243FBFF = percent
channel 2 -> audio + 75674, +3,            and byte_8243FC00 = percent
```

where `audio` is `dword_8243D89C`. It finishes with `sub_82141FE0()`, the
commit, and returns whether the level changed. So an API write of a volume goes
through this routine, which is guest code: from off the guest main thread it is
deferred to that thread's next frame and the API answers `QUEUED`. The other
settings are plain byte stores.

## The shared scratch trap

The byte typed into the menus is staged in a shared scratch area. The Music
screen zeroes eight dwords from `0x8243F358` (see `music.md`), and the
button-configuration screen (both pages, `sub_82201DA8` and `sub_82203180`)
commits three of those dwords back into the persistent block on close:

```c
byte_8243FBFC = dword_8243F364;
byte_8243FC01 = dword_8243F368;
byte_8243FBFD = (dword_8243F36C == 1) ? 0 : (dword_8243F36C < 3 ? 2 : 1);
```

The scratch dwords are shared between screens and mean nothing while neither is
open. Read the persistent bytes, never the scratch. And the last store shows the
Audio Output permutation in use again (`1 -> 0`, `2 -> 2`, else `-> 1`); anyone
driving that screen's scratch rather than the byte gets bitten by it.

## The API

`eternalsonata_settings_api.h` is read/write keyed by an enum, in the same
shape as the score-piece half of the item API. A generic get/set keeps the
header stable and lets the toggles (`0..1`), the volumes (`0..100`), Audio
Output (`0..2`) and the controller ports (`0..3`) share one surface:

```c
int EternalSonataGetSetting(int setting);
int EternalSonataSetSetting(int setting, int value);
int EternalSonataGetSettingRange(int setting, int* min, int* max);
```

`EternalSonataSetSetting` of a volume answers `QUEUED` from a non-guest thread
(see above). Out-of-range values and unknown settings each get their own
negative error. `eternalsonata.setting.changed` is published on the mod
registry bus (u64 = setting, f64 = new value) off a once-per-frame snapshot
whenever a setting changes, by the player in a menu or by a mod; this is how a
mod that forces a setting reapplies it after new-game or title-screen resets,
which run the whole block back to defaults.