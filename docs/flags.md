# The flag system

The game's memory of what has happened is one bit array. Chests, cutscenes,
doors, party changes: each is a bit that a field script sets and other field
scripts test. This documents where it lives, how it is addressed, and how the
mod API (`src/eternalsonata_flag_api.h`) exposes it.

## 1. Where it lives

| Address | Size | What |
|---|---|---|
| `0x8243C369` | 2048 | Flag bank, 16384 bits (symbol 547) |
| `0x8243CB69` | 4 | Scenario counter, big-endian u32 (symbol 500, offset 0) |

Both sit inside the 4412-byte block at `0x8243C230` that `sub_82241190` writes
to the save and `sub_82240AF8` restores, so flags persist exactly like party
and inventory do. `sub_820F91A8` (startup) and `sub_82240D40` (returning to
the title) zero the whole block, which is the new-game reset.

## 2. Why nothing in the exe touches it

`0x8243C369` has no cross-references anywhere in `default.xex`. The scripts
are its only user, and they do not reach globals by address: the `.e` loader
resolves numbered symbols.

`sub_820FF028(table, count, base_id)` registers a symbol table. The exe
registers several; `sub_820F91A8` registers `off_8240C6E0` with
`count = 49, base_id = 500`, and that table is 49 raw pointers into the
`0x8243C230` block. So script symbol `500 + n` is `off_8240C6E0[n]`:

```
500 -> 0x8243CB69      ...        546 -> 0x8243C368
502 -> 0x8243C230      547 -> 0x8243C369   <- the flag bank
503 -> 0x8243C234      548 -> 0x8243CB69
```

Symbol 547's region runs to the next entry, 2048 bytes. `sub_820FF748` walks
a script's block2 fixup table of `{symbol_id, patch_offset}` pairs and adds
the symbol's address to the dword already at that offset, so the addend in the
file is a byte offset into the region and the runtime index is computed by the
bytecode.

The VM itself (`sub_820FFE28`, see `script-vm-notes.md`) calls out exactly
once, through `bctrl` at `0x821010AC`, to a function pointer the fixups
patched in. That is how scripts call the engine; the flag bank is the other
half, the part they address directly.

## 3. Why it is bits, low bit first

Two saves at different points in the story, offsets into the bank (block
offset `0x139` onwards). In `savedata.txt` the block starts at
`16 + 24 + thumbnail + 4 + 2324 + 2048 + 480 + 480`, in `sub_82241190`'s
write order after the 16 byte `SAV` header:

```
save "late"    0x25:0x40  0x2A:0x70  0x3E:0xC0  0xAD:0xFE  0xC8:0x3F
save "early"   0x25:0x40  0x2A:0x30  0x3E:0x40  0xAD:0x3E  0xC8:0x07
```

Every byte in the later save is a superset of the earlier one, and the set
bits fill from bit 0 upward within a byte before the next byte is touched.
That is a bit array written in index order, so flag `n` is
`bit (n & 7)` of `byte (n >> 3)`, counting from the low bit.

The rest of the region is zero, in every save, which is what rules out its
being a byte array or a set of counters. At this alignment the set bits are
ids the scripts pass to the flag helpers (§6): 147 of 199 in a late save, the
rest computed at run time.

The scripts touch the bank only through three `lib.e` exports, each taking the
flag id and indexing `sym547 + (n >> 3)`, bit `n & 7`, unchecked: set (118,
`0x11F2`), clear (119, `0x121D`) and test (120, `0x1249`). Every constant id
in the 360 scripts is below 1925.

## 4. The scenario counter

Symbol 500 points just past the bank, and its first word is the scenario
counter: scripts read it as a u32 (1243 loads) and compare it to values such
as 7100, and `sub_82241190` stores it divided by 1000 as the save's chapter
byte. Saves hold 1010 to 4100. The debug room asks for exactly one such number
("シナリオカウンタ…いくつにする？", "what shall I set the scenario counter
to?"). Symbol 500 runs on to the end of the block, so the counter is the
first field of a larger record.

## 5. The debug room

`cfdata/zzz01.e` is a debug map whose text (`python scripts/btx.py --lang JPN
extracted/e/cfdata/zzz01.e`) is a menu of chapter warps, event jumps, battle
setup, a scenario-counter setter and:

```
28  どのフラグを切り替えるんだい？
    0を指定すればフラグが全てリセットされるよ。
29  %d のフラグを %s にしたよ。
32  フラグを全てリセットしたよ。
```

"Which flag shall I toggle? Specify 0 and every flag resets." That is the same
bank, and `EternalSonataResetFlags` is the same operation.

## 6. Finding a specific flag

There is no name table; the game ships none. Diff instead: read the bank with
`EternalSonataCopyFlags`, do the thing in game, read it again, and compare.
The bit that turned on is the flag for what you just did.

## 7. The PS3 release

The PS3 flag system is the 360's, extended with new ids, with two symbol shifts:

* **Storage**: same. The map state block (`0x9E8340`) is 4412 bytes, the bank
  is at `+0x139` and the counter at `+0x939`. The bank is symbol 548 (the
  PS3's 500 table inserts 540, see `ps3-assets.md` §2); symbol 500 is
  unchanged. The PS3 save writer `sub_3752D0` writes the block whole, and
  `sub_3AC840` reads the counter for the save header, as on the 360. The PS3
  save is still a different layout: the party block is 2604 bytes (360: 2324)
  and the two 48 byte stat arrays are twelve wide (576 against 480).
* **Helpers**: same bytecode, ids one higher: set 119 (`0x123D`), clear 120
  (`0x1268`), test 121 (`0x1294`). A PS3 export inserted before them (118,
  `0x1230`) shifts them. So on the PS3, `sym119` sets a flag; on the 360 it
  clears one.
* **Ids**: the 624 constant ids both releases use are used by the same scripts
  on both, so a flag means the same thing on either. The PS3 adds 54 and drops
  3:

| ids | PS3 scripts | what |
|---|---|---|
| 1931..1954 | `lam01..10`, `sbi02..05`, `bel01`, `ara07`, `tnt05` | PS3 maps and events |
| 1964..1972, 1977..1989 | `sbi03..07`, `bqm46`, `fmt05..08`, `bel01`, `tnt40`, `bqj12/13`, `zzz01` | new scenes on 360 maps |
| 1973..1976 | set by `Tnt03.e` (`0x3437`) for each costume 5027 reports unlocked; tested in `tnt40`/`tnt45` (1975), `rty04` (1976) | costumes: ALG 2, PLK 2, PLK 3, BET 2 |
| 31 | `lib.e` grant (`0x5EF9`), `Tnt03.e` | a costume was granted at least once (gates a one time message) |
| 419, 420 | `dld17`, `zzz01`, `zzz02` | |
| 599, 603 | `kzf62` | |
| 1626 | `sbi02` | |
| 993, 994 (360 only) | `tnt40` | scene gate, with the scenario counter at most 7100 |
| 1478 (360 only) | `ftm45` | |

No 360 save sets any PS3 only id (highest set bit in the saves surveyed:
1920), so a 360 save carries over without collisions. Every PS3 id fits the
same 16384 bit bank. In PS3 mode `ps3_natives.cpp` already maps symbol 548
onto the 360 bank, so PS3 scripts set and test the 360 bits directly.
