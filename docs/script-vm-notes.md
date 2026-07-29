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

## 1.5 `sub_821C9FE0`/`sub_821C0300` are FSMs, not the interpreter

> **Superseded in part (2026-07-29, see §7).** The title of this section used
> to read "There is no central bytecode VM". That went too far. Everything
> below is still correct — `sub_821C9FE0` and `sub_821C0300` really are
> hardcoded state machines, not interpreters — but a real bytecode VM *does*
> exist elsewhere in the binary: **`sub_820FFE28`**, found via an lldb
> backtrace. See §7. Read this section as "these two functions are not it",
> not as "there isn't one".

`sub_821C0300` (0x22C8 bytes,
also a slot in the per-object-type tick table alongside `sub_821C9FE0`) has
the **exact same skeleton** as `sub_821C9FE0`:

```
lwz  r11, 4(r31)          ; state = *(a1+4)
addi r11, r11, -1         ; 1-based
cmplwi cr6, r11, 0x60     ; 97 states here vs 0x42 (67) in sub_821C9FE0
bgt  cr6, <default>
lhzx r0, word_82082528, index*2   ; own, separate jump table
...
mtctr / bctr
```

Case `01` in both functions is the same idiom too: `cmplwi cr6,r7,0 / beq
<default> / li r11,8 / stw r11,4(r31) / b <epilogue>`.

So this is not one interpreter — it's a **pattern that recurs across multiple
independently compiled, hardcoded finite-state machines**, each with its own
fixed case count and jump table, reached through the same per-object-type
tick dispatch (`0x82087108`+). Each `.e` "script" most likely selects *which*
pre-baked FSM function an object runs (by type/folder: AI scripts vs.
dialogue/tutorial scripts likely map to different FSMs — `sub_821C9FE0` at 67
states is a plausible fit for the AI executor, `sub_821C0300` at 97 states for
something richer, e.g. the message/dialogue box). The `.e` image bytes almost
certainly supply *data* the FSM's states read (text, resource ids, relocated
pointers, parameters) — not instructions that drive control flow themselves.

**This retires the "disassemble the `.e` image as a bytecode stream" plan.**
The productive next steps are: (1) enumerate every entry in the
`0x82087108`–`0x8208726C` tick table and identify which script *kind*
(AI/tutorial/dialogue/etc.) maps to which FSM function, most likely by finding
where an object's `a1+4` is *initialized* per instance; (2) for the FSM
believed to own dialogue playback, map its states to the markup control codes
(1/2/13) from §3 below — `sub_821C0300` is the current best candidate given
its size and its own call to `sub_8218D408` (§2.1).

---

## 2. `sub_821C9FE0` — the 67-case dispatcher (hypothesis revised, see §2.1)

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

### 2.1 The fetch/decode step doesn't exist — the "opcode = bytecode" model is likely wrong

Found by tracing `sub_821C9FE0`'s real (non-`.pdata`) data xrefs, which `xrefs_to`
misses (same `lis`/`ori`-split blind spot as §5) but `find_bytes` on the raw
pointer value `82 1C 9F E0` catches:

- `0x820B1030` is a false lead — it's inside `.pdata` (PPC exception unwind
  table), not a real call site.
- `0x820871B0` is real: it's one slot in a flat function-pointer array running
  roughly `0x82087108`–`0x8208726C`. That same array also contains
  `sub_821ACBF8` — **the already-documented per-unit tutorial tick state
  machine** (§4 "ruled out"). So this is a generic **per-object-type tick
  dispatch table**, called once per frame per live object of that type; it is
  not something invoked externally once per script instruction.

Disasm of case `01` (`loc_821CA078`–`0x821ca13c`) and its fallthrough settles
the fetch question: at `0x821ca128` it does `li r11, 7` / `stw r11, 4(r31)` —
**hardcodes** the next state as the literal `7` and stores it straight into
`a1+4`. The instructions immediately after that (`0x821ca138`–`0x821ca13c`)
either jump to `loc_821CC4B0` or fall into `loc_821CA080`, which is `li r11,8`
followed by `b loc_821CC4AC`. And `loc_821CC4AC` is **the function epilogue**:
`stw r11, 4(r31)` then stack teardown and `b __restgprlr_23` — a return, not a
loop back to the dispatch header at `0x821ca030`.

So `sub_821C9FE0` runs **exactly one case per call** and returns. There is no
internal loop, and no separate fetch/decode function feeding `a1+4` — each
case computes or hardcodes its own successor state inline and writes it back
for the *next* call. That fully explains why searching for a shared
byte-stream decoder never found anything: it doesn't exist.

**Revised hypothesis:** this is not a bytecode interpreter reading the `.e`
image as an instruction stream at runtime. It reads more like a
hand-authored-or-codegen'd finite-state machine that was compiled *from* the
original script at build time, where each of the 67 states is native PPC code
baked in as a case, and the `.e` image supplies only *data* (dialogue ids,
relocated pointers, parameters) that individual states read — not control
flow. The "opcode" dword at `a1+4` is a **state index into this function**,
not a cursor position in the byte stream.

**Corroborated** against cases `02` and `03` (`0x821ca140`, `0x821ca244`):
both follow the same shape, and both add a **poll/park** variant not seen in
case `01` — re-check a readiness condition (case `02`: bytes at
`r30+0x83238`/`0x83239` and a call to `sub_8218D408`; case `03`: the return
value of `sub_821C9090`) and either return with `a1+4` **unchanged** (park —
same state re-entered next call) or write the next state and return. This is
a poll-driven finite-state machine with explicit wait states, not a bytecode
loop. `a1` is also a real object, not a plain struct: one path (`0x821ca288`)
loads `a1`'s own vtable pointer at `+0`, then virtual-calls slot `+0x8C`.

This shape — park in a state until some async condition clears, otherwise
advance — is suspiciously similar to what the still-unlocated "markup
control-code consumer" (§7 of `docs/asset-formats.md`: where codes 1/2/13,
i.e. `<w>`/`<wNNNN>`/`<wv>`, actually get acted on) would need to look like.
**Not confirmed**: `sub_821BA4B0`, called early in cases `01`/`06`, looked
like a promising "is the voice clip done" check but decompiles to a generic
reset helper (zeroes two fields across a small pointer array) — it is cleanup,
not the wait condition. The real poll condition for case `02` is whatever
`sub_8218D408` and the two bytes at `r30+0x83238/39` represent; that is the
next thing to chase if pursuing the control-code-consumer angle through this
function.

Where `a1+4`'s *initial* value comes from per object instance (i.e. what
selects which of the 67 states an object starts in, and by extension which
fixed "program" it runs) is still unknown — the `sub_821C9FE0` xrefs are all
indirect through the per-object-type tick table (`0x82087108`+), so finding
the allocator/initializer for these objects (probably where `a1[0]`'s vtable
gets assigned) is the next concrete step, not further case-by-case disasm.

**`sub_821C0300` is not the dialogue system — walk that back.** Its own guard
condition computes `base + 16136 * index`, and `sub_8219F698` (the documented
`btldata\script\ai\*.e` loader) stores its async-read handles at the *same*
16136-byte-stride unit struct, offsets `+16012`/`+16136`/`+32148`/`+32450`.
So `sub_821C0300` operates on the same per-battle-unit struct family as the AI
script loader — it's a sibling per-unit behavior FSM (97 states, larger scope
than `sub_821C9FE0`'s 67), not a UI textbox controller. Its earlier-noted call
to `sub_8218D408` is a shared helper, not evidence of a dialogue role.

**Revised reading of `sub_821C9FE0` itself**, tying together the pieces above:
its wait-gate (`sub_821C9090`, case `03`) computes a distance from
`a1+68`/`a1+72` (a position) to a fixed point and feeds the result into the
resource manager (`dword_824CF500`) via a handle at `object+81688` — the shape
of **3D positional audio (distance-based volume) for a speaking unit**. And
the motivating example in §6 below is a `<wv>`-terminated line from
`btldata\script\tutorial\t0001.e` — **battle tutorial narration**, which is
delivered through a battle unit, not a generic dialog box. Working hypothesis:
`sub_821C9FE0` is the per-unit "tutorial narration" FSM, and its states
implement things like "wait for this unit's voice line, using positional audio
distance to know when it's done" — i.e. this may be exactly the missing
markup control-code consumer, scoped to units running narrated `.e` scripts,
rather than a general-purpose text renderer. Not yet proven; the concrete next
check is whether `unk_82553699` (case 03's early-out flag) or the position
fields at `a1+68/72` can be tied to a known battle-unit struct layout, and
whether `sub_821C9FE0`'s tick-table slot is reached specifically for units
with an active tutorial/dialogue `.e` script loaded.

**Loader-side confirmation, still incomplete.** `sub_8219F698` (the AI script
loader) is called from `sub_821ACBF8` itself — the tutorial per-unit tick
state machine — at `0x821acff0`, inside a loop over every unit in the current
party (bound = byte at `[unit_array+0x2A1]`, stride `0x7EC8` = 32,456 bytes
per unit) that fires once per unit at battle/tutorial start. So the tutorial
tick function is what kicks off each unit's AI-script load. What was **not**
found in this pass: where the resulting FSM object's `a1+4` (state) and
`a1[0]` (vtable, presumably pointing at the `sub_821C9FE0`/`sub_821C0300`
family) get initialized once that async read completes — that almost
certainly happens in whatever callback `sub_821BBED8`'s async read job invokes
on completion, which hasn't been traced. That callback is the single most
direct way to settle every open question in this section: it would show the
exact object layout, which FSM function gets attached for which script kind,
and the state number the FSM starts in.

### Suggested attack (predates §2.1; revisit its premise before following it)

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

### 5.1 Async `.e` read job queue (partial trace, 2026-07-29)

`sub_821BBED8(a1, a2, a3, a4)` is the generic async-read job submitter:

- `a1` is a job-manager struct (called with `&unk_8255272C`); it holds up to 8
  job slots, stride **344 bytes** (`v9 = &a1[86 * slot]`, 86 dwords = 344
  bytes), found via a linear scan for the first slot whose dword at `+4` is 0.
- `sub_8210C9D8(slot+16, filename)` does the actual read kickoff; slot's last
  two dwords (`+340`, `+344`, i.e. indices 85/86 of the 87-dword slot) are set
  to the literal `a3` (job type, always `9` at every call site seen) and `a4`
  (buffer size, always `4096`). Returns the slot index as the "handle", or -1
  if all 8 slots are busy.
- Callers: `sub_8219F698` (AI script loader — stores the handle at
  unit+16012/+32148, per §"highest-value next step" below) and
  `sub_821ACBF8` (tutorial tick FSM, at `0x821adb0c`, four call sites for
  primary/fallback AI + secondary/fallback AI scripts — same pattern,
  confirms `sub_8219F698`'s call shape isn't unit-specific plumbing).
- **Completion check found**: `sub_821ACBF8` (around `0x821ae0a0`) reads
  `*((_DWORD *)&unk_82552738 + 86 * handle)` — `unk_82552738` is
  `unk_8255272C + 12`, i.e. **the same 344-byte-stride array, offset to a
  different field within each slot** (a "result ready" pointer/flag, separate
  from the status dword at index 85). `handle` comes from a per-object byte
  field at `v1+537148`. When set (and gated by flag bytes at `v1+537144`
  `v1+537145`), it calls `sub_821422B0(dword_8243D89C, 20, 0.0)`.
  - **Dead end for the FSM-construction question**: `sub_821422B0` is not an
    object constructor. It calls `sub_8213FB80(dword_8243D89C+22416, 14, 1, 0,
    0, &v10)` — shape matches a priority/event dispatch (arg `14` = message
    id, `20`/`0.0` from the caller look like priority/volume). Most likely
    this is the **tutorial voice-line trigger** firing once its `.e` script
    data has arrived, not where `a1[0]`/`a1+4` (vtable/state) of the
    `sub_821C9FE0`/`sub_821C0300` FSM objects get set.
  - This does newly confirm the *shape* of completion detection though: poll
    a slot-relative field in the `unk_8255272C`/`unk_82552738` job array by
    stored handle index, gated by a couple of flag bytes on the owning
    object. The still-missing FSM-attach point is presumably a **different**
    reader of the same job array (there are 8 slots and only 4 producers
    traced so far — `sub_8219F698`'s two handle fields haven't been traced to
    a consumer yet).
- **Dead-ended further**: `sub_821422B0` → `sub_8213FB80` turned out to be a
  generic ring-buffer message-post helper. `dword_8243D89C` (the object it
  posts to, at `+22416`) is a global message-hub singleton with **~100+**
  tiny (0x50–0x300-byte) wrapper functions in `0x8213F000`–`0x82143600`, each
  presumably posting one message type. This is a general event-bus
  subsystem, unrelated to FSM construction — do not follow it further for
  this question.

### 5.2 FSM object construction found (2026-07-29, same session)

Found by working backwards from `off_82087108` (the tick-table/vtable
address) instead of forwards from the job queue.

- **Constructor**: `sub_821A6DF0(a1)`. Confirms the object is real
  C++-style: `*(_DWORD *)a1 = &off_82087108` (vtable pointer, first field),
  `*(_DWORD *)(a1+4) = 1` (**initial state is 1**, not 0 — matches the
  "1-based state, `-1` before range check" idiom noted in §1.5/§2.1). Also
  zeroes/inits ~30 more fields (positions, flags, floats) — a plain POD-style
  init, not per-script data.
- **Spawner**: `sub_821A9F68(a1)`, the only caller of the constructor. Walks
  up to 8 slots at `a1+533044` (a second bank of 8 at `a1+533056`, gated by
  `*(BYTE*)(a1+737)`, same `a1` as `sub_821ACBF8`'s `v1` — this is the
  battle/tutorial manager object, not a per-unit struct). For each empty slot
  (`*v3 == 0`) whose request-type field at `a1+183648 + slot*81972` is in
  `[1,10]`: `sub_820C0000(124)` allocates 124 bytes, `sub_821A6DF0`
  constructs it, then two virtual calls: `vtable+28(obj, context_ptr,
  slot_index)` (init, passing the per-slot request struct) and `vtable+4(obj,
  8)` (purpose not traced — likely `SetState`/`Enqueue` with request kind 8).
  **This is the "still-missing FSM attach point"** the previous session was
  looking for — but it attaches to a shared 8(+8)-slot **narration-line
  object pool** owned by the manager, not to individual battle units. Revises
  the "per-unit tutorial FSM" framing in the current-best-hypothesis section
  above — units aren't the owner; the manager's line-slot array is.
- **Vtable layout** (raw dwords read from `0x82087108`, image-relative):
  - `+0` (`0x82087108`) = `sub_821C9E78`
  - `+168` (`0x820871B0`) = **`sub_821C9FE0`** (the 67-case dispatcher,
    §2/§2.1)
  - `+172` (`0x820871B4`) = `sub_821CCE70` — **the master per-frame Tick**,
    called from `sub_821ACBF8`'s per-line loop (`(*(*v21)+172)(*v21)`), once
    per active slot, every manager tick.
  - `sub_821CCE70` decompiles cleanly (no `bctr` issue — it's not itself a
    dispatch skeleton) and its body **calls `(*a1+168)(a1)`**, i.e. it calls
    `sub_821C9FE0` as one virtual sub-step of its own update, alongside
    `vtable+164`, `vtable+0`, and several dozen non-virtual helpers
    (`sub_821966D0`, `sub_821C8E88`, `sub_821C6450`, etc. — not yet
    individually identified). **This confirms `sub_821C9FE0` really is "one
    state transition per tick" for these line objects, driven externally by
    `sub_821CCE70`** — matches §2.1's finding exactly (no internal loop in
    `sub_821C9FE0`, one state step per call).
- Not yet identified: what `vtable+0`, `vtable+4`, `vtable+28`, `vtable+164`
  do (only `+168`=`sub_821C9FE0` and `+172`=`sub_821CCE70` are named);
  ~~what the 81,972-byte-stride request struct at `a1+183648` contains~~
  (**answered in §5.3 — it is the party-member record array, and the guess
  that it holds an `.e` filename/job handle was wrong**); and
  whether `sub_821C0300` (97-case sibling FSM, thought in an earlier
  self-correction to be a battle-unit AI FSM, see §1.5) is reached via an
  analogous vtable+168-style slot in a **different** vtable — worth reading
  that vtable's raw bytes the same way (`get_bytes`, not `disasm`, since IDA
  doesn't mark every table slot as `.long`) once found.

### 5.3 The "request struct" is the party-member table (2026-07-29)

§5.2's open question — what lives at `a1+183648 + slot*81972` — is answered,
and the answer reframes the whole pool. **It is not a script request. It is
the battle party-member record array, and the field the spawner gates on is
the character id.**

- The manager `a1` is the global singleton **`dword_824D0440`**. IDA already
  names the array base: `unk_824FD1A0` = `0x824D0440 + 0x2CD60` = manager +
  183648. Once you search on the symbol instead of the folded offset, all 22
  references are reachable via `xrefs_to`.
- Layout (all confirmed by multiple independent readers):

  | thing | base | stride | count |
  |---|---|---|---|
  | party members | `unk_824FD1A0` (mgr+183648) | 81972 | `byte_824D0720` |
  | enemies | `unk_82539240` (mgr+461840) | 32456 | `byte_824D0721` |

  `record[0]` = **character id, 1..10** (Eternal Sonata has exactly 10
  playable characters). `record[81698]` = `i16` model/kind id.
  `unk_82510FE8` (= mgr + 265128) is the `{cur, max}` HP pair in the same
  records — `sub_821BAB70` computes `cur/max < 0.2` from it to pick a
  low-health chatter line.
- So `sub_821A9F68`'s `(unsigned)(*v4 - 1) <= 9` gate reads "this party slot
  holds a valid character" — the pool is **one narration/label object per
  live party member**, spawned at battle start. The `vtable+28` init
  (`sub_821A6DD8`) is trivial and pins the mapping down:
  `obj+0x20 = &record`, `obj+0x10 = slot index`, `obj+0x0C = 0`. Every
  consumer (`sub_821C8B58`, `sub_821ABC68`, `sub_821ABE88`) reads the slot
  back out of `obj+0x10` / `obj+0x20`.
- **The link to the text section**: `sub_821ABC68(mgr, desc)` converts a
  `{kind, slot}` descriptor into a BTX string id — `party.character_id - 1`
  for kind 0, `enemy.enemy_id + 9` for kind 1 — and `sub_821ABE88` feeds that
  to `sub_8223B780("BTX ", id)`, the bulk-section text lookup. That reader is
  fully decoded in `docs/asset-formats.md` §3.4 and verified against
  `t0001.e`; it also closes the "Bulk indexing" and "Language selection"
  items in that document's §7.
- Several functions read the character id as an enum, so the values are
  characters, not opcodes: `sub_821AB7E0` branches on `== 7` and `== 8`,
  `sub_821ABD78` on `== 3`, `sub_821BAB70` searches the array for ids
  1/2/3/6/7/8 to find "which slot is character X".

**Consequence for §5.1**: the job queue and the FSM pool are *not* two halves
of one mechanism. The pool is keyed off party composition, not off loaded
`.e` bytes, so the `sub_821BBED8` handles never reach it. Don't spend more
time looking for that join.

**Still open**: `vtable+0`, `vtable+4`, `vtable+164` on `off_82087108`; and
locating `sub_821C0300`'s owning vtable (§5.2's last bullet, unchanged).

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

---

## 7. The script bytecode VM: `sub_820FFE28` (2026-07-29)

Found by accident, from an lldb backtrace of a crash caused by a hand-edited
`.e` file — not by static search. Seven prior rounds of call-graph walking
missed it because it is nowhere near the battle/tutorial FSMs; it sits in the
`.e` loader cluster at `0x820FF000`–`0x82101FFF`, next to `sub_820FF9C8`.

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

This is the fetch/decode step §7 of `docs/asset-formats.md` said "sits in
front of the dispatcher and has not been found". The opcode really is **a
raw byte from the stream**, 0x00..0x89 — matching the §3.3 statistics
(`0x07 0x0a 0x0b 0x0c 0x0e 0x7a 0x89` take pointer operands, all ≤ 0x89).

### Context layout

The VM context is at `owner + 48`, so `a1[n]` below is `ctx + 4n`:

| field | meaning |
|---|---|
| `ctx+0`  | script/module handle |
| `ctx+4`  | slot id in the `unk_8241006C` 512-entry handle table |
| `ctx+8`  | run state: 0 = finished, 1 = running, 2 = sleeping (`ctx+12` = countdown), 3 = done |
| `ctx+12` | sleep counter, decremented per call while state 2 |
| `ctx+24` | operand/eval stack pointer |
| `ctx+32` | **instruction pointer** |
| `ctx+40` | stack base |

`sub_820FFCA0` (the init) sets the IP:

```c
*(_DWORD *)(a1 + 32) = *(_DWORD *)(sub_820FEED0(*(_DWORD *)a1) + 8) + 24;
```

`sub_820FEED0(handle)` returns the loaded-`.e` record; its `+8` is the base of
the image allocation, and `+24` is `0x18` — **the VM executes the `.e` image
starting at file offset 0x18**, exactly the region §3.3 profiled. One call to
`sub_820FFE28` runs until the script yields, not one instruction.

### Call path

```
sub_821ACBF8   tutorial FSM
  sub_82101D70   allocate a 104-byte script object (vtable off_82082D08)
    sub_821014E8   sub_820FFCA0 (init IP/stack) then sub_820FFE28 (run)
```

### Opcode table (hand-decoded)

`word_82081F40` is a 138-entry `u16` table; `handler = 0x820FFEDC + table[op]`.
This gives **138 opcodes `0x00..0x89`, 136 unique handlers**, spanning
`0x820FFEDC..0x8210129C`. Two aliases share handlers: `0x03`/`0x07`
→ `0x820FFF24`, and `0x38`/`0x39` → `0x8210070C`.

The seven opcodes §3.3 identified as taking a 4-byte relocatable pointer
operand land at:

```
0x07 -> 0x820FFF24    0x0C -> 0x82100134    0x89 -> 0x8210129C
0x0A -> 0x8210009C    0x0E -> 0x821001D0
0x0B -> 0x821000E8    0x7A -> 0x82100F64
```

Regenerate the full map with:

```python
# get_bytes(0x82081F40, 276) -> 138 BE u16s
handler = 0x820FFEDC + table[opcode]
```

### VM dispatch

The IP is at `ctx+32`. The fetch/decode/dispatch sequence:

```asm
0x820FFE9C  lwz  r11, 0x20(r31)     ; ip = ctx+32
0x820FFEA0  lbz  r10, 0(r11)        ; op = *ip
0x820FFEA4  addi r11, r11, 1        ; ip++
0x820FFEA8  cmplwi cr6, r10, 0x89
0x820FFEB0  bgt  loc_82101304       ; op > 0x89 -> skip, keep looping
0x820FFEC0  lhzx r0, word_82081F40, op*2
0x820FFED8  bctr                    ; handler = 0x820FFEDC + table[op]
```

### Next step for the opcode table

The dispatch is `bctr` through a jump table, so Hex-Rays emits `__asm { bctr }`
(§5's caveat). Read the table's raw dwords with `get_bytes` — 0x8A entries —
then decompile handlers individually. That plus §3.3's relocation-adjacency
statistics should settle operand widths quickly. This supersedes the
"revisit its premise" note in §2.1: the premise was wrong, there *is* a fetch
step, and it was just in a different neighbourhood.

### Root cause: why growing a `.e` crashed (now fixed)

The crash was **47 of 64 list B relocation entries** holding bulk offsets into
the region *after* the BTX blob (the debug string pool). Growing the blob
shifted that data but the raw dwords embedded in the image stayed at their old
values. The loader (`sub_820FF6C0`) adds `bulk_base` at load time and produces
the correct absolute address, but the data at that address was wrong (shifted
by the blob's size delta), so `sub_820FFE28` read garbage and hit a wild
opcode-handler pointer like `0x52054163`.

The handoff's earlier "list A/B ruled out" conclusion was wrong — the check
read at the wrong file position (the image allocation starts at `file[0]`,
not `file[0x18]`, so the list entries are raw file offsets).

Fixed in `scripts/btx_edit.py:fix_relocations()` — it parses list B, finds
every entry whose raw dword points past the old BTX end, and adjusts it by
`delta`. Applied automatically on every non-`--preserve-size` edit.

### Catching a crash

The crash needs a human to play to the first tutorial battle (~1 minute; no
save files exist). `logs/` is useless — nothing flushes past
`SetInterruptCallback` on a hard crash.

```bash
lldb -b -s cmds.lldb -- ./eternalsonata.exe --game_data_root assets --gpu_plugin=xenos
# cmds.lldb:
#   settings set interpreter.stop-command-source-on-error false
#   run
#   thread backtrace all
#   register read ...
#   quit
```
