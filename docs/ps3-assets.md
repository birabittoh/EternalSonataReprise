# PS3 assets on the Xbox 360 executable

The PS3 release (2008) carries more content than the 360 one: two extra
playable characters (`pc011`, `pc012`), new enemies (`ep198`..`ep209`), new
bosses and AI scripts, extra battle maps (`lam9x`, `sbi90`) and extra field
areas. The long term goal is for this recompilation, which runs the PAL 360
executable, to load that content. This document records what differs between
the two releases' data and what `scripts/ps3_convert.py` does about it.

The 360 decoded tree (`scripts/unpack_e.exe` output under `extracted/`) is the
reference throughout: almost every PS3 file has a 360 counterpart, so a
conversion rule is only trusted once converting the PS3 file reproduces the
360 bytes. `ps3_convert.py --verify` runs that comparison.

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
python scripts/unpack_ps3.py assets-ps3-raw/PS3_GAME/USRDIR/archives -o assets-ps3 --case-from assets
```

`--case-from` borrows the 360 tree's spelling where a file exists on both.
Unlike the 360 the PS3 applies no codec: every file is already decoded, which
is the form `ps3_convert.py` and the 360 tools work on.

Then build a game directory and point `game_data_root` in
`eternalsonata.toml` at it:

```bash
python scripts/ps3_convert.py assets-ps3 assets-ps3-360 --base assets
```

The directory starts as hard links to the 360 tree (`--base`, which provides
`default.xex` and every file the PS3 data cannot replace yet), then each
convertible PS3 file is written over its counterpart, PS3-only files are
added, and `index.vmtoc` gets a stored record for every converted file. Files
kept from the 360: `AppKeep.bmd`, `op.bmd`, `ed1.bmd`, `ed2.bmd`,
`campdata/scp.bmd` (slot layouts differ), fonts and `.tex`. Audio needs
`ffmpeg` on `PATH` (§5).

`--verify extracted/e --verify extracted/other` compares converted models
against the decoded 360 release; without an output directory it only
converts and reports.

| PS3 | 360 | Notes |
|---|---|---|
| `.e`, `.bop`, `.bmd` | same | Same containers, same chunk tree; see §3 |
| `.p3tex` | `.x3tex` | Map textures: a bare `NTX3` chain vs `NTX2` |
| `.p3obj` | none | Field character models moved out of `AppKeep.bmd` |
| `.cps` | `.cxs` / `.wav` | Music, PS-ADPCM or PCM; see §5 |
| `.csf` | `.csf` | Same banks, ATRAC3 clips; see §5 |

## 2. Scripts

The `.e` bytecode is produced by the same compiler: `bos01_v1.e` differs only
in its header id and timestamp. Of every native id imported by any PS3 script,
only seven are missing from the 360 executable's tables:

| id | used by |
|---|---|
| 5026, 5032 | `lib.e` |
| 5027 | `Tnt03.e` |
| 5029 | `Bel01.e` |
| 5030, 5031 | `sbi02.e`..`sbi05.e` |
| 5033 | `Dld17.e` |

The 5000 range is the party lookup table (`off_8240CA88`, 26 entries on the
360), and the call sites are about the twelve character roster: `sbi02..05`
loop characters 0..11 through 5030 and 5031. `sub_820FF748`'s range check is
inclusive, so an unregistered 5026 silently resolves to the first battle
native. `src/engine/ps3_natives.cpp` registers ids 5026..5033 as host stubs
returning 0 until their real behaviour is known, which needs the PS3
executable decrypted (RPCS3, Utilities > Decrypt PS3 Binaries).

`lib.e` exports nearly the same symbol ids on both releases (block2 table 2):
the PS3 one adds 147, 159, 170, 173, 174 and lacks 149, 165, 171, and no other
PS3 script imports the new ones. How cross file symbols resolve has not been
traced yet, so whether a PS3 map can run against the 360 `lib.e` is open.

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
| `NLEF` / `Mefc` | contains `NTX3` at directory addressed slots | textures only |
| `NATR` | colours at irregular offsets | no |
| `NMR2` | different record layout (28 vs 44 bytes) | no |
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
614,760 on the 360). The DXT payload is
byte identical to the 360 `NTEX`'s, so the conversion writes the same DDS
header the 360 files carry (`flags 0xA1007`, `caps 0x401008`, or `0x81007` /
`0x1000` with one level) and copies the pixels. `NTEX` is `0x88 + size` and
`NTX3` is `0x80 + size` rounded up to 128, so an `NTEX` usually fits with an
`NPAD` filling the rest and nothing after it moves. When it does not, the
model tree grows instead; a texture inside a `Mefc`, which cannot move, is
then left unconverted with a warning.

Map textures (`cfdata/maptex/*.p3tex`) are the external texture list
`CreateModel` receives as its third argument, which accepts `NTEX` as well as
`NTX2`, so they convert to a plain `NTEX` chain served as `.x3tex`.

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

Bit 15 of the flags word (+0x18) is set on every PS3 mesh and on most 360
ones; it selects the synchronous vertex buffer path in `sub_821147B0`, which
the 360 supports, so it is left alone.

### 3.3 Materials and lights

`NMTR` holds 96 byte materials; the colours are at +4, +36, +40 and +44 (+8 is
a flags word). `NLIT` is 48 byte records from +8 with colours at +20 and +28,
`NFOG` has one at +12, `NCLC` at +20 and +36, and `NOL2` 32 byte records with
the colour at +24, or +8 for type 0 records.

## 4. Containers

Converting a skinned mesh grows its chunk, so offsets into the container move:

* `.e`: list B dwords in the image hold bulk relative offsets (to `NOBJ` and
  `BTX ` blobs) and are remapped; the header's total size (+0x0C) and reloc
  offset (+0x14) grow by the same delta. The image itself does not change.
* `.bmd`: the entry table after the 12 byte header holds absolute offsets.
* `.bop`: +0x0C points at a directory (`u32 count`, absolute offsets) of
  `NMTN`, `Mefc` and collision entries.

A resized `NMDL` ends with an `NPAD` that keeps the size change a multiple of
128, so everything after it keeps its PS3 alignment.

`AppKeep.bmd` is addressed by slot, and the PS3 moved the ten field character
`NOBJ`s out of it into the `pc*_v*.p3obj` files, so it cannot replace the 360
one as is.

## 5. Audio

No XMA encoder exists, so audio the 360 does not already have is shipped as
PCM: `pcm/<16 hex digits>.wav` in the game directory. The converter writes a
16 byte tag (`RXPcmSub` plus those eight bytes) at the start of the clip's
payload, and the host (`ScanGamePcm`) substitutes the WAV when the XMA decoder
meets it, the same way mod audio works.

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
adds 29. A clip whose 360 twin at the same ordinal has the same flags and a
length within 3000 samples keeps the 360 `TIM` and XMA; about 1400 MB of PCM
comes down to about 210 MB. The `BOOK`s differ in a few sequencer timing bytes
and the PS3's are kept.

### Music (`.cps`)

`CPS ` header: u32 header size (0x20), channels, data size, rate, loop start
and end in bytes, and a kind: 3 is PS-ADPCM with channels interleaved per 16
byte frame, 0 is big endian PCM. Every track both releases share has the same
sample count, so only names differ: PS3 scripts ask for `MP139.cps`, which the
converter rewrites to `MP139.cxs` in the `.e` files (the same length, and
`sub_820F80F8` builds `sound\cxs\<name>` from whatever the script says).

New PS3 tracks: `MP109_us`, `MP166..168` become a 360 `.cxs` of the nearest
length with a tagged payload plus a PCM sidecar (`smpl` loop); `MP187..189`
and their 5.1 versions `MP197..199` are PCM already and become plain big
endian `.wav`, which scripts ask for by that name.

## 6. Not done yet

* `NMR2` and the colours in `NATR`.
* `Mefc` effects: only their textures are converted.
* Real implementations of the seven stubbed natives, and cross file script
  symbols (§2).
* Slot addressed containers (`AppKeep.bmd`, `title.bmd`) and how the PS3 loads
  `.p3obj`.
* Runtime validation: none of the converted files has been loaded in game yet.
