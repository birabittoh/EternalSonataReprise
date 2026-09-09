# The flag system

The game's memory of what has happened is one bit array. Chests, cutscenes,
doors, party changes: each is a bit that a field script sets and other field
scripts test. This documents where it lives, how it is addressed, and how the
mod API (`src/eternalsonata_flag_api.h`) exposes it.

## 1. Where it lives

| Address | Size | What |
|---|---|---|
| `0x8243C369` | 2046 | Flag bank, 16368 bits |
| `0x8243CB67` | 2 | Scenario counter, big-endian u16 |

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

Three saves at different points in the story, block offset `0x139` onwards:

```
save "late"    0x21:0x40  0x26:0x70  0x3A:0xC0  0xA9:0xFE  0xC4:0x3F
save "early"   0x21:0x40  0x26:0x30  0x3A:0x40  0xA9:0x3E  0xC4:0x07
```

Every byte in the later save is a superset of the earlier one, and the set
bits fill from bit 0 upward within a byte before the next byte is touched.
That is a bit array written in index order, so flag `n` is
`bit (n & 7)` of `byte (n >> 3)`, counting from the low bit.

The rest of the region is zero, in every save, which is what rules out its
being a byte array or a set of counters.

## 4. The scenario counter

The last two bytes of symbol 547's region do not read as bits: they held
`0x046A` and `0x0834` in the two saves above, growing with progress. The debug
room asks for exactly one such number ("シナリオカウンタ…いくつにする？",
"what shall I set the scenario counter to?"), so those two bytes are it and
the flag bank proper is 2046 bytes.

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
