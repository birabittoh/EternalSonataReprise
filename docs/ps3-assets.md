# The PS3 release as a second target

The PS3 release (2008, BLES00444 in Europe) is a later, larger version of the
game: two extra playable characters (`pc011`, `pc012`, twelve party slots),
costumes for Allegretto, Polka and Beat, new enemies (`ep198`..`ep209`),
bosses and AI scripts, extra battle maps (`lam9x`, `sbi90`) and field areas,
and new events and lines. Its engine differs from the 360's: more script
natives, a different map state block, more task lists, and slot addressed
containers in a different order.

The USA and JP 360 releases run the same engine as PAL, so their executable
and containers are patched into PAL's and their scripts run unchanged
(`scripts/gen-release-patches.py`). That cannot work for the PS3: its scripts
and data assume engine behaviour the PAL executable lacks. The PS3 release is
therefore a **second target** of this executable:

* `game_data_root` may point at a PS3 copy, and the game detects it and runs in
  PS3 mode. A PS3 copy alone must be enough to play.
* PS3 scripts run **unmodified** on a PS3 script VM with the PS3's natives,
  state block and task lists, separate from the 360 VM. PS3 bytecode is never
  rewritten into 360 bytecode.
* PS3 engine features (twelve slots, costumes, camp menu flags, PS3 slot
  layouts, PS3 only maps) are implemented on the host, active only in PS3
  mode, with their own save state.
* Formats that carry no game logic (textures, vertex packing, colour byte
  order, audio) are converted into what the 360 renderer and audio system
  read, once, at install (`src/installer/ps3_install.cpp`, with
  `scripts/ps3_convert.py` as its reference).

The eight null slots appended to BattleKeep (§4) are a stopgap that goes away
as PS3 slot addressing replaces the 360's.

The 360 decoded tree (`scripts/unpack_e.exe` output under `extracted/`) is the
reference for format work: almost every PS3 file has a 360 counterpart, so a
format rule is only trusted once converting the PS3 file reproduces the 360
bytes. `ps3_convert.py --verify` runs that comparison.

## 1. Getting the files

`PS3_GAME/USRDIR/archives/*.files` holds everything; the rest of the disc is
XMB metadata and the encrypted `EBOOT.BIN`.

```
"FILE"  u32 archive size  u32 count  u32 0
count x { char path[32]; u32 offset; u32 size; u8 pad[8] }   (0x30 bytes)
```

All big endian, data stored raw. Paths are lowercase with `\`, relative to
`USRDIR`, and mirror the 360 `assets/` tree.

```bash
python scripts/unpack_ps3.py assets-ps3-raw/PS3_GAME/USRDIR/archives -o assets-ps3
```

The guest asks for mixed case names (`btldata\BattleKeep.bop`), but every
host lookup folds case (the SDK's `HostPathDevice`, the asset system's
`ResolveUnderRoot`), so the lowercase names serve as they are. Unlike the 360 the PS3 applies no codec: every file is already decoded, which
is the form `ps3_convert.py` and the 360 tools work on.

Then convert the tree in place and point `game_data_root` in
`eternalsonata.toml` at it:

```bash
python scripts/ps3_convert.py assets-ps3
```

Like a USA or JP copy, the tree is converted where it lies: each convertible
PS3 file is written over itself, the shipped file kept as `*.orig`, and every
run starts from those (`ps3-shipped.txt` lists what the PS3 shipped, so files
a run added are never taken for PS3 data, and a run first deletes every
other file). No 360 file is needed. `index.vmtoc` is built from scratch, a
stored record for every file. The PS3 only lacks seven files the 360 lists
(`cfdata/e0041.e`, `e1101.e`, `e7080_010.e`, `e8020.e`,
`btldata/map/zzz90..92.bop`); nothing PS3 mode runs opens them: the first
two were start events the PS3 tables replace, the others are reached only
from 360 scripts.

The game does the same itself (`src/installer/ps3_install.cpp`, a port that
must match the script byte for byte): pick a PS3 disc folder (or point
`game_data_root` at an unpacked tree), and it unpacks and converts once,
then writes `ps3-convert.stamp`. `ps3_convert.py --check` converts without
writing and compares every output with the tree, which is the regression
test for the port.

The game runs in PS3 mode when `game_data_root` holds `pcalg_v1.p3obj`, a
file only the PS3 ships (`src/core/target.cpp`, `IsPs3Target()`). PS3 mode
skips every USA/JP container patch and registers the PS3 natives; 360 mode
does neither.

Both modes share saves. `sub_82240AF8` loads a save as raw copies of the
engine's globals (party block, stats, map state block), and PS3 mode runs the
same executable, so the bytes are the same layout either way. A 360 save
continues in PS3 mode. A PS3 save made on a PS3 only map would not load on
360 data, and twelve slots and costumes do not fit the party block, so that
state goes in a host side record saved alongside (`reprise.txt` in the save
container, `src/engine/save_record.cpp`), which 360 mode can later use to
refuse such a save. It holds the costumes worn, the PS3's costume unlocks and
its camp menu flags so far.

`--verify extracted/e --verify extracted/other` compares converted models
against the decoded 360 release.

| PS3 | 360 | Notes |
|---|---|---|
| `.e`, `.bop`, `.bmd` | same | Same containers, same chunk tree; see §3 |
| `.p3tex` | `.x3tex` | Map textures: a bare `NTX3` chain vs `NTX2` |
| `.p3obj` | none | Field character models and costumes, moved out of `AppKeep.bmd` |
| `.cps` | `.cxs` / `.wav` | Music, PS-ADPCM or PCM; see §5 |
| `.csf` | `.csf` | Same banks, ATRAC3 clips; see §5 |
| `.fnt` | same | Same format; the PS3's glyphs are a superset, served as they are |
| `.tex` | same | 2D animation: `NTX3` textures where the 360 has `NTEX`; see §4 |

## 2. Scripts

The `.e` bytecode comes from the same compiler: `bos01_v1.e` differs only in
its header id and timestamp. What a script means, though, depends on the
engine's natives, state block and task lists, and those differ. This section
records the PS3 engine's side; the PS3 VM has to reproduce it.

### Interpreter

`sub_3B0B18` is `sub_820FFE28`: same context layout (`+8` state, `+0xC`
sleep count, `+0x18` acc, `+0x20` ip, `+0x24` sp, `+0x28` fp), same 0x8A
opcodes with the same semantics, checked handler by handler (jump table at
`0x3B0BB8`, 32 bit offsets from its own base). GCC duplicated the bodies the
360 shares (`03`/`07`, `38`/`39`) without changing them. The running context
is kept at `G_vm+0xC` (`G_vm = 0x9EA480`, the 360's `dword_824405FC`) and
`7e` sets the yield flag at `G_vm+0x120`. The one encoding difference is
`7d`: its patched operand points at an 8 byte function descriptor instead of
code. So PS3 bytecode runs on the 360 interpreter as is; what the PS3 VM needs
is its own native tables, task lists and state block.

### Party natives 5026..5033

Of every native id imported by any PS3 script, only seven are missing from
the 360 executable's tables (5028 is never imported):

| id | used by |
|---|---|
| 5026, 5032 | `lib.e` |
| 5027 | `Tnt03.e` |
| 5029 | `Bel01.e` |
| 5030, 5031 | `sbi02.e`..`sbi05.e` |
| 5033 | `Dld17.e` |

The 5000 range is the party table (`off_8240CA88`, 26 entries on the 360, 34
on the PS3). `sub_820FF748`'s range check is inclusive, so an unregistered
5026 silently resolves to the first battle native. Their PS3 code (table at
`0x7795B8` in the EBOOT) takes a 0 based character:

| id | PS3 | behaviour | used for |
|---|---|---|---|
| 5026 (c, v) | `0x381E18` | unlock costume v of c | `lib.e` grants ALG 2, PLK 2, PLK 3, BET 2 |
| 5027 (c, v) | `0x381DD8` | costume unlocked (v 1 always) | `Tnt03.e` text 1973..1976 |
| 5028 (c) | `0x381DA0` | selected costume, default 1 | |
| 5029 (x) | `0x381D60` | camp menu flag, block `+0x921 = x != 0` | `Bel01.e` |
| 5030 (c) | `0x381D18` | c has joined (display position > 0) | `sbi02..05` |
| 5031 (c, d) | `0x381CD8` | HP += d, clamped to [1, max], both stat copies | `sbi02..05`: -50 to everyone joined |
| 5032 (x) | `0x381C98` | camp menu flag, block `+0x920 = x == 0` | `lib.e` |
| 5033 (c) | `0x381C10` | c at display position 1..3 | `Dld17.e` picks a line |

The camp main menu (`sub_2728E8`, `sub_273788`) gains Costumes when any
costume is unlocked, and Scrapbook, a PS3 only camp screen, when `+0x921`
(5029, set by `Bel01.e`) is. `+0x920` (5032) is read by the costume page
(`sub_2733A0`).

Costumes exist only for Allegretto, Polka and Beat (`pcALG_v2`, `pcPLK_v2`,
... models, a "Costumes" camp menu). The PS3 party block (`G+0x820`, the
360's `0x8243FC08`, grows from 10 to 12 entries) keeps unlocks at
`+0x919..+0x91C` and the selected variant at `+0x91D..+0x91F`;
`sub_1E5840` turns the selection into a model index.

In PS3 mode `src/engine/ps3_natives.cpp` currently registers these eight
into the 360's party table. The roster and HP natives work on the twelve wide
party arrays; costume unlocks and the two
menu flags are host state saved in the save record (§1), and 5028 answers the
costume system's selection. That is interim: these natives belong to the PS3
VM, backed by twelve slot state.

The PS3 also stubs two 360 natives: 5021 (Xbox rich presence) returns 0 and
5022 (achievement write) returns 1.

### Character models (1141)

Native 1141 with kind 0 returns a field character model for 1062. On the 360
it indexes `dword_82420AF8` with 1..10 (ALG, PLK, BET, CPN, VOL, SLS, JRB, FST,
MCH, CLV). The PS3's (`sub_385D10` -> `sub_80610`) keeps that and adds
negative ids, which PS3 events use where the 360 ones pass 1..10:

| id | model |
|---|---|
| -10, -11, -12 | the worn costume of ALG, PLK, BET: `pc%s_v%1d.p3obj`, loaded per character by `sub_801F0` into 320 byte slots named `WEARALG`, `WEARPLK`, `WEARBET` |
| -20..-28 | `AppKeep2.bmd` entries 0..8: CRS, SRN, CPN, VOL, SLS, JRB, FST, MCH, CLV (`sub_80C40`) |

Any other id n is AppKeep slot n - 1 on both releases (the field's "?"
bubble over an interactable object is PS3 50, 360 51), so PS3 mode
translates it like 1052 (§4); read untranslated, the bubble showed the gold
sparkle one slot below it.

Both are raw `NOBJ` pointers, like the 360's (`sub_82162058` stores
`AppKeep.bmd`'s entry pointers in `dword_82420AFC`), and the PS3 keeps both
resident. PS3 mode loads them with the rest of `AppKeep.bmd` (§4): the
model table gets `pc*_v1.p3obj` and AppKeep2 entries 2..8, and CRS and SRN
are loaded after them, into physical memory when the `APPKEEP` heap is full;
guest physical memory once ran out late in a session with ~48 MB more
resident. `src/engine/ps3_models.cpp` serves -20 and -21 from those and
every other id from the model table slot of the same character. The costumes are the
costume system's ([costumes.md](costumes.md)), which writes the worn one into
that slot, so -10..-12 follow it like the field and battle do.

### Task lists

Builtins 5 (spawn task, `sub_82102500`) and 7 (spawn child task,
`sub_82102538`) take the priority as `args[2]` (the third push back). On the
360 it indexes 16 task lists, `dword_8243D800` (heads) and `dword_8243D840`
(tails), in `sub_8212DD10`. PS3 scripts pass 17 and 18 where the 360 passes 8
and 9 (one native 5 call in `lib.e`; 211 native 7 calls in `cfdata` and the
battle tutorials), so the PS3 engine has more lists. It has 32 (`sub_30B6B8`, builtin
7's insert, heads at `T+4`, tails at `T+0x84`, `T` from TOC slot `0x541EFC`),
run in index order each frame by `sub_30B438`; the task object links at
`+20`/`+24` and keeps its list at `+28` (the 360's `+24`/`+28`, `+32`). On
the 360 VM an index of 16 or more reads a tail as the head and writes past the
tails: it corrupts memory and faults storing to `0x1C` (loading a save in the
Ritardando sewers).

Engine tasks are named, so the lists match by the constant each executable
spawns the same task with (360 `sub_8212DD10` and the task constructor
`sub_820E64F0`; PS3 `sub_30B580` and `sub_30B6B8`):

| 360 | PS3 | tasks |
|---|---|---|
| 1 | 4 | GetPad, SignalManager |
| 3 | 8 | FontManager_Task |
| 4 | 11 | MenuSelect |
| 5 | 12 | ActionTask, CAMP_TASK |
| 6 | 15 | btlmanager |
| 7 | 16 | the constructor's default: most tasks |
| 8 | 17 | TextMgr Task; `lib.e`'s one native 5 |
| 9 | 18 | script tasks the engine spawns (`sub_82101D70` / `sub_3B6588`, `sub_82101E08` / `sub_3B6380`) |
| 9 | 20 | ObjectQueueTask |
| 10 | 21 | ObjectList TASK |
| 11 | 24 | BtlEndTask, CF Demo, OP/ED_CREDIT_TASK, NowLoading, SwapEffect |
| 13 | 28 | ChangeMode |
| 15 | 31 | DrawSync |

No 360 engine task uses lists 0, 2, 12 or 14. Scripts pass 17 (`lib.e`), 18
(211 native 7 calls in `cfdata` and the battle tutorials) and, in the battle
AI, 9 or a parameter of a helper that the AI calls with 9. The AI files are
the 360's byte for byte, so on the PS3 their child tasks run in list 9, ahead
of btlmanager, where the 360 ran them in its list 9 after it.

In PS3 mode, builtins 5, 6, 7 and 19 (`sub_82102500`, `sub_82102518`,
`sub_82102538`, `sub_821029A0`; the list is `args[2]`, `args[3]` for 6)
translate the list before spawning (`src/engine/ps3_natives.cpp`). A PS3 list
between two mapped ones goes to the lower 360 list, or to an unused one, so
every task keeps the PS3's order against every engine task: 0..3 -> 0, 5..7 ->
2, 9..10 -> 3, 13..14 -> 5, 19 -> 9, 22..23 -> 10, 25..27 -> 12, 29..30 -> 14.
Engine spawned scripts stay in list 9, which is why 18 maps there too: a
script and the children it spawns share a list, as on both releases.

### Map state block (500 series)

The 500 series natives return pointers into the map state block (the 360's
`dword_8243C230`, the PS3's `0x9E8340`, 4412 bytes on both). The two tables
(`off_8240C6E0`, `0x779910`) give the same offsets up to 539; the PS3 then
exports one more word at `+0x118` as 540, which no 360 code touches, so each
later PS3 id is the 360's plus one (541, `+0x120`, is the running skip task
slot `dword_8243C350`). Both registrations pass one id more than the table
holds: the 360's 549 and the PS3's 550 read the next table's first word.
Only `lib.e` and `Tnt01.e` use the shifted ids. On the 360 table, `lib.e`'s
skip helper would store the skip handler into the 360's 541, so a skipped
scene suspends the event and then waits forever for it to end.

PS3 mode registers the PS3's table, with its offsets applied to the 360
block, in place of the 360's (`src/engine/ps3_natives.cpp`, a hook on
`sub_820FF028`, which `sub_820FDFC0` and `sub_82240D40` call with it).

### Other native tables

Every other table has the same base and count on both executables:

| ids | 360 table | PS3 table |
|---|---|---|
| 1..30, 100..115 | `off_8240C628`, `off_8240C6A0` (`sub_821030C8`) | `0x7799D8`, `0x779A50` (`sub_3B3218`) |
| 200..225 | `off_8240C5C0` (`sub_82105A18`) | `0x779A90` (`sub_3BB308`) |
| 500..548 / 549 | `off_8240C6E0` (`sub_820FDFC0`, `sub_82240D40`) | `0x779910` (`sub_3AC8B0`) |
| 1000..1151 | `off_8240C828` | `0x779640` (`sub_387940`) |
| 2000..2027 | `off_8240C7B8` | `0x7798A0` (`sub_39F9A0`) |
| 5000..5025 / 5033 | `off_8240CA88` | `0x7795B8` (`sub_382360`) |
| 20000..20206 | `off_8238E840` (`sub_82176078`) | `0x776FEC` (`sub_EE628`) |
| 40000..40070 | `off_8238E720` | `0x777328` (`sub_137E30`) |
| 45000..45040 | `off_8240CAF0` | `0x776F48` (`sub_7C750`) |
| 60000..60101 | `off_822F4260` (`sub_82252430`) | `0x778F88` (`sub_3606F8`) |

Same count does not mean same behaviour. Alignment was only checked
statistically: by the argument words each native reads (identical in 1, 100,
1000, 5000 and 45000 apart from compiler noise) and by floating point use,
which matches best at offset 0 in every table (e.g. 100% against at most 78%
one entry off for 2000, 93% against 81% for 40000). Tables whose PS3 natives
fetch arguments through out of line accessors (20000, 40000, `sub_E5CB0` =
`lwz r3, 0(r3)`) rely on the latter. 60016 is a null entry on the PS3. A PS3
native may still do something different from the 360 entry at the same id
(events in particular), so each one has to be compared by decompiling it
before the PS3 VM can forward it to 360 code.

Natives that behave the same on both platforms (decompiled on both sides,
float arguments checked in the disassembly where Hex-Rays dropped them):
builtins 1..30 (bar the translated ones), 100..115, 200..225, 2000..2006, 2014,
2015, 2026, 5000, 5004, 5005, 5011, 5019, 20201, 40047, 60001, 60014, 60015,
60019, 60021..60023, 60025, 60026, 60032, 60033, 60039, 60043, 60045, 60047,
60049, 60051..60053, 60056, 60057, 60068, 60075, 60080, 60084, 60086, 60095,
60100, and in the 1000 table 1008, 1010, 1021, 1022, 1023, 1025, 1028..1032,
1038, 1039, 1041, 1042, 1044, 1050, 1053..1055, 1063, 1064, 1069, 1071, 1072,
1074, 1079, 1089, 1090, 1095, 1097, 1098, 1100..1102, 1104, 1105, 1108, 1122
(bar the pad word), 1123, 1136, 1139, 1140, 1143, 1145, 1146, plus the null
stubs. Natives that differ: 2021 (stream argument) and the shop stock table,
each described below. The PS3's unit record array (kind 1) sits 632 bytes lower
than the 360's with the same stride and fields, which is why raw offsets in
native bodies such as 40047 do not match.

### Music stream query (2021)

The PS3 native takes the stream in `args[0]` (nonzero asks stream 2, zero
stream 1); the 360's always asks stream 1. In PS3 mode `ps3_natives.cpp`
hooks `sub_820F8C88` to pass the stream through to `sub_8213FC38`.

### Pad words (1122)

1122 copies a pad's held and newly pressed words out. Only PS3 scripts call
it. The PS3 word's low 16 bits are `digital1 << 8 | digital2` (menu code at
EBOOT `0x283D70` tests d-pad `0x1000` up, `0x2000` right, `0x4000` down,
`0x8000` left, with the stick folded in at `0x10000`..`0x80000` as on the
360), so `0x40` is cross. The piano in `ftm45` waits on stream 2 (2021) and
ends on it. The 360 layout has the left stick click there, so in PS3 mode
`ps3_natives.cpp` hooks `sub_820ECA18` to also set `0x40` when A is set.

### Music start position (2000)

Scripts on both releases pass a start position in seconds as `args[4]` of
2000. The 360 honours it too: `sub_820F80F8` (Hex-Rays drops its float
arguments) hands it to `sub_82142070` in `f3`, which stores it in the stream
request, and `sub_82146AF0` skips `rate * seconds` samples of the PCM data.
Nothing to port. 2009..2013 read extra floats on the PS3 and are probably the
same.

### Shop stock

The native (5012, `sub_8222C190`) is the same. The stock table differs: the
PS3 keeps 16 records of 74 bytes (`{u16 id, u16 count, u16 items[35]}`) at VA
`0x47A0F8` (file offset `0x46A0F8`), with restocked shops, new items and shop
13 (`sbi05`, a PS3 only map). `scripts/ps3_shop_stock.py` writes it at build
time into `ps3_shop_stock.generated.inc`. Shops 10 and 12 stock 35 items but
the 360's list holds 32, so `src/engine/shop_stock.cpp` remaps the list
(`word_82560114` through the id byte at `0x82560199`) into a buffer with room
for 35. Accesses to the count and id bytes are told apart from records 33..35
by instruction address. Two midasm hooks (`config/shop.toml`) raise the fill
loop's end and, in PS3 mode, point its source at the PS3 table.

### Button prompts

PS3 text names buttons with icons: `<ibN>` is icon N + 54 and `<ib>` the
decide button (circle when the pad's decide mask is 0x20, else cross), both
the 360's `<iN>` control code 25. The PS3 icon table (`0x46B1C8`) maps
`<ib1>`..`<ib3>` to circle, cross and triangle; the 360's (`0x82074AC0`, AppKeep
slot + 1) has only 44 entries. No 360 text uses code 25, and its layout case
places a sprite that never shows; the PS3 adds the icon to the line's glyphs
instead, as both releases do for the code 24 arrows.

Native 45039 reads the same config byte on both (`byte_8243FC01 == 0`), but
the PS3 battle tutorial treats it as "attack on circle" and highlights circle,
where the 360 one means A.

PS3 mode (`src/engine/ps3_buttons.cpp`, `config/ps3.toml`) rewrites the tags
to `<i55>`.. and `<i0>` before the preprocessor, lays those icons out through
the code 24 case with the AppKeep button texture, untinted, and answers 45039
with "attack is on B".

### Starting events

`sub_820FEDC0` preloads a start's `E%04d.e` and `sub_820FEC48` loads its map,
indexed by start kind (a new game is 0). `dword_82081BC8` (`.rdata`) holds the
event numbers, which `sub_820FEC48` also reads as the spawn position;
`off_8240C7A4` holds the maps, but a new game loads a hardcoded `"tnt01.e"`
(`0x82082C28`). The PS3's tables (`0x517EBC`, `0x47C1C8`) and hardcoded map
(`sub_3A9AB8`, `sub_3A9F58`) differ: a new game opens with the PS3 only
prologue `E0005` on `tnt03` instead of `E0010` on `tnt01`.

| index | 360 | PS3 |
|---|---|---|
| 0 | `tnt01`, 10 | `tnt03`, 5 |
| 1 | `kts01`, 1141 | `kts01`, 1141 |
| 2 | `prs01`, 41 | `cbs60`, 2231 |
| 3 | `tnt03`, 1101 | `agg02`, 1231 |

PS3 mode writes the PS3's values into the 360 tables and rewrites the
hardcoded name in place before the first start
(`src/engine/ps3_start_events.cpp`).

### Script symbols

`lib.e` exports nearly the same symbol ids on both releases (block2 table 2):
the PS3 one adds 147, 159, 170, 173, 174 and lacks 149, 165, 171, and no other
PS3 script imports the new ones. PS3 mode runs the whole PS3 script set
together, so PS3 maps never meet the 360 `lib.e`.

### PS3 executable

`EBOOT.BIN` decrypts with `rpcs3 --decrypt`; the result is a PPC64 big endian
ELF. The TOC is `0x5415F8` (every function descriptor's second word, starting
with the entry descriptor at `0x51D5E0`); IDA does not set it and has to be
told in the processor options. Native tables hold pointers to 8 byte
descriptors (code, TOC), and table pointers are loaded from TOC slots.
`sub_3B00F8` is `sub_820FF028`. The party block `G` is `0x828EB0` with the
same leading layout as the 360's `0x8243F3E8` (gold at `+8`); its stat arrays
sit at `G+0xAD0` (the 360's `G+0x920`), same 48 byte stride. The map state
block is not identified yet.

## 3. Model chunks

Both releases use the same chunk tree (`tag`, `u32 size` including the header).
`CreateModel` (`sub_82114960`) walks an `NMDL`'s children by tag, skips
unknown tags, and skips `NPAD` inside texture lists, which is what lets a
conversion leave padding behind instead of moving data.

```
NOBJ
  NPAD                      aligns what follows (4 KiB on 360 for XPR2, 128 on PS3)
  NMDL  header 64 bytes, 96 when flags(+32) & 2
    NSHP NMTR NTEX/NTX2/NTX3 NLC2 NCAM NBN2 NMTN NATR NLIT NFOG NOL2 ...
    NLOB  u32 count, u32 offsets from +8, then placed NOBJs (map objects)
    NLEF  u32 count, u32 offsets from +8, then Mefc effects
    NMR2
```

Surveyed over the 805 files present on both releases:

| Chunk | Difference | Handled |
|---|---|---|
| `NMDL` | version byte `0x83`, 360 accepts only `0x82` | yes |
| `NTX3` | GTF texture, 360 wants `NTEX`/`NTX2` | yes |
| `NSHP` | RSX vertex packing | yes |
| `NMTR`, `NLIT`, `NFOG`, `NCLC`, `NOL2` | colours RGBA vs ARGB | yes |
| `NLOB` | offsets move when children resize | yes |
| `Mefc` | directory addressed sections (§3.4) | yes |
| `NLEF` | `Mefc`s with embedded models | yes |
| `NATR` | colour array RGBA vs ARGB (§3.3) | yes |
| `NMR2` | morph vertex records, CMP packed (§3.2) | yes |
| `NBN2`, `NCAM`, `NLC2`, `NCLS`, `NDYN`, `NMRP`, `NMTN` | float drift from re-export, or identical | nothing to do |

### 3.1 Textures

An `NTX3` is a one-texture GTF:

```
+0x00 "NTX3"  u32 size
+0x08 u32 count (1)   u32 id   u32 offset (0x80, from the chunk tag)   u32 byte size
+0x18 CellGcmTexture: u8 format, u8 mips, u8 dimension, u8 cubemap, u32 remap,
      u16 width, u16 height, u16 depth, u8 location, u8 pad, u32 pitch, u32 offset
```

Formats shipped: `0x86` DXT1, `0x88` DXT5, their `| 0x20` linear variants for
non power of two sizes, and eight `0xA5` linear A8R8G8B8. Linear textures keep
the base level's row pitch on every mip level, so their chain is repacked tight
for the DDS (the 1280x720 title background is 931,840 bytes on the PS3 and
614,760 on the 360). The DXT payload is byte identical to the 360 `NTEX`'s,
so the conversion writes the same DDS header the 360 files carry (`flags
0xA1007`, `caps 0x401008`, or `0x81007` / `0x1000` with one level) and copies
the pixels. `NTEX` is `0x88 + size` and `NTX3` is `0x80 + size` rounded up to
128, so an `NTEX` usually fits with an `NPAD` filling the rest and nothing
after it moves. When it does not, the model tree grows instead; a texture
inside a `Mefc`, which cannot move, is then left unconverted with a warning.

Map textures (`cfdata/maptex/*.p3tex`) are the external texture list
`CreateModel` receives as its third argument, which accepts `NTEX` as well as
`NTX2`, so they convert to a plain `NTEX` chain served as `.x3tex`. The 360
pairs maps with maptex files and `sound\mapSE` banks through a sorted table
(`off_820166A0`, 332 entries of map, maptex, mapSE, searched by
`sub_820FA680`); a map without an entry keeps the previous map's textures.
The PS3's (`0x516E2C`, 351 entries) is the 360's plus `cbs60`, `lam01..10`,
`lam14` and `sbi01..07`, which PS3 mode answers when the 360 table has none
(`src/engine/ps3_maptex.cpp`). Their area names, and those of every other
PS3 only cfdata file, are in `docs/cfdata_names_ps3.txt` (`cfdata_names.py
--cfdata assets-ps3/cfdata --exclude docs/cfdata_names.txt`), which the area
overlay and the Discord presence use in PS3 mode.

### 3.2 Meshes

`NSHP` names its vertex format with the u16 at +0x1C; `sub_82131148` turns it
into a declaration. Both releases store the same elements in the same order:

| bit | element | 360 | PS3 |
|---|---|---|---|
| `0x1` | position | FLOAT3 | same |
| `0x400` | weights, indices | FLOAT3 + UBYTE4 | one CMP dword + UBYTE4 |
| `0x2`, `0x4` | normal, tangent | DEC3N (10:10:10, x low) | CMP (11:11:10, x low) |
| `0x8` | colour | D3DCOLOR (ARGB) | RGBA |
| `0xF0` | n x FLOAT16_2 texcoords | same | same |
| `0x30000` | n x FLOAT2 | same | same |
| `0x4000`, `0x8000`, `0x41000` | extra texcoords | same | same |

Skinned meshes therefore grow by 8 bytes per vertex. Normals are quantised
independently on each release, so the conversion lands within one DEC3N step
of the 360 value, never further. Skin weights are snapped to thousandths and
the last one recomputed as `1 - rest` in float32, which is how the 360 files
were built; about 80% of converted weights are then bit exact and the rest
within 1e-3. Index data and section tables are identical.

`NMR2` is a model's morph stream, which `sub_82116988` reads through
`sub_82117018` to blend one model instance into another (an enemy changing
form). For each `NSHP` of the model in order it holds one record per vertex,
then a u16 bone count, the u16 bone ids and a u16 pad when the count is
even. A 360 record is float3 normal, float3 position, DEC3N normal, float3
weights and UBYTE4 indices (44 bytes); the PS3 packs both normals and the
weights into CMP dwords (28 bytes) and is converted like `NSHP`. Read
unconverted, the morph objects get garbage bone lists and the battle faults
in `sub_821178D8` (`ep209.bop`, `lam01`).

Bit 15 of the flags word (+0x18) is set on every PS3 mesh and on most 360
ones; it selects the synchronous vertex buffer path in `sub_821147B0`, which
the 360 supports, so it is left alone.

### 3.3 Materials and lights

`NMTR` holds 96 byte materials; the colours are at +4, +36, +40 and +44, and
+8 is a flags word. When its bit 0 is set the material is textured and +4
holds the texture index (u16) instead of a colour, so it must not be rotated.
`NLIT` is 48 byte records from +8 with colours at +20 and +28, `NFOG` has one
at +12, `NCLC` at +20 and +36, and `NOL2` 32 byte records with the colour at
+24, or +8 for type 0 records.

`NATR` is map collision (`sub_821100D8`): a 32 byte header whose u16s at +16,
+20, +22 and +18 count 8 byte nodes, 12 byte triangles, 6 byte records and a
u16 list, stored in that order from +32. When the u16 at +24 (a scale, /1024)
is nonzero, a colour per 6 byte record follows, 4 aligned, ending the chunk.
Rotated, 610 of the 661 chunks both releases share match the 360 bytes; the
rest are collision or tints the PS3 changed.

### 3.4 Effects

A `Mefc` is a directory of sections, each found by offset from the `Mefc`
tag:

```
+0x00 "Mefc"  u32 size
+0x08 "XB"  ...  u16 directory offset at +0x0C (0x28 or 0x30)
dir   "CK"  u8 entry size (12)  u8 count, then per entry:
      char tag[4]  u8 type  u8 pad[3]  u32 offset
```

Tags seen: `TEXn` (`NTX3`), `CBLn` (`NOBJ`), `SE_0` (a
`CSF ` bank), `EFCT` (`etbl`), `TRCn`, `COMT`, `MLNn`, `ANMn`. The 360 aligns
sections to 0x1000, the PS3 to 128. Converting an embedded model or bank
changes its size, so the converter converts each section and rewrites the
directory and the `Mefc` size; converting in place without that leaves `SE_0`
pointing into the model, and the effect loader rejects every instance after
the first (map objects `80d`, `80e` on `tnt03`).

A model's `NLEF` (u32 count, u32 offsets from +8, then the `Mefc`s) is only
pointed at by `CreateModel` (model `+176`), so its effects are converted the
same way and the offsets rewritten. 564 `NLEF`s in 385 files hold 1325
effects, 205 of them with a model (`CBLn`), which the 360 refuses
unconverted (version `0x83`); none carries a bank.

## 4. Containers

Converting a skinned mesh grows its chunk, so offsets into the container move:

* `.e`: list B dwords in the image hold bulk relative offsets (to `NOBJ`,
  bare `NMDL`, `Mefc` and `BTX ` blobs; native 1062 takes all three model
  kinds) and are remapped; the header's total size (+0x0C) and reloc
  offset (+0x14) grow by the same delta. The image itself does not change.
* `.bmd`: the entry table after the 12 byte header holds absolute offsets.
* `.bop`: +0x0C points at a directory (`u32 count`, absolute offsets) of
  `NMTN`, `Mefc` and collision entries.

A resized `NMDL` ends with an `NPAD` that keeps the size change a multiple of
128, so everything after it keeps its PS3 alignment.

Some containers are addressed by slot from executable code, and the PS3
reordered them. In PS3 mode the host has to read them by PS3 slot rather
than reorder the files into the 360's layout.

`AppKeep.bmd`, the global model, texture and effect store: 412 entries on the
360, 374 on the PS3. `sub_82162058` loads it into the 58 MB `APPKEEP` heap
(`unk_82420948`) and copies its entry pointers into `dword_82420AFC` (room
for 512), which code reads at constant slots, through the item icon table
`word_8202C9C8` and native 1052 (`sub_820EA260`, slot + 1, from scripts).
Matching every entry by content (effects by their sections, textures by
their decoded image) gives the PS3 layout:

| 360 slots | PS3 source |
|---|---|
| 0..2 | `pcalg_v1.p3obj`, `pcplk_v1.p3obj`, `pcbet_v1.p3obj` |
| 3..9 | `appkeep2.bmd` 2..8 |
| 205..229, 283..289, 307..309, 346 | `campdata/camp_char.bmd` (portraits, skill icons, gems, three effects, a menu bar) |
| everything else | `AppKeep.bmd`, the 360 slot minus 1 up to 204, then plus 5, 7 or 9 |

The PS3 fills the 360's empty slots 93..100 with eight new effects, restyles
the save/load, clef, pad and controller icons in place, and inserts art for
CRS and SRN after each ten character block. 360 slots 164, 165, 194 and 245
(three Japanese labels and a frame) have no PS3 counterpart, and nothing in
the executable names them. `camp_char.bmd` (a `CAMP` header, `.bmd` layout)
also holds costume portraits and the CRS and SRN camp portraits, which the
360 has no slots for.

In PS3 mode `src/engine/ps3_appkeep.cpp` rebuilds the array in the 360's
numbering after the load, placing the characters and the camp entries in the
same heap (SRN spills into physical memory), and appends the 14 PS3 only
entries from slot 412. Natives 1052 and 1141 (kind 0, positive ids)
translate the PS3 slot scripts pass. The
achievements screen's texture patches address the PS3 file's ordinals. Asset
API ordinals (`appkeep.bmd#tex:N`) follow the file, so they differ per
release.

`btldata\BattleKeep.bop`: 106 entries on the 360, 98 on the PS3. The PS3
dropped the eight `Mefc`s at 360 slots 26..33 and every later entry moved
down by eight (360 slot s holds the same clips as PS3 slot s-8, matched by
clip length). `sub_821A0F18` copies the directory into a slot array at
`0x824FCE30` (its object plus `0x2C9B0`). Executable code reads constant
slots from it, and battle records name slots by negative id (`-n` is slot
`n-1`), yet the PS3's battle files carry the 360's ids unchanged. The PS3
parser (`sub_15ED60`) and record reader (`sub_15EF90`) copy and index the
array directly, so where the PS3 maps those ids is not found yet.

So the array has to be in the 360's layout. The converter appends eight null
slots after the PS3's entries (`append_empty_battlekeep`), and in PS3 mode
`src/engine/ps3_battlekeep.cpp` reorders the array after the parser runs:
360 slots below 26 are the file's, 26..33 the appended eight, 34 and up the
file's slot minus 8. Nothing reaches 26..33 in PS3 mode: the executable's
constant ids through the slot accessor `sub_821A5148` (1 based) are 35, 45,
49, 50, 52, 76..79 and 96..98, the hit effect table is translated around
them, and every reader (`sub_821CD0D8`, `sub_821CDCB0`, the accessor's
callers) skips a null slot.

`.tex` (`btl_exit_text.tex`, `levelup_JPN.tex`) is a 2D animation: magic
`03 33 90 10`, u16 entry count plus one, u16 0, then {u32 kind, u32 offset}
entries; kind 1 is a texture (`NTX3` on the PS3, `NTEX` on the 360), 2 a
0x140..0x1C0 byte record, 9 the last. The converter converts the textures
and lays the entries out 32 byte aligned as the 360 does, which reproduces
the 360 files apart from the PS3's own animation values and the redrawn
first texture of `levelup_JPN.tex`.

Enemy n (1 based) is the 80 byte record n - 1 of `off_82024100`: word 0
holds n and a kind, words 1..6 model name pointers, the u16 at +40 the
`btldata\enemy\ep%03d.bop` number (most enemies have no such file), the rest
plain values. The 360 has 281, the PS3 336 (`0x51052C`): its new enemies
are appended (`em27`, `bos11`, `bos13`, `_v4`/`_v5` variants, ...) and 25
shared records differ. An encounter on a PS3 only map reads past the 360
table and never starts (`ep771.bop`). PS3 mode points the three readers
(`sub_821A0628`, `sub_821BD480`, `sub_821BD778`) at a guest copy of the
PS3's (`src/engine/ps3_enemies.cpp`). The EBOOT is not redistributed, so
the build reads that table from `assets/EBOOT.elf` (`scripts/ps3_enemy_table.py`;
CI fetches it with `default.xex`); built without it, PS3 mode keeps the 360
table.

The effects a hit shows come from record n of `unk_82078B00`
(`sub_8218E480`; `sub_8218E558` reads its two trailing floats): 25 entries
of {u8 kind, u8 kind, u16 pad, s32 slot, s32 slot}, kind 0 an `AppKeep.bmd`
slot, kind 1 a `BattleKeep.bop` slot, -2 the current default. The 360 has
111 records, the PS3 119 (`0x410F00`), so a new enemy's hit read past the
table and indexed the `AppKeep` model table with a float. Pairing the 111
shared records gives the slot maps this table needs: `BattleKeep` 360 = PS3
+ 8 from slot 26, and fourteen `AppKeep` slots in PS3 65..92, each 360 =
PS3 + 1; translated, 105 records are the 360's byte for byte. PS3 mode
reads a guest copy in 360 numbering (`src/engine/ps3_hit_effects.cpp`),
generated from the EBOOT at build time like the enemy table
(`scripts/ps3_hit_effect_table.py`).

360 slot 38 (PS3 30) is the battle voice table (`BMD `), read by
`sub_821BCC40` (12 byte candidates: delay, clip, cumulative weight) and, for
categories 8..31, `sub_821BCEA0` (8 byte entries pointing at 32 byte
records). Rows are the character id minus 1, or the enemy id plus 9 on the
360 and plus 11 on the PS3 (`sub_1A6A98`), which has twelve character rows:
131 rows to the 360's 115. The PS3 has 61 categories to the 360's 59; matching every
caller (`sub_82196248` / `sub_144FD8`) shows categories up to 40 unchanged
and the 360's 41 and up asked for as 43 and up (`sub_821AB7E0`'s 41..44 are
`sub_175248`'s 43..46; the 8..31 lookup table is identical). The pre-battle
line (battle state 5, `sub_821BB310`) asks for category 35 and waits until
that voice ends, so a wrong table leaves the intro camera circling forever.
PS3 mode adds 2 to enemy rows (`sub_821ABC68`) and to categories from 41
(`sub_821BCC40`).

`title.bmd` holds six entries but its count says five: the title screen
picks one of three effects with a counter cycling 1..3 (`dword_8238EBD0`,
`TITLE_TASK__Init`), and the third is the uncounted sixth word. The converter
therefore remaps every offset between the count and the first entry.

The credits (`op.bmd`, `ed1.bmd`, `ed2.bmd`) are a `BMD ` header with ten
list offsets, then 16 byte records whose text pointers are file offsets.
Both executables read list 0, 1 or 2 by the language
(`dword_8243D36C`: Japanese, English, any other; `sub_82131E18`, PS3
`0x32287C`); the PS3 fills lists 3..9 too, and the 360 leaves them empty.
The PS3 files ship as they are.

`campdata/scp.bmd` holds the score pieces: `"SCP "`, total size, offset of
a `CSL ` directory of 68 banks, the texture count (64), then one 1120x128
DXT5 per piece. Both releases order the pieces alike (the PS3 restyled piece
51). The camp menu reads it whole into a 0x12A0000 byte buffer
(`sub_8222BDE8`), which the PS3 file overflows with its pitched textures, so
the converter lays it out as the 360 does: textures 16 byte aligned, banks
from the next 0x1000.

## 5. Audio

No XMA encoder exists, so every clip and new track ships as a sidecar,
`pcm/<16 hex digits>.wav` in the game directory. The converter writes a 16
byte tag (`RXPcmSub` plus those eight bytes) at the start of the clip's
payload, and the host (`ScanGamePcm`) substitutes the sidecar when the XMA
decoder meets it, the same way mod audio works. Sidecars keep the PS3
encoding: WAV format 0x0270 (ATRAC3, decoded through the SDK's FFmpeg) or
the private 0x5053 (PS-ADPCM), decoded by `LoadPcmWav` when the clip first
plays, with an optional `smpl` loop in samples. About 1 GB on disk, where PCM
would take about 3.5 GB.

### Sound banks (`.csf`)

Same container as the 360: `CSF ` (total, header size, payload size), `BOOK`
(the `SONG` sequences, byte identical in layout), then `PGHD` holding `PROG`s
of `TIM` clips. `TIM` fields from +0x08, all u32: flags, rate, payload offset,
size, loop start, loop end, then (offset, length) of the `LIP ` chunk, the XMA2
header and the seek table, each relative to the `TIM`. The PS3 leaves the XMA
pair empty and packs chunks unaligned; the 360 aligns `TIM`s to 4, clip
payloads to 0x1000 and the header to 0x1000.

* PS3 payloads are mono ATRAC3, 192 byte frames of 1024 samples (`a2 00 ...`).
* Flags: PS3 1 (looping effect) is 360 0xFF, 0 and 0x100 (voice) are equal.
  The first parameter word at +0x38 has 6 in its low half on every PS3 clip
  and 0 on the 360.
* The 360 rate field is tuned per looping clip (47968..48019); PS3 says 48000.
* XMA2 header, nine u32: `0x030100FF` looping / `0x03010000`, loop begin
  (384) and end, rate, block size (0x4000 effects, 0x1000 voices), samples,
  samples without padding, block count, `0x01000001`. The seek table is one
  cumulative sample count per block.

The PS3 appended its new clips: `pc001` keeps all of the 360's 173 in place and
adds 29. Each clip becomes a 360 `TIM` with the PS3 clip's parameters and
`LIP `, and one 0x800 byte tagged packet of payload described as the clip's
length. The `BOOK`s differ in a few sequencer timing bytes and the PS3's are
kept.

### Banks inside `.e` files

Events carry their voices as `CSF ` banks in the `.e` bulk (293 PS3 files,
8807 banks, 431 MB of ATRAC3). They come in groups, each led by a `CSL `
directory: `"CSL "`, u32 count, then u32 offsets from the directory to its
banks, which follow back to back. The script image references the
directories, not the banks. On the 360 directories and banks start on 0x1000
boundaries of the file; on the PS3 they are packed.

A voiced event has one directory per voice language, in the opposite order
on the two releases (`t0001.e`: two of 24 one line banks).

The converter converts each bank as a standalone one. Directories and banks
are realigned to 0x1000, and the directory offsets are rewritten
(`convert_region`).

Battle `.bop` files (an `SE_0` bank in each attack's `Mefc`) and `title.bmd`
carry banks the same way. One ATRAC3 clip left anywhere reaches the XMA decoder, which stalls,
and the game plays no sound again until it restarts.

### Music (`.cps`)

`CPS ` header: u32 header size (0x20), channels, data size, rate, loop start
and end in bytes, and a kind: 3 is PS-ADPCM with channels interleaved per 16
byte frame, 0 is big endian PCM. Every track both releases share has the same
sample count, so only names differ. PS3 scripts ask for `MP139.cps`, and
`sub_820F80F8` builds `sound\cxs\<name>` from whatever the script says. PS3
mode turns `.cps` into `.cxs` where the stream starts (`sub_82142070`,
`sub_82142360`, path in `r4`).

The 66 PS-ADPCM tracks become a `.cxs` written from scratch plus an ADPCM
sidecar. Every 360 `.cxs` has the same header, all u32 big endian: `"CXS "`,
header size 0x800, rate, channels, samples, loop start and end in samples
(end rounded up to 512, both 0 when the track does not loop), payload size in
0x10000 byte blocks rounded up, payload offset 0x1000, payload size, then
0x800 twice. From 0x800, one u32 per block: the samples played by its end,
in whole XMA frames; the last is the sample count rounded up to 512, plus
one frame. The guest does not start a track whose table is empty, nor a
looping one whose loop end is not below the last entry. The guest streams the
payload while the track plays, so it gets a quarter byte per sample and
channel (the 360's XMA takes 0.11 to 0.28), zero but for the tag in its first
packet. The 20 kind 0 tracks (`MP180..199`, the 5.1 versions among them) are
PCM already and become plain big endian `.wav`, which scripts ask for by
that name, as the 360 ships them.

## 6. Not done yet

* The PS3 script VM: natives compared one by one (§2).
* Twelve party slots.
* Last, since the game plays without them: the PS3 only camp screens, the
  costumes page ([costumes.md](costumes.md)) and Scrapbook.
