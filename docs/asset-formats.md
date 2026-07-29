# Eternal Sonata asset formats and tooling

Reference for the Xbox 360 `assets/` tree: how files are indexed, how they are
encoded, and how to unpack and repack them.

All multi-byte fields are **big-endian** unless stated otherwise.

---

## 1. `index.vmtoc` — the asset index

`assets/index.vmtoc` is a flat array of 48-byte records. No header, no
footer: 53,040 bytes = **1105 records**. Records are sorted by name and
binary-searched at runtime by `sub_8210D080`, which lowercases before
comparing.

| Offset | Size | Type | Description |
|---|---|---|---|
| 0 | 32 | `char[32]` | Path, NUL-padded, lowercase, `\`-separated |
| 32 | 4 | `u32be` | **Decoded** size in bytes |
| 36 | 1 | `u8` | **Codec flag** (see §2) |
| 37 | 3 | — | Always zero |
| 40 | 4 | `u32be` | Always zero |
| 44 | 4 | `u32be` | Timestamp-like value; **not** a type code |

Files are stored **loose on disk** at the path in the record, relative to
`assets/`. There is no archive blob. The size at `[32]` is the size *after*
decoding and is what the loader allocates.

> The upper byte of `[44]` takes values `0x44`/`0x45`/`0x46` that do not
> partition by extension (`.e` files use both `0x45` and `0x46`, `.bop` uses
> all three), so it is not a file-type field.

7 `camp_grp*.bmd` files have **no TOC record** and are loaded by some other
path.

---

## 2. Codec flags

The flag at `[36]` is a bitmask: **bit 0 = LZSS**, **bit 1 = range coder**.
When both are set the range coder is the outer layer — decode range first, then
LZSS the result.

| Flag | Layers | Count | Extensions |
|---|---|---|---|
| 0 | stored, no transform | 136 | all `.cxs` (62), all `.wav` (14), 60 map `.csf` |
| 1 | LZSS only | 8 | `op.bmd`, 7 AI `.e` scripts |
| 2 | range coder only | 13 | 3 voice `.csf`, 10 `cfdata/e0020*.e` |
| 3 | range coder → LZSS | 948 | everything else |

Per extension:

```
.e      678   flag3=661  flag1=7   flag2=10
.csf    123   flag0=60   flag3=60  flag2=3
.bop    122   flag3=122
.x3tex   90   flag3=90
.cxs     62   flag0=62
.wav     14   flag0=14
.bmd     12   flag3=11   flag1=1
.tex      2   flag3=2
.fnt      2   flag3=2
```

Because the flag is stored **per record**, a file may be rewritten with a
different, simpler codec than it shipped with. This is what makes repacking
easy (§6).

### 2.1 Range coder

Implemented in `sub_8210DFA8` (init) and `sub_8210E0F8` (symbol decode).

```
bytes 0..255   frequency table, one u8 per symbol
bytes 256..259 initial 32-bit code word
bytes 260..    coded payload
```

Init builds cumulative frequencies (`cum[i+1] = cum[i] + freq[i]`,
`total = cum[256]`) and a `total`-sized symbol LUT. `total` is bounded by the
decoder's LUT budget (ctx+816 .. ctx+9020 = 8204 entries); observed totals are
2459–5886.

Per symbol, with `low`/`range`/`code` at ctx+9008/9012/9016:

```
while ((low + range) ^ low) < 0x1000000:      # renormalise
    low <<= 8; range <<= 8; code = code<<8 | next_byte()
while range < 0x2000:                          # underflow rescale
    old = low
    low = old << 8
    range = (-(old << 8)) & 0x1FFF00
    code = code<<8 | next_byte()

r    = range / total
sym  = lut[(code - low) / r]
low += cum[sym] * r
range = freq[sym] * r
```

> The 255 low-entropy header bytes are **raw symbol frequencies**, not Huffman
> code lengths. They fail the Kraft inequality because they were never a prefix
> code.

### 2.2 LZSS

Implemented in `sub_8210E260`. A 4096-byte ring buffer, zero-filled, with the
write cursor starting at **4078**.

Control bytes carry 8 flags, **LSB first**:

- bit set → one literal byte, appended to output and to `ring[pos++]`
- bit clear → a 2-byte match `{lo, hi}`:
  ```
  len = (hi & 0x0F) + 3            # 3..18
  off = ((hi << 4) & 0xF00) | lo   # 0..4095
  ```
  `len` bytes are copied from `ring[off++]`, each appended to the output *and*
  written to `ring[pos++]`.

Because the ring is written while it is read, a match may legitimately overlap
its own output — an encoder must simulate this rather than assume disjointness.

Decoding stops when the TOC's decoded size is reached; trailing flag bits in
the final group are ignored.

---

## 3. `.e` container (678 files)

The universal script/asset container: battle-AI scripts
(`btldata/script/ai/*.e`), tutorials (`btldata/script/tutorial/t000*.e`), and
~600 field/dialogue scripts (`cfdata/*.e`).

Layout is parsed by `sub_820FF9C8`; relocations by `sub_820FF6C0`. Validated
against all 678 decoded files.

### 3.1 Header (24 bytes)

| Offset | Type | Description |
|---|---|---|
| 0x00 | `u32be` | Magic `0x00000181` (loader accepts `0x180` or `0x181`) |
| 0x04 | `u32be` | id/hash |
| 0x08 | `u32be` | Timestamp-like |
| 0x0C | `u32be` | **Total size** — must equal the decoded file length |
| 0x10 | `u32be` | `image_size - 0x18` |
| 0x14 | `u32be` | `reloc_off` |

### 3.2 Sections

```
image      = file[0x18 .. 0x18 + hdr[0x10]]     copied to a fresh allocation
                                                and relocated at load time
bulk       = file[image_end .. reloc_base]      text/resources; NOT loaded by
                                                the loader, streamed on demand
reloc_base = image_end + hdr[0x14]
  listA:  u32be count, u32be offsets[count]     patched against the image base
  listB:  u32be count, u32be offsets[count]     second relocation class
block2     = rest of file                       four chained fixup tables
```

`sub_820FF9C8` allocates `hdr[0x10] + 24` bytes and memcpy's the file's first
`hdr[0x10] + 24` bytes into it — **starting from file offset 0**, so the
24-byte header IS part of the image allocation. List entries in the relocation
tables are therefore raw file offsets (not relative to `+0x18`). The loader
then runs the two relocation lists, copies `block2` into a second allocation
and runs `sub_820FF748`, `sub_820FF838`, and `sub_820FF910` twice over it.

`block2` is four chained fixup tables. In `t0001.e`:
- Table 1: 229 `{u32be symbol_id, u32be patch_offset}` pairs sorted by
  `symbol_id` — an import/symbol fixup table.
- Table 2: 7 pairs (same format).
- Tables 3/4: empty (count = 0).

No bulk-relative offsets were found in any block2 table — all patched offsets
target the image, not the bulk.

`sub_820FF6C0` stores each relocated dword **byte-reversed**, so relocated
pointers end up little-endian in the stream while everything around them stays
big-endian.

Image sizes are round for files with a large bulk, which is why the tutorial
loader reads exactly `0x2000` bytes and the AI loader exactly `0x1000`:

```
                       size      image     bulk       relocA relocB block2
bos01_v1.e (ai)        0x3208    0x254e    0x72        180     4     2400 B
t0001.e                0x40e184  0x2000    0x40b4b0    279    64     1904 B
t0002.e                0x2c24bc  0x2000    0x2bf9dc    328    74     1168 B
adg01.e (cfdata)       0x62f464  0x4000    0x629fa0     82    85     4640 B
adg40.e                0x169048  0x1000    0x167e08     12     8      488 B
```

### 3.3 Bytecode

The image is a packed byte stream, not an array of structs: relocation offsets
are byte-unaligned. Relocations patch 32-bit absolute pointers embedded at
arbitrary offsets.

The byte immediately preceding every relocation across all 678 files:

```
listA:  0x7a ×31179   0x07 ×29445   0x0c ×3798   0x0a ×1350   0x0e ×528   0x0b ×4
listB:  0x07 ×38871   0x89 ×485
```

So opcodes `0x07 0x0a 0x0b 0x0c 0x0e 0x7a 0x89` each take a 4-byte relocatable
pointer operand. `0x81` recurs as a statement/expression terminator.

The opcode set is **not fully decoded**. See §7.

### 3.4 Text blocks — the `BTX ` section

Text lives in the *bulk* section, inside a self-describing `BTX ` blob (545 of
the 678 `.e` files contain one). The reader is `sub_8223B780(btx, string_id)`
in `default.xex`; the layout below is decompiled from it and verified by
re-parsing `extracted/e/btldata/script/tutorial/t0001.e`.

All fields are u32 big-endian. Offsets are **relative to the start of the
struct that contains them**, never absolute.

```
BTX header (at `btx`)
  +0x00  char[4]  'BTX '
  +0x04  u32      offset to first language sub-block   (0x10 in practice)
  +0x08  u32      total size of this whole BTX blob
  +0x0C  u32      language sub-block count             (always 7)

language sub-block (at `q`, first = btx + hdr[0x04])
  +0x00  char[4]  language fourcc — 'JPN ' 'USA ' 'GBR ' 'FRA ' 'ITA '
                                    'DEU ' 'ESP '
  +0x04  u32      offset to the entry table            (0x14)
  +0x08  u32      offset to the NEXT sub-block, relative to `q`
  +0x0C  u32      total size of this sub-block's string data
  +0x10  u32      entry count                          (36 in t0001.e)

entry table (at `q + sub[0x04]`), `count` × 8 bytes
  +0x00  u32      string_id
  +0x04  u32      offset to the string, relative to `q`
```

Lookup is **by string id**: the reader linear-scans the entry table for
`entry.id == string_id` and returns `q + entry.offset`. In practice every
blob's ids turn out to be dense `0..count-1` and identical across all seven
languages (checked over all 758 blobs), so an id doubles as an index — but
the reader does not assume that, and a repacker need not either. The
language is chosen at runtime by `off_822FF578[dword_8243D370]`, a 7-entry
table of the fourccs above (index 0 = `JPN `, 1 = `USA `, … 6 = `ESP `) — see
§3.4.1 for who supplies the string ids.

Two gotchas, both of which will silently corrupt a naive parser:

- The sub-block chain **may or may not self-terminate**, and both shapes ship.
  In some files the last language's "next" is 0; in others (`t0001.e`) it
  still points forward, past the end of the blob, into whatever follows.
  Always bound the walk with `hdr[0x0C]` rather than waiting for a 0.
- A file may hold **several BTX blobs**, packed back to back and 4-byte
  aligned (`adg01.e` has two). Nothing found so far indexes them; the bulk
  section has no directory of its own, so `scripts/btx.py` locates them by
  scanning for the magic and validating by parse.

`scripts/btx.py` implements all of the above:

```bash
python scripts/btx.py extracted/e/btldata/script/tutorial/t0001.e
python scripts/btx.py --lang USA extracted/e/cfdata/adg01.e
python scripts/btx.py --json "extracted/e/cfdata/*.e" > text.json
```

Across the 678 decoded files it finds **758 blobs in 545 files, 103,229
strings**, with the blob count matching the raw `BTX ` magic count exactly —
no parse failures and no false positives.

### 3.4.2 Editing text

`scripts/btx_edit.py` rewrites a string and rebuilds the blob (recomputing
every entry offset, `next` link and size). Its serialiser reproduces all 758
shipped blobs **byte-for-byte** when no edit is applied, so the layout above
is exact.

By default the entire blob is rebuilt and spliced in; any length change shifts
everything that follows in the file. **This is now handled automatically** —
the tool reads list B of the `.e` relocation table and adjusts every raw dword
that points into the post-BTX area (the debug string pool) by `delta`, so the
loader's `*(image + entry) += bulk_base` still lands on the right data.

| edit | size | result |
|---|---|---|
| full replacement, blob grew 4 bytes | 4252040 | works |
| full replacement, blob shrank 207 bytes | 4251829 | works |
| `--preserve-size` | unchanged | works |

The crash mechanism was: 47 of 64 list B entries held bulk offsets pointing
into the region *after* the BTX blob (the debug string pool). Growing the blob
shifted that data but the raw dwords embedded in the bytecode image stayed at
their old values. The loader added `bulk_base` and produced the right absolute
address, but the data at that address was now wrong — shifted by `delta` —
so the script VM dereferenced garbage, producing a wild handler pointer like
`0x52054163`.

`--preserve-size` is still available as a safety valve: it keeps every
sub-block at its original length and rewrites only the edited one, leaving
every other byte of the file untouched. It buys room for a longer string by
**deduplicating identical strings** within that sub-block so they share one
copy (entry offsets are arbitrary, so the reader cannot tell), then NUL-pads
the remainder. If a block has no duplicates to reclaim, the tool fails loudly:

```bash
python scripts/btx_edit.py <file.e> --lang ITA --id 1 \
    --replace "così" "quindi" --preserve-size
python scripts/repack_e.py <mod_tree> --only tutorial/t0001 --out repacked
```

Strings are NUL-terminated. A newline is a **literal two-character `\` `n`**,
not `0x0A`. `JPN ` is Shift-JIS; the western blocks are single-byte
(Latin-1-like — whether this is true Latin-1 or a custom glyph table is
unverified; compare against `p1.fnt`).

Entry 0 in a tutorial file is a `<y6><l6><m1><name>\0` label record naming the
file itself (`t0001`), so ids are dense from 0 and the first real line is id 1.

### 3.4.1 Who supplies the string ids

For battle/tutorial narration the id comes straight out of the battle manager
singleton `dword_824D0440`:

```
party member records   base unk_824FD1A0 (= mgr + 183648), stride 81972,
                       count = byte_824D0720
                       record[0]      = character id, 1..10
                       record[81698]  = i16 model/kind id

enemy records          base unk_82539240 (= mgr + 461840), stride 32456,
                       count = byte_824D0721
                       sub-record at +429568, stride 16136, index at +461840
                       sub[300]       = i16 enemy id
```

`sub_821ABC68(mgr, desc)` is the accessor. `desc` is the 2-field descriptor
`{u32 kind, …, u8 slot@+4}` that the narration objects carry:

```
kind 0 (party)  string_id = party[slot].character_id - 1
kind 1 (enemy)  string_id = enemy[slot].enemy_id + 9
```

`sub_821ABE88` then does `sub_8223B780("BTX ", string_id)` and hands the result
to the label widget — so party ids 1..10 map to BTX ids 0..9 and enemy ids
follow directly after. This is the missing link between the runtime and the
text section; `docs/script-vm-notes.md` §5.3 covers the object pool that owns
the descriptors.

### 3.5 Markup tags

`sub_821D50A8` expands `<...>` tags into single-byte control codes in a scratch
buffer, storing each tag's numeric argument in a parallel slot array at
`a2 + 8*(argidx + 65)` (counter at `a2 + 840`).

| Tag | Code | Meaning |
|---|---|---|
| `<w>` | 2 | Wait for player input, no timeout (29,473 uses) |
| `<wNNNN>` | 1 | Auto-advance after NNNN ms, arg = NNNN (26,829 uses) |
| `<wv>` | 13 | Wait for the voice clip to finish (568 uses) |
| `<p>` | 3 | — |
| `<f N>` | 4 | default 0 |
| `<m N>` | 5 | default 0 |
| `<l N>` | 6 | default 0 |
| `<c X>` | 7 | default -1; arg is a name or an int |
| `<d N>` | 8 | default 40 |
| `<U>` | 11 | — |
| `<g N>` | 12 | default 1, clamped 1..2; speaker |
| `<a N>` | 14 | default -1 |
| `<Q N>` | 15 | default 0 |
| `<ar N>` | 24 | clamped 1..4 |
| `<i N>` | 25 | default 1 |
| `<#N>` | — | literal character with code N |
| `<">` `<<>` `<>>` | — | literal character |
| `<v N>` | — | voice-clip index, 0..88 |

**Message duration** is `<wNNNN>`, in milliseconds: 71 distinct values,
500–20750, 99 % of them exact multiples of 250. It is authored per text entry
and is identical across all 7 language blocks, so it is not localisable.

Characters `n`..`z` dispatch through a 13-entry `bctr` jump table at
`word_820821B8`, base `loc_821D5740`. Hex-Rays emits `__asm { bctr }` for it —
read it with raw `disasm`.

---

## 3.6 Loading pipeline and custom content

Traced end to end in IDA (`default.xex.i64`). No encryption anywhere in this
chain — just the codecs in §2.

1. **`sub_8210D168`** (TOC init, runs at boot) — opens `game:\index.vmtoc` via
   `sub_82252458`, allocates a buffer sized to the file, reads it whole into
   `dword_8244061C`, and sets `dword_82440820 = filesize/48` (record count).
   Also builds `byte_82440620` = `"game:"` + trailing `\`, the root prefix
   used for every asset open.
2. **`sub_8210D080`** — the binary search described in §1, over
   `dword_8244061C`/`dword_82440820` by lowercased path.
3. **`sub_8210C9D8`** — the open primitive. Builds the full path (`host://`
   passthrough, or `byte_82440620` + name → `game:\<path>`), opens it with
   `sub_82252458`, *then* looks it up in the TOC:
   - **found**: decoded size (`a1[6]`) = TOC `+32`, codec flag (`a1+16`
     byte) = TOC `+36`.
   - **not found**: falls back to raw/stored — decoded size = actual file
     size via `sub_822528D8`, codec flag forced to `0`. This only fires if
     the file open itself still succeeded, i.e. the file exists on disk at
     that path.
4. **`sub_820FF458`** — async load wrapper (used for `.e` and others):
   allocates a job struct, calls `sub_8210C9D8` to resolve size/flag, queues
   a decode job via **`sub_8210CC68`**.
5. **`sub_8210CC68`** — reads the codec flag byte at `+16`; if nonzero, hands
   off to a worker queue (`dword_82466108` via `sub_8210D5D8`) that runs the
   range coder (`sub_8210DFA8`/`sub_8210E0F8`) and/or LZSS (`sub_8210E260`)
   from §2.

So the TOC is a **lookup table for size + codec flag**, not a manifest gate:
a file with no TOC entry still loads (uncompressed, sized by `stat()`) as
long as it exists on disk and something references its path.

**Custom content:**

- **Replacing an existing asset**: edit the decoded file, fix the `.e`
  header's total-size field at `+0x0C` if the length changed, and repack
  with `scripts/repack_e.py --in-place`. This is the verified route — same
  path, TOC size/flag patched to match, decoder round-trip checked.
- **Adding a brand-new path**: drop the file under `assets/` and it loads
  raw/stored even with zero TOC entry, *provided* some other game data
  (script, table) already references that path by name — nothing does yet
  for genuinely new content. Use `scripts/repack_e.py --add-new` if you want
  it to get a real TOC record (size/flag bookkeeping) instead of relying on
  the fallback; it inserts a new 48-byte record in sorted order. Untested
  at runtime.

---

## 4. Other formats

| Format | Notes |
|---|---|
| `.fnt` | `FONT` magic, `u32be` total size, then `01 00 02 00 / 00 30 00 30 / 40 80 c0 00 / "BC  "`. `0x0030 0x0030` is a 48×48 glyph cell. `p1.fnt` = 1,598,328 B, `p1_g.fnt` = 1,676,392 B. |
| `.bmd` | `BMD ` magic; entry count at +8 (≤512), relative offsets at +12. Loader `sub_82162058`. |
| `.bop` | `BOP ` magic; entry count at +8, relative offsets at +12. Dispatch `sub_821A0C30` (5 slots, state machine 0→1→2→3). |
| `.wav` | Xbox 360 **big-endian** RIFF/WAVE, 16-bit BE PCM. Convert with `scripts/convert_wav.py`. |
| `.cxs` | Music. Flag 0, readable headers. |
| `.csf` | Sound banks (flag 0, 60 map banks) and compressed voice audio (flag 3, 60 files). Referenced via `"%spc%03d.csf"` / `"%spc%03d_usa.csf"` at ~`0x820872C8`. |
| `.x3tex`, `.tex` | Textures. Not investigated. |

---

## 5. Unpacking

`scripts/unpack_e.exe` (C, fast) and `scripts/unpack_e.py` (reference,
readable, slow) implement both codec layers.

Build the C version:

```bash
clang -O2 -D_CRT_SECURE_NO_WARNINGS -o scripts/unpack_e.exe scripts/unpack_e.c
```

Run it. The third argument is a **filename suffix filter**, defaulting to `.e`:

```bash
# all 678 .e files -> extracted/e/  (~40 s, 4.2 GB)
./scripts/unpack_e.exe assets extracted/e

# one other type at a time
./scripts/unpack_e.exe assets extracted/other .fnt
```

> Pass an empty suffix (`""`) to decode all 1105 records at once. **This does
> not work from PowerShell**, which silently drops empty native arguments — use
> Bash, or run one suffix at a time.

Output paths mirror the TOC. The tool reports `N/N decoded to full size` and
names any file whose output length disagrees with the TOC.

Inspect a decoded `.e`'s section layout:

```bash
python scripts/e_layout.py extracted/e/btldata/script/tutorial/t0001.e
```

Dump its text (see §3.4 for the format):

```bash
python scripts/btx.py --lang USA extracted/e/btldata/script/tutorial/t0001.e
```

---

## 6. Repacking

`scripts/repack_e.py` writes modified decoded files back into loadable form and
patches `index.vmtoc`.

Because the codec flag is per-record, a repacked file can use a **simpler codec
than it shipped with**. Two modes:

| Mode | Flag | Notes |
|---|---|---|
| `stored` (default) | 0 | Bytes verbatim. Exact by construction, instant, larger on disk. |
| `lzss` | 1 | Real LZSS encoder mirroring `sub_8210E260`. ~600 KB/s. |

Every encode is decoded again with `unpack_e.py` and compared to the input; a
file is written only if it round-trips exactly.

```bash
# stage into ./repacked (default), then copy over assets/ yourself
python scripts/repack_e.py extracted/e --only "tutorial/t0001" --mode lzss

# or write straight into assets/ (index.vmtoc is backed up to .bak first)
python scripts/repack_e.py extracted/e --only "script/ai/" --in-place
```

`--only` matches a substring of the TOC path, `/` or `\` either way. Only TOC
entries that have a matching file under the source directory are touched.

Compression achieved by `lzss`:

```
btldata\script\ai\bos01_v1.e      12808 ->    5769   45%
btldata\script\ai\*  (106 files)   1.95M ->  907885   47%
cfdata\adg40.e                  1478728 -> 1075665   73%
btldata\script\tutorial\t0001.e 4252036 -> 4091609   96%
```

Text-heavy files barely compress with LZSS alone — the range coder is what
compresses those (`t0001.e` ships at 3.87 MB with flag 3). Flags 2 and 3 are
**not** implemented by the repacker; nothing needs them, since any file can be
rewritten as flag 0 or 1.

### Editing checklist

1. Edit the decoded file under `extracted/e/`.
2. If the length changed and it is a `.e`, patch the header total size at
   `+0x0C` to the new length. The repacker warns if you forget.
3. Repack. The TOC size field is updated automatically.

### Verification

108 files have been re-encoded with `lzss` and round-tripped with zero
failures. What is **not** verified is runtime acceptance — that the game loads
a flag-0/flag-1 file where it shipped a flag-3 one. The code path demonstrably
exists (136 flag-0 and 8 flag-1 files ship that way) and the flag is read per
record, but this has not been tested in-game.

---

## 7. Next steps

**Script VM opcodes.** The interpreter is **`sub_820FFE28`** — found 2026-07-29
from an lldb backtrace, not by static search; it is in the `.e` loader cluster,
not near the battle FSMs where seven earlier rounds looked. It fetches a raw
**byte** from the stream, advances the IP, and dispatches through a jump table
for opcodes `0x00..0x89`. `sub_820FFCA0` initialises the IP to
`image_base + 0x18`, confirming the VM executes the `.e` image from `+0x18`
exactly as §3.3 assumed. (`sub_821C9FE0` was never the interpreter — it is a
hardcoded FSM; see `docs/script-vm-notes.md` §1.5 and §7.)

What remains is the opcode table itself: read the 0x8A-entry jump table's raw
dwords with `get_bytes`, decompile handlers individually, and cross-check
operand widths against §3.3's relocation-adjacency statistics. Then build a
disassembler that walks the image from `+0x18` and run it over all 678 files;
consuming every file with no unknown opcode and no desync is a much stronger
signal than eyeballing one file.

**Markup control-code consumer.** Where codes 1/2/13 are acted on is unknown. A
scan of `0x821cc000..0x821e8000` for a compare against 13 found nothing, so the
message state machine probably dispatches through its own jump table. Finding
it would allow a proper hold-to-fast-forward instead of the control-code
rewrite currently in `src/eternalsonata_hooks.cpp`.

**Character encoding.** Confirm the JP blocks are plain Shift-JIS (spot checks
say yes) and determine whether the western blocks are true Latin-1 or a custom
glyph table, by decoding `p1.fnt` / `p1_g.fnt` and comparing glyph order against
the byte values used in text. `GOTHIC_FONT` (`0x82082850`) and `MESSAGE_FONT`
(`0x82082B68`) are the constants to cross-reference.

~~**Language selection.**~~ Solved — `off_822FF578[dword_8243D370]` selects a
language fourcc and the reader walks the sub-block chain for it. See §3.4.

~~**Bulk indexing.**~~ Solved — the bulk `BTX ` section is addressed **by
string id** through `sub_8223B780`, not by array index; the earlier
"`{offset, index}` table" reading had the pair backwards (it is
`{id, offset}`). See §3.4 and §3.4.1.

**Untouched formats.** `.x3tex` and `.tex` have not been looked at at all.
`.bop`/`.bmd` have loaders identified but no format documentation.

**Who hands `sub_8223B780` a BTX pointer for a `.e` file?** The loader
`sub_820FF9C8` copies only `hdr[0x10]+24` bytes — the image — so the bulk
is streamed separately and something must know its offset. Note
`sub_821ABE88` is **not** that path: it passes BTX blobs embedded in
`default.xex` itself (`0x823857D0`, `0x82332D90` — same format, 7 languages,
character-name tables). Not a blocker for text editing, but unanswered.

**Range-coder encoder.** Only needed to keep repacked builds small. Requires
mirroring `sub_8210E0F8`'s two normalisation loops exactly and requantising
symbol counts into one byte each under the 8204-entry LUT budget without
zeroing any symbol that occurs.

---

## Related documents

- `docs/script-vm-notes.md` — function inventory, opcode dispatch tables,
  ruled-out leads, IDA tooling caveats
- `docs/debug-hooks.md` — devkit gate and debug console hooks
