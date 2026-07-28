# Script VM and text system — reverse-engineering notes

Companion to `docs/asset-formats.md`. That document describes what the formats
*are*; this one records where things live in `default.xex`, what has been ruled
out, and what the open leads are.

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
| `sub_8210D470` | Spawns two worker threads: `sub_8210D8E8` and **`sub_8210DC28`** (the decoder thread) |
| `sub_8210DFA8` | Codec init: 256-byte frequency table, cumulative freqs (u16 at ctx+302, total at ctx+814), symbol LUT at ctx+816, primes the 4-byte code word; if flag bit 0, zeroes the 4096-byte ring at ctx+9020 and sets ring pos 4078 |
| `sub_8210E0F8` | Range-coder symbol decode; `low`/`range`/`code` at ctx+9008/9012/9016 |
| `sub_8210E260` | LZSS layer |

The decoder runs on the **async-I/O worker thread**, which is why eleven rounds
of searching the battle-tick call graph never found it.

### `.e` container loading

| Address | Role |
|---|---|
| `sub_820FF2B8` | Entry; validates magic `(*a2 & 0xFFFFFFFE) != 0x180` → reject |
| `sub_820FF9C8` | The loader — allocates, memcpy's the image, runs relocations, copies `block2` |
| `sub_820FF6C0` | Relocator; patches a 32-bit word per list entry |
| `sub_820FF748`, `sub_820FF838`, `sub_820FF910` ×2 | `block2` fixup passes |

`sub_820FF6C0` stores each relocated dword **byte-reversed**, so relocated
pointers end up little-endian in the stream while everything around them is
big-endian.

### File readers

| Address | Role |
|---|---|
| `sub_821BBED8` | Generic async read request `(pool, …, len, type)` |
| `sub_8210C9D8` | Opens the file, handles the `"host://"` path prefix |
| `sub_8210CD20` | Allocates the read buffer (length rounded to 2048-byte sectors) |
| `sub_8210CC68` | Issues the async read |
| `sub_8210CBB8` | Generic handle cleanup/release (104 call sites across ~90 unrelated functions) |
| `sub_8219F698` | Battle-AI script loader — 4 calls, `type=9`, `len=0x1000`, loads `btldata\script\ai\<name>.e`, falling back to `default.e` |

The tutorial load site is case 11 of `sub_821ACBF8`'s dispatch
(`0x821ADA9C`–`0x821ADB60`), `type=9`, `len=0x2000`, filename from the
`{u32 size; char* name}` table at `off_8238E330`. These read lengths are not
arbitrary: they are exactly the `.e` **image** size for those files.

`sub_821BBED8`'s pool argument differs per caller — the tutorial passes a
per-unit pool at `unit + 0x822EC`, the AI loader passes the global
`&unk_8255272C`. The 8-slot / 344-byte-stride array is a **reusable pattern**
instantiated in several places, not one global structure.

### Text system

| Address | Role |
|---|---|
| `sub_821D50A8` | Markup preprocessor (§3) |
| `sub_821D1898` | Parses a decimal argument, returns value + length via out-param |
| `sub_821D1908` | Resolves a `<cNAME>` name to an id |
| `sub_821D1E18` | Called from the `<Q…>` path when `a1[7850] == -1` |
| `GOTHIC_FONT` `0x82082850`, `MESSAGE_FONT` `0x82082B68` | String constants; xref these to reach the renderer |

### Other parsers

| Address | Role |
|---|---|
| `sub_82162058` | `.bmd` loader — `"BMD "` magic, count at +8 (≤512), relative offsets at +12 |
| `sub_821A0C30` | `.bop` dispatch — 5 slots, state machine 0→1→2→3 |
| `sub_821A0E40` | Map BOP parser — `"BOP "` magic at `a1[177]` |
| `sub_821A0178` | BattleKeep BOP loader — `btldata\BattleKeep.bop`, tags buffer `0xFFFF9933` |
| `sub_8219FF58` | Map BOP loader — `btldata\map\<name>`, tags buffer `0xFFFF0000` |

---

## 2. `sub_821C9FE0` — the 67-case dispatcher

`0x821C9FE0`, 0x24E0 bytes. The strongest candidate for the script VM.

Only static xref is a **data-only function-pointer-table slot at `0x820B1030`**,
which itself has no xrefs — so it is dispatched at runtime and static analysis
cannot resolve what supplies its `a1`.

Hex-Rays produces a 3-line stub ending in `__asm { bctr }`. Use raw `disasm` at
each case address; never `decompile` the whole function.

Dispatch, verified by raw disasm at `0x821C9FE0`–`0x821CA078`:

```
lwz    r11, 4(r31)        ; opcode = *(a1+4), a 32-bit field
addi   r11, r11, -1       ; 1-based!  index = opcode - 1
cmplwi cr6, r11, 0x42
bgt    cr6, loc_821CC4B0  ; out of range -> default
lhzx   r0, word_820824A0, index*2
add    r12, loc_821CA078, r0
mtctr  r12 / bctr
```

**The opcode is a dword at `a1+4`, 1-based (1..0x43) — not a raw stream byte.**
So a fetch/decode step sits in front of this and has not been found. Find it
before guessing operand widths; it defines the real instruction encoding.

All 67 case targets, derived from `word_820824A0` + base `loc_821CA078`:

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

`*` = `loc_821CC4B0`, the out-of-range/default exit, so ops `0x26 0x2d 0x37` are
no-ops. `^` = `0x821CA4D4`, shared by `0x1d 0x23 0x28 0x31`. Ops
`0x0b 0x0d 0x0f 0x1f` share `0x821CA67C`; ops `0x2a 0x2c` share `0x821CBC84`.

### Suggested attack

1. Raw-`disasm` each case body; record opcode → operand widths → effect.
2. Cross-check against real streams in `extracted/e/btldata/script/ai/*.e` —
   the AI scripts are smallest (`default.e` is 11,984 bytes decoded) and are
   almost pure image with no bulk. Start there, not with 4 MB tutorial files.
3. Build a disassembler in `scripts/` that walks the image from `+0x18` and
   flags anything it cannot handle.

**Validation:** a correct disassembler consumes every one of the 678 files
end-to-end with no unknown opcode and no desync. That is a far stronger signal
than eyeballing one file. Additionally, every relocation offset must land
exactly one byte past one of `0x07 0x0a 0x0b 0x0c 0x0e 0x7a 0x89` — free
ground truth for seven operand widths.

Start of `extracted/e/btldata/script/ai/default.e` at `+0x18`, `|` marking a
relocated pointer:

```
7a |00000523| 0001 0081 0101 81 180c 81 1808 81
7a |00000e98| 8810783d 03 00000000 81 03 00000000 81 …
```

`0x81` recurs as a statement/expression terminator; other bytes seen are
`0x03 0x07 0x0c 0x18 0x24 0x3d 0x86 0x88`. A tempting split — bytes `< 0x80` are
commands (fits the `<= 0x43` range check), `>= 0x80` are expression tokens —
does **not** hold cleanly, since `0x7a` takes a pointer and is both `< 0x80` and
`> 0x43`.

---

## 3. `sub_821D50A8` — the markup preprocessor

`0x821D50A8`, 0xC18 bytes. Expands `<...>` tags in a raw text entry into
single-byte control codes in a scratch buffer at `a1 + 19104`, stores each tag's
numeric argument at `a2 + 8*(argidx + 65)` (counter at `a2 + 840`), then copies
the scratch buffer to `(a2+8 | a1+35122) + *(u16*)(a2+420)`. The destination
selector is `*(u32*)a2 == a1[9037]`.

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

**Applied:** `src/eternalsonata_hooks.cpp` wraps this function and rewrites
control code 13 → 2 in the output, making `<wv>` messages player-skippable
without touching assets. Flags at the top of that hook also allow 1 → 2. Not
yet verified in-game.

---

## 4. Ruled out

Do not re-investigate these.

**Codec / decode path**

- Standard DEFLATE. zlib raises "invalid code lengths set" / "invalid stored
  block lengths" at every offset tried.
- The 255-byte header as Huffman code lengths. It is a raw symbol frequency
  table; `Σ 2^-len ≈ 1.469` fails Kraft because it was never a prefix code.
- All simple ciphers/S-boxes against the "scrambled header" premise. The premise
  itself was wrong — the files are compressed, not obfuscated.

**Tutorial call graph** (`sub_821ACBF8`, a 1541-instruction per-unit tick state
machine with its own 23-case computed-goto at `0x82082730`, base `0x821ACEDC`)

- `sub_82101750` → `sub_820FF2B8` → `sub_820FF9C8` → `sub_820FF6C0` **for the
  tutorial's type-9 load**. `sub_82101750` receives the buffer *pointer*, so the
  magic check reads the file's first 4 bytes, which are packed and do not match.
  Confirmed dead twice. (The chain is live for *decoded* buffers — that is how
  `.e` files are actually parsed — it just is not reached this way.)
- Slot-struct `+0x4` (`unit+0x8323C`), where case 11 stores the read handle:
  **never read anywhere in the binary.** Two whole-`.text` scans plus a manual
  read of all 1540 instructions and all 23 case bodies. Write-only.
- `sub_821AA500` — generic 2-way vtable event dispatcher; index-select and
  callback, resolvable only at runtime.
- `sub_82197C68` — positional-audio/listener-list updater.
- `sub_821AA6D8` — table lookup keyed on a small discriminant; the `0x23`/`0x21`
  it is compared against are plain integers, **not** ASCII `#`/`!`.
- `sub_821AB1A0`, `sub_821AA730` — combat AI turn/action selection.
- `sub_8218CB70`, `sub_821AE410`, `sub_821B7D78`, `sub_821B98D8` — read only
  struct `+0x0`/`+0x2` as gating bytes.
- `sub_821BA2B0`, `sub_821BB7B0` — false-positive `0x3238` immediates,
  unrelated to the struct.
- `sub_8210CBB8` — generic handle cleanup, 104 call sites.
- The `+0xAC` vtable-slot call pattern — generic per-tick listener dispatch.

**Text sources**

- The target phrase is not plaintext anywhere: whole xex string table and the
  entire `assets/` tree, ASCII and UTF-16LE. The plaintext Italian strings in
  `.data` (`0x823606A9`, `0x82361249`, `0x820560B3`, `0x822F750A`) are item and
  location text reached by computed `base+index*stride+offset` addressing, and
  are unrelated to tutorial narration.
- Voice `.csf` files are compressed audio, not text.

---

## 5. IDA tooling caveats

- **The xref database is incomplete for this binary.** PPC addresses built from
  `lis`/`ori` pairs frequently produce zero xrefs.
- **`insn_query`'s `op_any` is not exhaustive.** It fails to match values that
  appear only as the displacement in `disp(reg)` operands — a scan for `0x3E8C`
  returned zero matches while `lwz r11, 0x3E8C(r31)` was plainly visible in the
  disassembly — and it failed to match `lis`-immediate halves in testing. Never
  treat a zero-match result as a confirmed negative; verify with raw `disasm` on
  a function known to contain the value.
- **Hex-Rays fails on every computed-goto in this binary** (`sub_821ACBF8`,
  `sub_821C9FE0`, `sub_821D50A8`), emitting a stub ending in `__asm { bctr }`.
  Use raw `disasm` at the specific case address.
- `disasm` with `offset: 0` on large functions can return only 10 lines
  regardless of `max_instructions`. Use `offset: 1` — instruction 0 is
  `mflr r12` and never interesting.
- Prefer fresh IDA output over prose from earlier notes. Two separate rounds
  produced contradictory characterisations of `sub_8210CBB8` before a dedicated
  pass resolved it. Always state which call site and arguments you mean, since
  the generic helpers have many callers.
- Reopen with `idb_open` on `assets/default.xex`. If the lock cannot be
  acquired, kill the stale worker via its `pid` from `idb_list`.

---

## 6. Worked example

The line that started the investigation, in
`extracted/e/btldata/script/tutorial/t0001.e` at offset `0x40A21C`:

```
<y6><l6><m1><v1>Gli animali che vivono nella foresta qui intorno, non sono\n
molto forti, così dovrei riuscire a sconfiggerli senza troppi\n
problemi. Tuttavia solo per prudenza, ripasserò ancora le\n
basi del combattimento.<wv>
```

It ends in `<wv>`, so it blocks for the length of its voice clip with no skip
path — the motivation for the `sub_821D50A8` hook in §3.
