# Script VM and text system

Companion to `docs/asset-formats.md`. That document describes the formats;
this one records where things live in `default.xex`: the `.e` loader, the
bytecode VM (§7, §8), the text system (§3), the battle-side state machines
that consume `.e` data (§2, §5), and leads that are known to be dead (§4).
`scripts/e_disasm.py` disassembles any decoded `.e`.

Target binary: `assets/default.xex` (IDA database `assets/default.xex.i64`).
Big-endian PowerPC, Xbox 360. Image base `0x82000000`;
`.text` `0x820C0000`–`0x822E7CB4` (564,663 instructions);
`.data` `0x822F0000`–`0x82566B3C`.

---

## 1. Function inventory

### Asset index and codec

| Address | Role |
|---|---|
| `sub_8210D080` | `index.vmtoc` binary search; lowercases, compares byte-by-byte, `48 * mid + base` |
| `sub_8210D168` | Startup: loads `game:\index.vmtoc`, sets `dword_8244061C` (base), `dword_82440820` (count) |
| `sub_8210D470` | Spawns two worker threads: `sub_8210D8E8` and `sub_8210DC28` (the decoder thread) |
| `sub_8210DFA8` | Codec init: 256-byte frequency table, cumulative freqs (u16 at ctx+302, total at ctx+814), symbol LUT at ctx+816, primes the 4-byte code word; if flag bit 0, zeroes the 4096-byte ring at ctx+9020 and sets ring pos 4078 |
| `sub_8210E0F8` | Range-coder symbol decode; `low`/`range`/`code` at ctx+9008/9012/9016 |
| `sub_8210E260` | LZSS layer |

The decoder runs on the async-I/O worker thread, not on the thread that
requested the file.

### `.e` container loading

| Address | Role |
|---|---|
| `sub_820FF2B8` | Entry; validates magic `(*a2 & 0xFFFFFFFE) != 0x180` → reject |
| `sub_820FF9C8` | The loader: allocates, memcpy's the image, runs relocations, copies `block2` |
| `sub_820FF6C0` | Relocator; patches a 32-bit word per list entry |
| `sub_820FF748`, `sub_820FF838`, `sub_820FF910` ×2 | `block2` fixup passes; `sub_820FF748` binds native ids to function pointers (§8) |

`sub_820FF6C0` stores each relocated dword byte-reversed, so relocated
pointers are little-endian in the stream while everything around them is
big-endian. The image allocation starts at `file[0]`, so relocation list
entries are raw file offsets.

### File readers

| Address | Role |
|---|---|
| `sub_821BBED8` | Generic async read request `(pool, …, len, type)` (§5.1) |
| `sub_8210C9D8` | Opens the file, handles the `"host://"` path prefix |
| `sub_8210CD20` | Allocates the read buffer (length rounded to 2048-byte sectors) |
| `sub_8210CC68` | Issues the async read |
| `sub_8210CBB8` | Generic handle cleanup/release (104 call sites across ~90 unrelated functions) |
| `sub_8219F698` | Battle-AI script loader: 4 calls, `type=9`, `len=0x1000`, loads `btldata\script\ai\<name>.e`, falling back to `default.e` |

The tutorial load site is case 11 of `sub_821ACBF8`'s dispatch
(`0x821ADA9C`–`0x821ADB60`), `type=9`, `len=0x2000`, filename from the
`{u32 size; char* name}` table at `off_8238E330`. The read lengths are the
`.e` image size for those files.

`sub_821BBED8`'s pool argument differs per caller: the tutorial passes a
per-unit pool at `unit + 0x822EC`, the AI loader passes the global
`&unk_8255272C`. The 8-slot / 344-byte-stride array is a pattern
instantiated in several places, not one global structure.

### Text system

| Address | Role |
|---|---|
| `sub_821D3890` | `SetText(mgr, window_id, text, tag)` on the global text manager `dword_82555690` |
| `sub_821D50A8` | Markup preprocessor (§3) |
| `sub_821D49A0` | Per-record render tick: latches the advance button, lazily preprocesses, then calls the layout pass |
| `sub_821D5CC0` | Layout pass; acts on the control codes (§3) |
| `sub_821D1898` | Parses a decimal argument, returns value + length via out-param |
| `sub_821D1908` | Resolves a `<cNAME>` name to an id |
| `sub_821D1E18` | Called from the `<Q…>` path when `a1[7850] == -1` |
| `GOTHIC_FONT` `0x82082850`, `MESSAGE_FONT` `0x82082B68` | String constants; xref these to reach the renderer |

### Other parsers

| Address | Role |
|---|---|
| `sub_82162058` | `.bmd` loader: `"BMD "` magic, count at +8 (≤512), relative offsets at +12 |
| `sub_821A0C30` | `.bop` dispatch: 5 slots, state machine 0→1→2→3 |
| `sub_821A0E40` | Map BOP parser: `"BOP "` magic at `a1[177]` |
| `sub_821A0178` | BattleKeep BOP loader: `btldata\BattleKeep.bop`, tags buffer `0xFFFF9933` |
| `sub_8219FF58` | Map BOP loader: `btldata\map\<name>`, tags buffer `0xFFFF0000` |

---

## 1.5 `sub_821C9FE0` and `sub_821C0300` are FSMs, not the interpreter

The bytecode VM is `sub_820FFE28` (§7, §8). `sub_821C9FE0` (67 states) and
`sub_821C0300` (97 states) are hardcoded per-object state machines that read
`.e` *data* (text, resource ids, relocated pointers); they do not run image
control flow. Both share one skeleton:

```
lwz  r11, 4(r31)          ; state = *(a1+4), 1-based
addi r11, r11, -1
cmplwi cr6, r11, N        ; 0x42 for sub_821C9FE0, 0x60 for sub_821C0300
bgt  cr6, <default>
lhzx r0, <jump table>, index*2
mtctr / bctr
```

Both are slots in the per-object-type tick table starting at `0x82087108`.
`sub_821C9FE0` is the per-party-member narration-line object (§5.3);
`sub_821C0300` works on the per-battle-unit struct family: its guard
computes `base + 16136 * index`, the same stride at which `sub_8219F698`
stores its async-read handles (`+16012`/`+16136`/`+32148`/`+32450`).

---

## 2. `sub_821C9FE0`: the 67-state narration-line FSM

`0x821C9FE0`, 0x24E0 bytes. One state step of a per-party-member narration
object, reached through vtable slot `+168` of `off_82087108`. The
`0x820B1030` xref is `.pdata`, not a call site.

Hex-Rays produces a 3-line stub ending in `__asm { bctr }`; use raw `disasm`
at each case address.

Dispatch (`0x821C9FE0`–`0x821CA078`): state dword at `a1+4`, 1-based
(1..0x43), jump table `word_820824A0`, base `loc_821CA078`:

```
01 0x821ca078   0f 0x821ca67c   1d 0x821ca4d4^  2b 0x821cbc90   39 0x821cc1c0
02 0x821ca140   10 0x821cad68   1e 0x821cb6d4   2c 0x821cbc84   3a 0x821cc2cc
03 0x821ca244   11 0x821caeec   1f 0x821ca67c   2d 0x821cc4b0*  3b 0x821cc390
04 0x821ca288   12 0x821caad0   20 0x821cb848   2e 0x821cbd80   3c 0x821cc450
05 0x821ca314   13 0x821cace0   21 0x821cb938   2f 0x821cbdb8   3d 0x821cc30c
06 0x821ca088   14 0x821cb4d8   22 0x821cb994   30 0x821ca43c   3e 0x821cc364
07 0x821ca130   15 0x821cb544   23 0x821ca4d4^  31 0x821ca4d4^  3f 0x821caf64
08 0x821ca348   16 0x821cb554   24 0x821cba08   32 0x821cbdf0   40 0x821cb120
09 0x821ca430   17 0x821cb590   25 0x821cba38   33 0x821cbef4   41 0x821cb2b8
0a 0x821ca50c   18 0x821cb5a0   26 0x821cc4b0*  34 0x821cbfe0   42 0x821ca868
0b 0x821ca67c   19 0x821cb5d0   27 0x821cbb40   35 0x821cc0a0   43 0x821cad0c
0c 0x821ca6b8   1a 0x821cb614   28 0x821ca4d4^  36 0x821cc19c
0d 0x821ca67c   1b 0x821cb6a0   29 0x821cbba0   37 0x821cc4b0*
0e 0x821ca790   1c 0x821cb67c   2a 0x821cbc84   38 0x821cc47c
```

`*` = `loc_821CC4B0`, the out-of-range/default exit, so states `0x26 0x2d
0x37` are no-ops. `^` = `0x821CA4D4`, shared by `0x1d 0x23 0x28 0x31`.
States `0x0b 0x0d 0x0f 0x1f` share `0x821CA67C`; `0x2a 0x2c` share
`0x821CBC84`.

Each call runs exactly one case and returns: a case either writes its
successor into `a1+4` (e.g. case 01 stores the literal 7) or leaves it
unchanged to park until a readiness condition clears (case 02 polls bytes at
`r30+0x83238`/`0x83239` and `sub_8218D408`; case 03 polls `sub_821C9090`,
which computes a distance from the position at `a1+68`/`a1+72` and feeds it
to the resource manager `dword_824CF500` through a handle at `object+81688`,
i.e. positional audio for the speaking unit). `loc_821CC4AC` is the
epilogue: `stw r11, 4(r31)`, stack teardown, `b __restgprlr_23`. Case 04
(`0x821ca288`) calls the object's own vtable slot `+0x8C`.

`sub_821BA4B0`, called early in cases 01/06, is a reset helper (zeroes two
fields across a small pointer array), not a wait condition.

The AI script loader `sub_8219F698` is called from `sub_821ACBF8` at
`0x821acff0` inside a loop over every party unit (bound = byte at
`[unit_array+0x2A1]`, stride `0x7EC8`), once per unit at battle start. The
narration objects are not created from that read; their pool is keyed off
party composition (§5.2, §5.3).

---

## 3. `sub_821D50A8`: the markup preprocessor

`0x821D50A8`, 0xC18 bytes. Expands `<...>` tags in a raw text entry into
single-byte control codes in a scratch buffer at `a1 + 19104`, stores each tag's
numeric argument at `a2 + 8*(argidx + 65)` (counter at `a2 + 840`), then copies
the scratch buffer to `(a2+8 | a1+35122) + *(u16*)(a2+420)`. The destination
selector is `*(u32*)a2 == a1[9037]`.

It is not a "message started" signal: `sub_821D49A0` calls it lazily for any
record whose formatted flag at `+518` is clear, and `sub_821D4598` /
`sub_821D4630` call it just to measure a string. `SetText` (`sub_821D3890`)
fires exactly once per "this window now shows this string".

Constants held in registers: `r25=0 r23=1 r20=2 r15=5 r22=-1 r21=0x18 r14=0xA
r17=0xC r18=0xB`.

The tag → control-code table is in `docs/asset-formats.md` §3.5.

Characters `'n'`..`'z'` dispatch through a 13-entry `bctr` jump table at
`word_820821B8`, base `loc_821D5740` (Hex-Rays emits `__asm { bctr }`):

```
n 0x821d5b10   o 0x821d5b8c (default)   p 0x821d58d4   q 0x821d5a34
r 0x821d59ac   s 0x821d5abc             t 0x821d58dc   u 0x821d5a2c
v 0x821d5740   w 0x821d5804             x 0x821d5950   y 0x821d5988
z 0x821d5b38
```

The `w` case at `0x821D5804`: `0x821D5810` tests for `>` (bare `<w>` → code 2),
`0x821D5844` tests for `v` (`<wv>` → code 13), otherwise it parses a number
(`<wNNNN>` → code 1). All three advance the arg index by exactly 1, and the
bare `<w>` path never reads its argument slot.

Usage counts across all `.e` files: `<w>` 29473, `<wNNNN>` 26829, `<wv>` 568.

### Control-code consumer: `sub_821D5CC0`

The layout pass `sub_821D5CC0(mgr, record)` acts on the control codes through
the 25-entry jump table `word_82082230` (base `loc_821D600C`, index =
code − 1): code 1 at `0x821D6370`, code 2 at `0x821D63C4`, code 13 at
`0x821D69AC`. Per-code state is at `record + 0x20C + 8*argidx` (reached) and
`+0x20D` (released); the record's state dword at `+4` is 6 during a `<w>` wait
and 8 during a `<wv>` wait.

* `<w>` releases on `byte_8255D125` (`mgr+31381`), the advance-button edge
  that `sub_821D49A0` latches from the pad word `dword_824BB5C4[116*slot]`
  before calling the layout pass.
* `<wv>` polls `sub_821431C0(dword_8243D89C, *(mgr+31372))` ("is this voice
  handle still playing") and advances when it returns 0. The game's own
  stop-the-clip call is `sub_82142EE8(sndmgr, handle, fade)` (used by
  `sub_821D96F8`).

`src/engine/eternalsonata_hooks.cpp` uses these to let the advance button cut
a `<wv>` wait short.

---

## 4. Ruled out

Do not re-investigate these.

**Codec / decode path**

- Standard DEFLATE: zlib rejects every offset.
- The 255-byte header as Huffman code lengths: it is a raw symbol frequency
  table (`Σ 2^-len ≈ 1.469` fails Kraft).
- Ciphers/S-boxes: the files are compressed, not obfuscated.

**Tutorial call graph** (`sub_821ACBF8`, a 1541-instruction per-unit tick state
machine with its own 23-case computed-goto at `0x82082730`, base `0x821ACEDC`)

- `sub_82101750` → `sub_820FF2B8` → `sub_820FF9C8` → `sub_820FF6C0` is not
  reached for the tutorial's type-9 load: `sub_82101750` receives the buffer
  pointer and the magic check reads packed bytes. The chain is live for
  decoded buffers.
- Slot-struct `+0x4` (`unit+0x8323C`), where case 11 stores the read handle,
  is never read anywhere in the binary.
- `sub_821AA500`: generic 2-way vtable event dispatcher, resolvable only at
  runtime.
- `sub_82197C68`: positional-audio/listener-list updater.
- `sub_821AA6D8`: table lookup keyed on a small discriminant; the `0x23`/`0x21`
  it compares against are integers, not ASCII `#`/`!`.
- `sub_821AB1A0`, `sub_821AA730`: combat AI turn/action selection.
- `sub_8218CB70`, `sub_821AE410`, `sub_821B7D78`, `sub_821B98D8`: read only
  struct `+0x0`/`+0x2` as gating bytes.
- `sub_821BA2B0`, `sub_821BB7B0`: false-positive `0x3238` immediates.
- `sub_8210CBB8`: generic handle cleanup, 104 call sites.
- The `+0xAC` vtable-slot call pattern: generic per-tick listener dispatch.

**Text sources**

- Dialogue is not plaintext anywhere in the xex or `assets/` (ASCII or
  UTF-16LE). The plaintext Italian strings in `.data` (`0x823606A9`,
  `0x82361249`, `0x820560B3`, `0x822F750A`) are item and location text
  reached by `base+index*stride+offset` addressing.
- Voice `.csf` files are compressed audio, not text.

**Event bus.** `dword_8243D89C` is the sound/message-hub singleton; its
`+22416` ring is posted to by ~100 small wrapper functions in
`0x8213F000`–`0x82143600` through `sub_8213FB80`. `sub_821422B0` is one such
wrapper (message 14), fired by `sub_821ACBF8` around `0x821ae0a0` when a
script read completes; it is not an object constructor.

---

## 5. IDA tooling caveats

- The xref database is incomplete for this binary: addresses built from
  `lis`/`ori` pairs frequently produce zero xrefs. `find_bytes` on the raw
  big-endian pointer value catches what `xrefs_to` misses, and searching on
  a named symbol (e.g. `unk_824FD1A0`) finds references that a folded
  `base+offset` does not.
- `insn_query`'s `op_any` is not exhaustive: it misses values that appear
  only as the displacement in `disp(reg)` operands and `lis`-immediate halves.
  Never treat a zero-match result as a confirmed negative.
- Hex-Rays fails on every computed-goto in this binary (`sub_821ACBF8`,
  `sub_821C9FE0`, `sub_821D50A8`, `sub_821D5CC0`), emitting a stub ending in
  `__asm { bctr }`. Use raw `disasm` at the specific case address.
- `disasm` with `offset: 0` on large functions has occasionally returned only
  10 lines regardless of `max_instructions`. If it happens, use `offset: 1`.
- IDA does not mark every vtable slot as `.long`; read vtables with
  `get_bytes`, not `disasm`.
- Generic helpers (`sub_8210CBB8`, `sub_8218D408`, `sub_821BBED8`) have many
  callers; always state which call site and arguments are meant.
- Reopen with `idb_open` on `assets/default.xex`. If the lock cannot be
  acquired, kill the stale worker via its `pid` from `idb_list`.

### 5.1 Async `.e` read job queue

`sub_821BBED8(a1, a2, a3, a4)` is the generic async-read job submitter:

- `a1` is a job-manager struct (e.g. `&unk_8255272C`) holding up to 8 job
  slots, stride 344 bytes (`v9 = &a1[86 * slot]`), scanned linearly for the
  first slot whose dword at `+4` is 0.
- `sub_8210C9D8(slot+16, filename)` kicks off the read; the slot's dwords at
  `+340`/`+344` are set to `a3` (job type, 9 at every call site seen) and `a4`
  (buffer size). Returns the slot index as the handle, or -1 if all 8 slots
  are busy.
- Callers: `sub_8219F698` (AI script loader, handles stored at
  unit+16012/+32148) and `sub_821ACBF8` (tutorial tick FSM, `0x821adb0c`,
  four call sites for primary/fallback AI + secondary/fallback AI scripts).
- Completion check: `sub_821ACBF8` (around `0x821ae0a0`) reads
  `*((_DWORD *)&unk_82552738 + 86 * handle)`, i.e. slot field `+12` of the
  same array. `handle` comes from a per-object byte at `v1+537148`, gated by
  flag bytes at `v1+537144`/`v1+537145`. When set it calls
  `sub_821422B0(dword_8243D89C, 20, 0.0)`, the voice-line trigger (§4).

The job queue and the narration-object pool (§5.2) are independent: the pool
is keyed off party composition, and the `sub_821BBED8` handles never reach it.

### 5.2 Narration-object pool

- Constructor `sub_821A6DF0(a1)`: `*(_DWORD *)a1 = &off_82087108` (vtable),
  `*(_DWORD *)(a1+4) = 1` (initial state), plus ~30 POD field inits.
- Spawner `sub_821A9F68(a1)`, the constructor's only caller. `a1` is the
  battle manager `dword_824D0440` (the same object `sub_821ACBF8` ticks).
  It walks 8 slots at `a1+533044` (a second bank of 8 at `a1+533056`, gated
  by `*(BYTE*)(a1+737)`); for each empty slot whose party record (§5.3) holds
  a character id in `[1,10]` it allocates 124 bytes with `sub_820C0000(124)`,
  constructs, then calls `vtable+28(obj, &record, slot)` (`sub_821A6DD8`:
  `obj+0x20 = &record`, `obj+0x10 = slot`, `obj+0x0C = 0`) and
  `vtable+4(obj, 8)`.
- Vtable `off_82087108` (raw dwords at `0x82087108`):
  - `+0` = `sub_821C9E78`
  - `+168` (`0x820871B0`) = `sub_821C9FE0`, the state dispatcher (§2)
  - `+172` (`0x820871B4`) = `sub_821CCE70`, the per-frame tick, called from
    `sub_821ACBF8`'s per-line loop (`(*(*v21)+172)(*v21)`) once per active
    slot. It decompiles cleanly and calls `(*a1+168)(a1)` as one sub-step,
    alongside `vtable+164`, `vtable+0` and helpers such as `sub_821966D0`,
    `sub_821C8E88`, `sub_821C6450`.
  - `+0`, `+4`, `+164` are not identified. `sub_821C0300`'s owning vtable
    has not been located.

### 5.3 Battle record arrays

The battle manager `dword_824D0440` holds the party and enemy record arrays;
IDA names the party base `unk_824FD1A0` = `0x824D0440 + 0x2CD60`.

| thing | base | stride | count |
|---|---|---|---|
| party members | `unk_824FD1A0` (mgr+183648) | 81972 | `byte_824D0720` |
| enemies | `unk_82539240` (mgr+461840) | 32456 | `byte_824D0721` |

`record[0]` is the character id, 1..10. `record[81698]` is an `i16`
model/kind id. `unk_82510FE8` (mgr+265128) is the `{cur, max}` HP pair in
the same records; `sub_821BAB70` computes `cur/max < 0.2` from it to pick a
low-health chatter line. Functions reading the id as an enum: `sub_821AB7E0`
(`== 7`, `== 8`), `sub_821ABD78` (`== 3`), `sub_821BAB70` (searches for ids
1/2/3/6/7/8).

Every narration-object consumer (`sub_821C8B58`, `sub_821ABC68`,
`sub_821ABE88`) reads the slot back out of `obj+0x10` / `obj+0x20`.
`sub_821ABC68(mgr, desc)` converts a `{kind, slot}` descriptor into a BTX
string id (`party.character_id - 1` for kind 0, `enemy.enemy_id + 9` for
kind 1) and `sub_821ABE88` feeds that to `sub_8223B780("BTX ", id)`, the
bulk-section text lookup (`docs/asset-formats.md` §3.4).

---

## 6. Message example

A tutorial line from `extracted/e/btldata/script/tutorial/t0001.e` at offset
`0x40A21C`:

```
<y6><l6><m1><v1>Gli animali che vivono nella foresta qui intorno, non sono\n
molto forti, così dovrei riuscire a sconfiggerli senza troppi\n
problemi. Tuttavia solo per prudenza, ripasserò ancora le\n
basi del combattimento.<wv>
```

`<v1>` starts voice clip 1 and `<wv>` holds the message until it ends (§3).

---

## 7. The script bytecode VM: `sub_820FFE28`

Lives in the `.e` loader cluster at `0x820FF000`–`0x82101FFF`, next to
`sub_820FF9C8`.

### The loop

```c
int sub_820FFE28(_DWORD *a1)          // a1 = VM context
{
  ...
  do {
    v4 = (unsigned __int8 *)a1[8];    // instruction pointer
    v5 = *v4;                         // fetch one opcode BYTE
    a1[8] = v4 + 1;                   // advance
    if ( v5 <= 0x89 )
      __asm { bctr }                  // computed-goto dispatch, 0x00..0x89
    v6 = a1[2];
  } while ( v6 == 1 );                // 1 = running
  ...
}
```

The opcode is a raw byte from the stream, 0x00..0x89 (the opcodes
`0x07 0x0a 0x0b 0x0c 0x0e 0x7a 0x89` take pointer operands, matching the
relocation statistics in `docs/asset-formats.md` §3.3).

### Context layout

The VM context is at `owner + 48`. Full field list in §8; the ones the loop
above uses: `ctx+8` run state (0 finished, 1 running, 2 sleeping with the
countdown at `ctx+12`, 3 done), `ctx+0x20` ip. `sub_820FFCA0` (the init) sets
the ip:

```c
*(_DWORD *)(a1 + 32) = *(_DWORD *)(sub_820FEED0(*(_DWORD *)a1) + 8) + 24;
```

`sub_820FEED0(handle)` returns the loaded-`.e` record; its `+8` is the base of
the image allocation, and `+24` is `0x18`: the VM executes the `.e` image
starting at file offset 0x18. One call to `sub_820FFE28` runs until the
script yields, not one instruction.

### Call path

```
sub_821ACBF8   tutorial FSM
  sub_82101D70   allocate a 104-byte script object (vtable off_82082D08)
    sub_821014E8   sub_820FFCA0 (init IP/stack) then sub_820FFE28 (run)
```

Map scripts (`cfdata/*.e`) run the same VM; their tasks are spawned from
inside the script through builtin 7 (§8).

### Dispatch

`word_82081F40` is a 138-entry `u16` table; `handler = 0x820FFEDC + table[op]`,
giving opcodes `0x00..0x89` with 136 unique handlers (`0x03`/`0x07` and
`0x38`/`0x39` are aliases). The handlers are inline blocks of `sub_820FFE28`,
so Hex-Rays emits `__asm { bctr }`; read them with `disasm` on the whole
function. The decoded table is in §8.

```asm
0x820FFE9C  lwz  r11, 0x20(r31)     ; ip = ctx+32
0x820FFEA0  lbz  r10, 0(r11)        ; op = *ip
0x820FFEA4  addi r11, r11, 1        ; ip++
0x820FFEA8  cmplwi cr6, r10, 0x89
0x820FFEB0  bgt  loc_82101304       ; op > 0x89 -> skip, keep looping
0x820FFEC0  lhzx r0, word_82081F40, op*2
0x820FFED8  bctr                    ; handler = 0x820FFEDC + table[op]
```

### Resizing a `.e`

List B relocation entries include bulk offsets into the region after the BTX
blob (the debug string pool). Growing the blob shifts that data while the raw
dwords embedded in the image keep their old values, so after relocation the
VM reads shifted data and eventually jumps through a garbage handler pointer.
`scripts/btx_edit.py:fix_relocations()` parses list B, finds every entry whose
raw dword points past the old BTX end, and adjusts it by the size delta; it
runs on every non-`--preserve-size` edit.

A VM crash does not flush `logs/`; catch it under lldb:

```bash
lldb -b -s cmds.lldb -- ./eternalsonata.exe --game_data_root assets --gpu_plugin=xenos
# cmds.lldb:
#   settings set interpreter.stop-command-source-on-error false
#   run
#   thread backtrace all
#   register read ...
#   quit
```

## 8. The VM decoded: accumulator machine and natives

`scripts/e_disasm.py` implements this table.

### Execution model

`sub_820FFE28` is a single-accumulator machine. Context fields (`ctx = a1`):

| field | meaning |
|---|---|
| `ctx+0` | script/module handle; `ctx+4` slot id in the `unk_8241006C` handle table |
| `ctx+8` | run state: 0 finished, 1 running, 2 sleeping (`ctx+0xC` = frames left), 3 done |
| `ctx+0x18` | acc, 8 bytes: u32/int/f32 in the low word, f64 as a whole |
| `ctx+0x20` | ip |
| `ctx+0x24` | sp (grows down; `81` pushes 4 bytes, `82` 8) |
| `ctx+0x28` | fp; locals are `fp + s8/s32`, arguments sit at `fp+8, fp+12, ...` |

A call (`7a ptr`) pushes the return ip and the old fp and sets `fp = sp`;
`7c` returns. Native calls (`7d ptr`) pass r3 = sp, so the native sees its
arguments as `a1[0], a1[1], ...` in reverse push order (last pushed is
`a1[0]`), and the return value lands in acc. `7e` sleeps acc frames
(`ctx+8 = 2`, `ctx+0xC = acc`): a script that does `... 01 01 7e` yields
every frame, which is how per-frame loops are written.

`dword_824405FC` holds the ctx of the VM currently running (saved/restored
around `sub_820FFE28`), so a native can find its caller's ip.

### Opcode table

Operand widths in bytes; `L[x]` = `*(fp + x)`, `ptr` = 4-byte relocated
image pointer (`docs/asset-formats.md` §3.3), `pop` = value popped from the
stack.

```
00 halt                       01 acc=u8        02 acc=u16        03/07 acc=u32 (07 = pointer)
04 acc=f64                    05 acc=-u8       06 acc=-u16
08 acc=&L[s32]                09 acc=&L[s8]
0a..0f acc=*ptr as u8,u16,u32,f64,s8,s16
10..15 acc=L[s32] as u8,u16,u32,f64,s8,s16
16..1b acc=L[s8]  as u8,u16,u32,f64,s8,s16
1c..21 acc=*acc   as u8,u16,u32,f64,s8,s16
22..25 *pop=acc   as u8,u16,u32,f64       26 memcpy(pop,acc,u32)   27 memcpy(pop,acc,u8)
28..2f conversions int->f32,int->f64,uint->f32,uint->f64,f32->int,f32->f64,f64->int,f64->f32
30 acc=!acc      31 acc=(f64acc==0)
32/33/34 acc=pop+acc  (int/f32/f64)       35/36/37 pop-acc      38,39/3a/3b pop*acc
3c udiv  3d sdiv  3e f32 div  3f f64 div  40 umod  41 smod
42 srl   43 sra   44 shl   45 and   46 xor   47 or   48 neg   49 fneg   4a dneg   4b not
4c *u8acc&=/|=imm   4d..54 *acc += imm (u8,u16,u32,f32,f64,s8,s16; 54 = u32 += s32), acc = new
55..5c same, acc = old value
5d/5e/5f ==   60/61/62 !=   63/64/65 <   66/67/68 <=   6b/6c/6d >   6e/6f/70 >=  (int/f32/f64)
69 u<  6a u<=  71 u>  72 u>=  73 acc=(acc==0)
74 jmp s32   75 jz s32   76 jnz s32   77 jmp s8   78 jz s8   79 jnz s8   (relative to operand start)
7a call ptr  7b call table[u32]  7c ret  7d native ptr  7e sleep acc frames
7f frame u32 (sp -= n, zeroed)  80 frame u8   81 push acc   82 push f64 acc
83 push struct u32 from *acc    84 push struct u8   85 pop4  86 pop8  87 pop u32  88 pop u8
89 switch ptr -> {count, {value, target}[count]}
```

### Native binding

`7d` operands are patched at load by `sub_820FF748` from block2 table 1
(`{native_id, patch_offset}`, `docs/asset-formats.md` §3.2). Ids resolve
against the tables registered with `sub_820FF028(table, count, base_id)`:

| table | ids | registered by | contents |
|---|---|---|---|
| `off_8240C628` | 1..30 | `sub_821030C8` | VM builtins: 7 = spawn task (fn, args, prio...), 9 = kill task, 24 = array push |
| `off_8240C6A0` | 100..115 | `sub_821030C8` | |
| `off_8240C6E0` | 500..548 | `sub_820F91A8` | |
| `off_8240C828` | 1000..1151 | `sub_820F91A8` | field/object natives (`sub_820E8710`..`sub_820ED4F8`) |
| `off_8240C7B8` | 2000..2027 | `sub_820F91A8` | |
| `off_8240CA88` | 5000..5025 | `sub_820F91A8` | party lookups (5019 = `sub_820E7DE8`) |
| `off_8238E720` | 40000..40070 | `sub_821A82A8` (battle manager init) | battle natives |
| `off_8240CAF0` | 45000..45040 | `sub_821A82A8` | battle natives; 45002 = `sub_820E6B38`, posts a named signal (see "Battle signals") |

The 500-series table is not functions: `off_8240C6E0` holds pointers into
the map/event state block at `dword_8243C230`, so `acc=ptr symN` in a script
is a direct guest global (502 `dword_8243C230`, 505 `dword_8243C23C` map
script, 506 `dword_8243C240` running event task, 537 `byte_8243C345` skip
requested, 540 `dword_8243C34C` skip handler, 541 `dword_8243C350` skip task).

Event (cutscene) natives: 1010 `sub_820E8D10` (set active camera object
`dword_8244BEA8` via `sub_820FD7B8(blend_time, blend_param, map, object)`;
a nonzero blend time parks the target in `dword_8244BEAC` and interpolates the
old render camera towards it with `sub_82108D28`; the gate `byte_8244BA93` /
`dword_8244BAA4` is map+1507 / map+1524, i.e. "field camera only", and
map+1524 is the field camera object itself; the native always returns 0, so
scripts never wait on a cut), 1011 `sub_820E8968` (start event:
`dword_8243C240 = handle`), 1012 `sub_820E8A10` (end event via
`sub_820FC9F8`), 1013 `sub_820E8A48` (set skip mode `byte_82440579`; lib.e
`0x4749` pairs it with the skip handler in 540), 1014 `sub_820E8A60` (get
skip mode). The player's skip is in the field tick `sub_820FE7F8` and the
pause state machine `sub_820FA2F8`; see `src/engine/cutscene_system.cpp`.

Useful 1000-series natives: 1039 `sub_820E91D0` (object.pos += vector),
1059 `sub_820EA758` (wait ms, converted through `300/fps`), 1108
`sub_820EC6B8` / 1109 `sub_820EC630` (object command list into
`sub_820F29F8`), 1122 `sub_820ECA18` (copy pad state out). Block2 table 2
ids (`4..169`, `20000+`) are script-side symbols (functions and globals).

Field movement: the leader's stick walk never goes through the object
command dispatcher `sub_820F29F8` (only NPC states do), and `bel01.e` imports
no pad or wait native. Player walking is engine code; scripts only nudge.

### Battle signals

`sub_8212D350(&dword_824BF250, name_a, name_b, arg)` pushes a named entry
into the 32-byte-stride queue at `0x824BF280..0x824BFA80` (names are 8-byte
`NAMEABLE_ITEM` strings, flags word at `+20`, time at `+24` = `300/fps`).
`sub_8212D420` looks entries up, `sub_8212D740` clears them. The battle
camera timeline (`sub_820E3F58`, 72-byte keyframes with a wait-for-signal
name at `+56`) consumes them, so a script calling native 45002 with `"S002"`
releases the camera cut waiting on that name. The tutorial scripts do this
before most dialogue lines (`t0002.e` calls it eleven times).

### Per-frame script pushes

`bel01.e` image `0x186f`:

```
slide(x, y, z):                 ; called with (0.015, 0, 0.1) or (0.04, 0, 0.05)
  loop:
    native1039(obj 1, &vec)     ; leader.pos += vec
    sleep 1                     ; 86 01 01 7e
```

Spawned as a task by `0x1899` (array-push the three floats, builtin 7 with
the function pointer, handle stored at `img+0x3568`, killed via builtin 9 at
`0x18ef`). The push is a constant per frame while the player's walk is per
`300/fps` tick, so at 60 fps the slope pushes twice as hard per second
(walking against it drops from 3.48 to 1.56 units/s). The `sub_820E91D0`
hook in `src/engine/overworld_system.cpp` scales the vector by
`30 / byte_82465F90` when the caller's ip points at `86 01 01 7e`; one-shot
placements through the same native are left alone.
