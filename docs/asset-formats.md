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
`hdr[0x10] + 24` bytes into it, runs the two relocation lists, then copies
`block2` into a second allocation and runs `sub_820FF748`, `sub_820FF838`, and
`sub_820FF910` twice over it.

`block2`'s first table is `{u32be symbol_id, u32be patch_offset}` pairs sorted
by `symbol_id` — an import/symbol fixup table.

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

### 3.4 Text blocks

Text lives in the *bulk* section. `t0001.e` has 7 language blocks, each starting
with a `<y6><l6><m1><name>\0` label record:

```
0x4074E4  Japanese (Shift-JIS)
0x407F78  0x408D43  0x409B0D  0x40A876  0x40B6BE  0x40C51C
```

Strings are NUL-terminated. A newline is a **literal two-character `\` `n`**,
not `0x0A`. Western blocks are single-byte (Latin-1-like — whether this is true
Latin-1 or a custom glyph table is unverified; compare against `p1.fnt`).

Immediately before each block is a table of `{u32be offset, u32be index}` pairs.

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

**Script VM opcodes.** The likely interpreter is `sub_821C9FE0` (0x24E0 bytes),
a 67-case computed-goto dispatcher. All 67 case addresses are derived in
`docs/script-vm-notes.md`. The opcode is a **dword at `a1+4`, 1-based**, not
a raw stream byte, so a fetch/decode step sits in front of it and has not been
found — locate that first, since it defines the real instruction encoding.
Then build a disassembler that walks the image from `+0x18` and run it over all
678 files; consuming every file with no unknown opcode and no desync is a much
stronger signal than eyeballing one file.

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

**Language selection.** How one of the 7 language blocks is chosen is unknown;
there should be an index in the header or in a table preceding the blocks.

**Bulk indexing.** Nothing yet explains how the image addresses into the bulk
section. The `{u32be offset, u32be index}` tables before each text block are the
obvious candidate; confirm the direction of that pairing.

**Untouched formats.** `.x3tex` and `.tex` have not been looked at at all.
`.bop`/`.bmd` have loaders identified but no format documentation.

**Range-coder encoder.** Only needed to keep repacked builds small. Requires
mirroring `sub_8210E0F8`'s two normalisation loops exactly and requantising
symbol counts into one byte each under the 8204-entry LUT budget without
zeroing any symbol that occurs.

---

## Related documents

- `docs/script-vm-notes.md` — function inventory, opcode dispatch tables,
  ruled-out leads, IDA tooling caveats
- `docs/debug-hooks.md` — devkit gate and debug console hooks
