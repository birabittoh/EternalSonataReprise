# Making mods

NocturneRecomp mods are folders under `mods/`, layered over the game's data
and, optionally, shipping native code. Two kinds of content can go in a mod,
and a single mod can mix both:

- **Asset replacement**: swap a single line of text, a single texture, or a
  single mesh, without repacking the container it ships in (see
  [Replacing single assets](#replacing-single-assets)); or, at the low level,
  swap a whole game file by mirroring the game's own directory layout (see
  [Whole-file asset replacement](#whole-file-asset-replacement) at the end).
- **Code**: a native DLL that hooks into the app lifecycle (register ImGui
  overlays, keybinds, read guest memory, etc.), via the SDK's mod-plugin ABI.

Mods are enabled in priority order by the `[[mods]]` list in
`mods/mods.toml` (each entry an `id`/`enabled` pair); earlier entries win on
conflicting files.

Mod **source** and build tooling live in a separate repo,
[birabittoh/NocturneRecomp-Mods](https://github.com/birabittoh/NocturneRecomp-Mods)
(`src/<name>/` there), not here -- this repo only ever contains the
built/shipped `mods/<name>/` folders a player enables. Grab prebuilt mods
from that repo's releases (one zip per mod, with binaries for all three
platforms), or clone it to develop a new one; this doc otherwise describes
the mod-plugin ABI and `mod.toml` format that repo's mods target.

## Replacing single assets

Eternal Sonata keeps almost nothing in a file of its own. Dialogue, textures
and meshes all live inside `.e` (and `.bmd`) containers that hold hundreds of
unrelated assets at once, so replacing one line of dialogue with a whole-file
overlay means redistributing every texture, mesh and script that happens to
share that container: megabytes of other people's copyrighted data, shipped to
change one sentence. Two mods that each want to change one string in the same
container also can't coexist, because only one of their files can win.

So don't do that. A mod ships **only the assets it authored**, as loose files
under `assets/`, and the host splices them into the container as the game
loads it:

```
mods/<name>/
  assets/
    cfdata/adg01.e/
      text/ITA/17.txt                 one string, one language
      textures/face_alg.tga.png       one texture, by its name in the container
      meshes/head.nshp                one native mesh chunk, by embedded name
      skeletons/0.nbn2                its skeleton, by ordinal
      animations/walk.nmtn            one animation, by embedded name
    map/nyaza.e/
      textures/3.png                  by ordinal, when a chunk has no name
    text/ITA.csv                      a whole translation, any container
  mod.toml
  icon.png
```

The game's installed files and loader are untouched. The host decodes the
container (see
[asset-formats.md](asset-formats.md) §2), splices in every patch, and serves
the result into a generation-keyed cache under the user data directory. It
always writes that generation's matching `index.vmtoc` record with the codec
set to stored and the size set to the new decoded length. The loader sizes its
allocation from that record, so the two are never served apart.

### Finding what to replace

Every asset has a **reference**, which is the guest path plus what to address
inside it:

```
cfdata/adg01.e#text:USA/17        string id 17 in the USA language block
cfdata/adg01.e#text:1/USA/17      same, in the file's second BTX blob
e0020_020.e#tex:face_alg.tga      texture chunk by its embedded name
e0020_020.e#tex:3                 fourth texture chunk, in file order
map/nyaza.e#mesh:head             mesh (NSHP chunk) by name
map/nyaza.e#mesh:2                third mesh, in file order
e0020_020.e#skeleton:0            first NBN2 skeleton
e0020_020.e#animation:walk        NMTN animation by embedded name
cfdata/e0020_050.e#lipsync:0      first embedded lip instruction track
```

The guest path is the path as it appears in `index.vmtoc`: lowercase,
`/`-separated, no `game:\` prefix. Prefer names over ordinals wherever a name
exists; an ordinal shifts if the container ever changes, a name doesn't.

The planned `es_asset.py` tooling will list and extract them with:

```bash
python scripts/es_asset.py list "extracted/e/cfdata/adg01.e"
python scripts/es_asset.py list --kind text --lang ITA "extracted/e/cfdata/*.e"
python scripts/es_asset.py extract "cfdata/adg01.e#tex:face_alg.tga" -o face.png
```

`extract` is planned to write the shipped asset out in the same format the
replacement goes back in, so the intended round trip is: extract, edit, drop
the result into your mod under the matching name. The planned in-game asset
browser will do the same thing for assets loaded by the current area, with a
Reload button that re-reads `assets/`.

### The folder layout in full

The path under `assets/` *is* the reference, spelled as directories:

| Reference | File |
|---|---|
| `cfdata/adg01.e#text:ITA/17` | `assets/cfdata/adg01.e/text/ITA/17.txt` |
| `cfdata/adg01.e#text:1/ITA/17` | `assets/cfdata/adg01.e/text/1/ITA/17.txt` |
| `e0020_020.e#tex:face_alg.tga` | `assets/e0020_020.e/textures/face_alg.tga.png` |
| `map/nyaza.e#mesh:head` | `assets/map/nyaza.e/meshes/head.nshp` |
| `e0020_020.e#skeleton:0` | `assets/e0020_020.e/skeletons/0.nbn2` |
| `e0020_020.e#animation:walk` | `assets/e0020_020.e/animations/walk.nmtn` |
| `sound/cxs/bgm042.cxs#music` | `assets/sound/cxs/bgm042.cxs/music.wav` |
| `sound/spc001.csf#sfx:7` | `assets/sound/spc001.csf/sfx/7.wav` |
| `cfdata/e0020_050.e#lipsync:0` | `assets/cfdata/e0020_050.e/lipsync/0.csv` |
| `default.xex#text:2/ESP/137` | `assets/default.xex/text/2/ESP/137.txt` |
| whole file `sound/vo/field01.wav` | `assets/sound/vo/field01.wav` |

Language folders are the game's own fourccs without the trailing space: `JPN`,
`USA`, `GBR`, `FRA`, `ITA`, `DEU`, `ESP`. A folder named `ALL` applies to every
language, which is what a mod shipping a single translation usually wants.

Textures accept `.png` or an uncompressed 32-bit `.dds` (export a compressed
one as `.png` instead). Model graph replacements use complete native `.nshp`,
`.nbn2`, and `.nmtn` chunks. A whole-file replacement is just the file itself
with no `<container>/<kind>/`
folder in the path, which is the same thing the `game/` overlay does, kept here
so a mod doesn't need two trees.

For a translation, one file per string gets old fast, so a whole language can
go in one table instead:

```csv
# mods/<name>/assets/text/ITA.csv
file,blob,id,text
cfdata/adg01.e,0,17,"Che cosa stai facendo qui?"
cfdata/adg01.e,0,18,"Niente di importante."
btldata/script/tutorial/t0001.e,0,4,"Premi A per attaccare."
```

`blob` is almost always `0` and may be left empty. The text is the game's own
single-byte encoding, not UTF-8 (see [Text encoding](#text-encoding) below);
The planned `es_asset.py` validator will check these tables before shipping.

### Size rules

By default a patch must fit the space the original occupies, and the remainder
is padded, so nothing after it moves and nothing else in the container can be
disturbed. Text gets extra room for free here: identical strings within a
language block are deduplicated to share one copy, which the game's reader
cannot tell apart from the original layout.

When that isn't enough, `allow_resize` rebuilds the container and fixes up the
`.e` relocation tables for the shift (the mechanism is documented in
[asset-formats.md](asset-formats.md) §3.4.2, including the crash it avoids).
It's set per patch in an optional `assets.toml` next to the tree:

```toml
# mods/<name>/assets.toml
[defaults]
allow_resize = false

["cfdata/adg01.e#text:ITA/17"]
allow_resize = true
```

Resizing is well tested for text and is what a translation normally needs. It
does not apply to textures at all: a texture is always spliced into the pixel
region the shipped chunk already owns, so the replacement has to carry the
original's dimensions and the container never changes length. Model chunks
often change size, so NSHP, NBN2, and NMTN replacement usually implies
`allow_resize`.

### What a replacement may and may not change

Textures are re-encoded to whatever format the original chunk used (DXT1, DXT3
or DXT5), mipped, and tiled for the Xbox 360 by the host. The replacement must
match the original's dimensions exactly, because the chunk's fetch constant, its
mip chain's layout and the size of its pixel region are one consistent set that
a splice cannot rewrite. Use `#tex:` enumeration or the studio's viewer to find
out what a chunk's dimensions are.

Two kinds of chunk are refused rather than guessed at: a texture whose mip
layout the host cannot reproduce (three chunks in the whole game, all
non-power-of-two), and the handful of chunks that are not DXT. Both are logged
naming the mod and the reference.

Models are patched as a graph. Geometry is an NSHP chunk, its skeleton is an
NBN2 chunk, and its animations are NMTN chunks. All three may grow or shrink:
the host updates enclosing NOBJ and NMDL sizes, repairs `.e` relocations, then
serves the final container with a matching stored `index.vmtoc` record.

```text
mods/<name>/assets/
  e0020_020.e/meshes/body.nshp
  e0020_020.e/skeletons/0.nbn2
  e0020_020.e/animations/walk.nmtn
```

Native chunk files include their eight-byte magic and size header. The runtime
deliberately consumes the exact NSHP, NBN2, and NMTN encodings the game uses;
the studio documents and exports the decoded model graph for authoring tools.

Model changes are constrained by the rest of the container:

- An isolated NSHP replacement keeps its original local bone list, so its bone
  indices must remain within that list. To introduce bones, replace the NSHP,
  NBN2, and affected NMTN chunks together.
- Every face section's material id must be one the container already declares.
  A replacement can't introduce a new material or texture slot.
- Vertex, index, bone, track, and keyframe counts are otherwise free when
  `allow_resize` is enabled.

### Music, voices, and sound effects

Music (`.cxs`, one file per track) and sound and voice banks (`.csf`, many
clips per file) both hold raw XMA2, the Xbox 360's codec, and no open encoder
for it exists. So audio is the one kind that is **not** re-encoded and spliced
into its container. The host already decodes XMA to play it, and it substitutes
your audio there instead:

```
mods/<name>/assets/
  sound/cxs/bgm042.cxs/music.wav     replaces one music track
  sound/spc001.csf/sfx/7.wav         replaces effect or voice clip 7
  sound/vo/field01.wav               a plain PCM .wav, replaced whole
```

Music uses the selector `#music`. Both voices and sound effects use `#sfx:N`,
where `N` is the zero-based TIM clip ordinal inside the CSF bank. Replacing a
voice therefore uses exactly the same folder shape as replacing an effect. Use
the asset enumeration API or Eternal Sonata Studio's CSF viewer to identify the
ordinal.

Ship an ordinary little-endian, uncompressed, 16-bit PCM WAV at any sample rate
and channel count. The host converts its sample rate and channel layout to the
XMA stream requested by the game. Compressed WAV, Ogg, FLAC, and MP3 input are
not currently accepted. You never touch XMA and you never need the XDK.

The host tags the selected XMA payload without changing its size or metadata.
Audio replacement never resizes anything and can't collide with a text or
texture patch in the same file. Replacing one effect in a bank of two hundred
leaves the other 199 playing as shipped.

Two things follow from substituting rather than splicing:

- **Music length is free, voice length isn't.** A `.cxs` track can be any
  length. By default it uses the shipped track's sample-frame loop points, which
  47 of the 62 retail tracks carry. A WAV `smpl` loop overrides them. A voice
  clip that a text box waits on through the `<wv>` tag should match the
  original's duration: longer audio is cut off when the game moves on, not
  waited for. Shorter audio ends early.
- **The `.wav` files under `assets/sound` are a separate, easier case.** They
  are plain big-endian PCM rather than XMA, so they are replaced as whole
  files with a byte swap and no decoder involvement at all. Drop a normal
  little-endian 16-bit PCM WAV at the same guest path and the host swaps its
  RIFF fields and samples.

The equivalent runtime calls are `EternalSonataReplaceAudio` for interleaved
signed 16-bit PCM already in memory and `EternalSonataReplaceAudioFromFile` for
a WAV on disk. Registering a runtime patch follows the same lifecycle as the
other asset calls: invalidate the guest path when an already-running asset
generation must be rebuilt.

### Lip sync instructions

Voice containers can carry `LIP ` chunks beside their CSF audio. Each chunk is
an ordered list of a mouth shape and its duration. Address one as
`#lipsync:N`, where `N` is the zero-based LIP chunk ordinal in the container,
and place an editable CSV at the matching path:

```text
mods/<name>/assets/cfdata/e0020_050.e/lipsync/0.csv
```

```csv
phoneme,duration
1,5
2,3
0,2
5,4
```

The phoneme values are `0` neutral, `1` A or ah, `2` E or ee, `3` O or oh,
`4` U or oo, and `5` M or mm. Duration is the game's timing unit and must be
between 1 and 255. Blank lines, the header shown above, and lines beginning
with `#` are ignored.

Lip sync replacement is independent of voice replacement. A mod can replace
either one or both, and separate mods can replace them without shipping the
surrounding container. The instructions are rewritten in place, so the CSV may
contain at most as many events as the original LIP chunk has room for. The
asset enumeration API reports each `#lipsync:` reference and its original
event count. Lip sync does not support `allow_resize` because growing a LIP
chunk would also require rebuilding its enclosing CSF and CSL offset tables.

Runtime mods can pass `EternalSonataLipEvent` entries to
`EternalSonataReplaceLipSync`, or load the same CSV format through
`EternalSonataReplaceLipSyncFromFile`.

### Text encoding

The game's font draws **one glyph per byte**: text is single-byte CP1252 /
Latin-1, not UTF-8, so `é` written as UTF-8 renders as two garbled glyphs.
`.txt` and `.csv` files under `assets/text*` are read as UTF-8 and transcoded
for you, and `es_asset.py` fails loudly on a character with no single-byte
equivalent rather than emitting mojibake. If you write bytes directly through
the C API you get no such help; write `"\xE9"`.

A newline inside a string is the literal two-character sequence `\` `n`, not
`0x0A`. Markup tags (`<w>`, `<w1500>`, `<c Allegretto>`, …) pass through
untouched; [asset-formats.md](asset-formats.md) §3.5 lists the set. `JPN ` is
Shift-JIS; the six western blocks are single-byte.

### The game's own menu chrome (`default.xex`)

"Player Controls", "Player 1", "Next", "ON"/"OFF" and about 2850 other strings
per language are not in any `.e` file, which is why translating every container
still leaves the menus reading in the original language. They are baked into
the executable image, but in **23 ordinary `BTX ` blobs**, the same format
every container uses, so they are patched the same way:

```
mods/<name>/assets/default.xex/text/2/ESP/137.txt      -> "Controles del jugador"
```

`default.xex` is a container name like any other. The blob index is required
here (it is not almost always `0` the way it is in a container), and the string
ids are the game's own, shared across all seven languages. Blob `2` is the main
menu chrome; use `EternalSonataEnumerateAssets` to list the rest.

Two differences from a container patch, both consequences of the blob living
inside the executable rather than in a file:

- **`allow_resize` does nothing here and the patch can never grow the blob.**
  It is pinned in the image with unrelated data on both sides. What must fit is
  not the individual string but *the whole language block*: rewrite all of a
  block's strings and the total just has to come in under the original total,
  and identical strings are automatically stored once to buy a little more
  room. A patch that does not fit is dropped with a `TOO_LARGE` warning naming
  your mod and the string, and the rest of your patches still apply.
- **Nothing is written to disk.** Container patches are materialised into the
  asset cache; these are written straight into guest memory at launch. There is
  no patched file to diff, so the log line is the confirmation.

Spanish and German are the wordiest shipped languages, which makes them the
most comfortable donor slots for a translation that needs the room.

### Adding a new language

A translation mod can add a language of its own. It appears in the F4 settings
overlay's Language row and in the game's own Options screen Text row, both from
one registration, and every text patch above can then target it.

The catch the game imposes: BTX has a **fixed table of seven language blocks**
(`JPN USA GBR FRA ITA DEU ESP `) and the guest picks its block by an index
latched at boot, not by name. There is no eighth block to add. So a new language
**piggybacks on an existing one**: it claims a `slot`, the host boots the guest
into that slot's language so it reads the block your mod patched, and both menus
show your label in place of the donor's. `ESP ` and `DEU ` are the conventional
donors. While your mod is enabled the donor language is not selectable, and two
translation mods claiming the same slot conflict (first in `mods.toml` wins, the
loser is named in a warning and keeps all of its non-text patches).

A pure translation needs no C++ at all. One `[[language]]` block in
`assets.toml`, plus text files:

```toml
# mods/<name>/assets.toml
[[language]]
id = 9              # any XLanguage id the built-in five (1, 3, 4, 5, 6) don't use
label = "Portugues" # shown in the F4 overlay's Language combo
code = "PT"         # the two letters the native Options screen's Text row draws
slot = "ESP"        # the BTX block this language's text lives in
```

```
mods/<name>/assets/cfdata/adg01.e/text/PT/1234.txt
mods/<name>/assets/text/PT.csv
```

The folder is named after your `code`, and the host routes it to the donor block
for you. Everything in [Size rules](#size-rules) and
[Text encoding](#text-encoding) applies unchanged.

Text the *host* authors rather than the game is translated by a
`[language.strings]` table under the `[[language]]` it belongs to. That covers
the option rows this project adds to the game's own screens, the achievements
this project ships (`achv_name_<id>`, `achv_desc_<id>`,
`achv_desc_locked_<id>`), and achievements another mod added, which are keyed by
that mod's name instead of by an id (see
[Adding achievements](#adding-achievements)):

```toml
[language.strings]
resolution_label = "Spyglass Size"
achv_name_1 = "First Blood"
achv_name_shutterbug_five_photos = "Fotoreporter"
```

Achievement strings reach both the ImGui overlay and the status menu's
Achievements screen; the latter is drawn with the guest font, so it folds them
to Latin-1 and anything outside that draws as `?`.

To do the same from a code mod, publish three mod-registry events from
`OnCreateDialogs` (see `src/settings.h`). `settings.language_option` adds the
entry (`payload.u64` is the id, `payload.bytes` is `"Label"` or
`"Label|CODE|SLOT"`), `settings.language_slot` claims the block on its own, and
`settings.native_string` translates one of the rows this project synthesises
into the game's Options screen (`payload.u64` is the id, `payload.bytes` is
UTF-8 `"key=value"`). The keys are `resolution_label`, `framerate_label`,
`text_label` and `overworld_model_label`; rows you registered yourself translate
through `EternalSonataSetOptionRowLabel` instead, whose `language` argument is
an index into the same list your language just joined.

Three limits worth knowing before you start:

* **The font is the game's, and it is fixed.** One glyph per byte out of a
  shipped atlas, so a language needing glyphs the atlas lacks renders as
  mojibake no matter how correct the text is. Check yours before translating
  anything at length.
* **Keep labels ASCII.** They are drawn by the guest, under the same one-byte
  rule.
* **Language changes need a restart.** `user_language` is read once at guest
  boot, so both menus mark the row accordingly.

The list holds nine languages in total, five built in and four added, which is
where the Options screen's Text row runs out of width for its two-letter codes.
Registrations past that are dropped with a warning.

### Adding a new voice language

The voice half of the same idea, and deliberately **not** the same list. The
game ships two spoken languages, Japanese and English, and a mod can add a
third; it appears as an extra value on the game's own Voice row, so Text and
Voice stay independently selectable. Mod text with Japanese voice, or stock
English text with a mod voice, both work.

Where BTX forced a text language to borrow a donor block, voice has no such
scarcity. Voice banks are `.csf` files under `btldata\voice\` with **no language
directory and no language field**: the language is a filename suffix, bare for
Japanese and `_usa` for English. So a voice language simply gets a suffix of its
own, both shipped languages stay selectable, and two voice mods never collide.

```toml
# mods/<name>/assets.toml
[[voice_language]]
id = "ptbr"         # what the voice_language cvar stores
label = "Portugues" # drawn on the Voice row, next to the game's own two values
code = "PT"         # two-letter form, for menus too narrow for the label
suffix = "_ptbr"    # the bank filename suffix; defaults to the id
donor = "usa"       # "usa" or "jpn": which bank your clips are timed against
```

```
mods/<name>/assets/btldata/voice/pc001.csf/sfx/3.wav
```

Address the **shipped** bank, exactly as any other audio patch does, because that is
the container whose clip numbering your selectors mean. The host builds
`pc001_ptbr.csf` from your `donor`'s framing, tags the clips you replaced, adds
its `index.vmtoc` record, and serves it under your suffix. The 18 JP/USA pairs
all have identical clip counts, so ordinal `3` is the same line whichever donor
you name.

Five things to know:

* **A clip you did not replace plays in the donor's language.** The donor's own
  XMA2 is left in place rather than blanked, so a partial voice mod is a mix,
  not silence.
* **`.csf` is XMA2 and there is no open encoder**, which is why the bank is
  synthesised rather than shipped. Your clips are 16-bit PCM WAV, same as every
  other audio patch.
* **Duration still matters.** A line the game times a text box against (the
  `<wv>` markup tag, 568 uses) is cut off at the original's length, because the
  game advances on its own schedule.
* **The suffix has at most seven letters.** An `index.vmtoc` path record is 32
  bytes, and `btldata\voice\bosfga` + suffix + `.csf` has to fit. A longer one is
  refused at registration with a warning naming your mod.
* **Voice changes need a restart**, for a sharper reason than text does: the
  guest caches loaded banks keyed on its own selector byte, which a mod voice
  language leaves at the donor's value. Two mod voice languages are
  indistinguishable to that cache, so a live switch would replay the bank
  already in it.

Not every bank has an English twin (27 of the 45 are Japanese-only), so
`donor = "usa"` falls back to the bare name for those, exactly as the game's own
probe does. To do all of this from a code mod instead, publish
`settings.voice_language_option` with `payload.bytes` set to
`"id|Label|CODE|SUFFIX"`.

### Layering and conflicts

There is **one** patched image per container, built from every enabled mod at
once. The host does not pick a winning mod and use its version of the file: it
collects the patches from all of them, keyed by reference, and splices them all
into a single rebuild. Two mods that each change a different string in
`cfdata/adg01.e` both take effect, and neither has to know the other exists.
This is the whole point of the feature, and it is what the `game/` overlay
cannot do.

The rules, in order:

1. **Distinct references all apply.** Different strings, different texture
   chunks, different meshes, even in the same file, compose with no conflict
   and in any mod order.
2. **The same reference is resolved by priority.** Whichever mod is earlier in
   `mods.toml` wins. The loser's patch is dropped, not merged: a texture or a
   mesh is a whole-chunk replacement and there is no meaningful way to blend
   two of them. The drop is logged at WARN naming both mods and the reference,
   the F1 mod manager flags it, and the C API returns
   `ETERNALSONATA_ASSET_CONFLICT` to the losing caller rather than silently
   swallowing it.
3. **`force` overrides priority.** A patch with `force = true` in `assets.toml`
   (or `ETERNALSONATA_ASSET_FORCE` from code) beats an earlier mod's patch for
   the same reference. Two forcing mods fall back to rule 2 between
   themselves. This is for a compatibility patch that exists precisely to
   override another mod, not a way to opt out of load order.
4. **A whole-file replacement becomes the base.** If a mod ships the container
   itself under `game/`, that file replaces the shipped one (by rule 2 among
   whole-file overlays), and every mod's granular patches are then applied on
   top of it, including the replacing mod's own. A reference that no longer
   resolves against the new file is reported as
   `ETERNALSONATA_ASSET_NOT_FOUND` naming the mod that registered it, which is
   the usual sign that a whole-file mod and a granular mod disagree about what
   the container holds.
5. **Splicing order is by file offset, not by mod order.** All the surviving
   patches for a container are applied in one pass in ascending offset, so
   several resizing patches in the same file compose and offsets are recomputed
   once. Mod order decides *which* patch applies, never *where* the bytes land,
   so the result does not depend on load order beyond rule 2.

One shared budget is worth knowing about: under the default preserve-size rule
the free room in a BTX language block comes from deduplicating identical
strings *in that block*, so several mods growing strings in the same block are
spending the same pool. When it runs out, the patches that did not fit are
reported individually (`ETERNALSONATA_ASSET_TOO_LARGE`, with the mod and
reference named) instead of one of them corrupting the block. Setting
`allow_resize` on those patches removes the limit; it is set per patch, so one
mod opting into a rebuild does not force the cost or the risk on the others.

### Doing it from code

A mod that decides at runtime what to replace (a translation that follows the
language setting, a texture built from the player's own files, a randomiser)
uses the C ABI instead. Copy `src/eternalsonata_asset_api.h` from this repo
into your mod for the signatures and the full contract; the entry points
resolve out of the host executable exactly as with the Options and party APIs:

```cpp
#include "eternalsonata_asset_api.h"

auto set_text = reinterpret_cast<EternalSonataSetTextFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSetText"));
if (set_text) {
  set_text("cfdata/adg01.e#text:ITA/17", "Nuova battuta", 0);
}
```

The same ABI exposes structured mesh replacement through
`EternalSonataReplaceMesh`, full fidelity `.nshp` replacement through
`EternalSonataReplaceMeshFromFile`, native NBN2 skeleton and NMTN animation
replacement through their memory and file variants, and decoded whole-file
replacement through `EternalSonataReplaceFile`. These calls remain ABI version
1 because the project has not released this API yet.

Things worth knowing before you use it:

- **Always null-check the `GetProcAddress` result**, and call
  `EternalSonataAssetAbiVersion()` if you need to branch on host capability.
- **Register before the startup cache build.** A patch registered afterward
  applies when `EternalSonataInvalidateAsset()` rebuilds and remounts the
  served cache. In the current implementation any invalidation rebuilds the
  complete cache generation, even when a guest path is supplied.
- **Register lazily for anything large.** Pushing thousands of patches up front
  to cover text the player may never reach is wasteful;
  `EternalSonataRegisterAssetProvider()` calls you once per container, at the
  moment the host is about to build its patched image, and you register only
  what that container needs. The `"eternalsonata.asset.loading"` event on the
  shared registry bus is the same point without the header.
- **Providers and asset events run on the cache-building thread.** They must
  be thread-safe, must not touch ImGui, and must not block startup or an
  explicit rebuild.
- **`EternalSonataEnumerateAssets()` decodes every file it touches.** It's a
  browse call for tooling and startup scans, not something to run per frame.
- **Nothing here runs guest code**, so no call is queued and none can be
  refused for game state. Patches take effect after the served cache is built.

### Textures the hash path already covers

The SDK's content-hash texture replacement (`mods/<name>/textures/<hash16>.png`,
described under [Whole-file asset replacement](#whole-file-asset-replacement)
below) is independent of all of this and needs no container knowledge at all:
it replaces any texture the game uploads to the GPU, wherever it came from. If
you already have a hash from a dump and only care about how the texture looks
on screen, that path is simpler and stays the recommended one. Reach for
`#tex:` references when you want to address a texture by the name it carries in
the container rather than by a hash you had to dump first, or when the texture
is one the game reads but never uploads.

See the `NocturneRecomp-Mods` repo's `src/` directory for some working examples.

## Code mods

A code mod adds a `code = "<stem>"` key to `mod.toml` and ships a built DLL
at `mods/<name>/code/<stem>.dll`. At startup the SDK loads that DLL through a
versioned C ABI (`rex::system::IModPlugin`) and calls its lifecycle hooks
alongside the game's own overlays.

The project ships two builds: vanilla and title-update (TU), which relocates
the whole image and shifts every guest address, and a single mod DLL has to
work with both. The best way is to avoid hardcoded addresses entirely and
read guest state generically, like `memory_peek` does. When a mod genuinely
needs a specific known address (e.g. poking a particular game setting, like
`ui_color` does), don't hardcode it or re-derive the vanilla/TU split
yourself: look it up by name from the SDK's shared mod registry instead. See
[Library mods and the shared registry](#library-mods-and-the-shared-registry)
below.

### 1. Scaffold the mod under `NocturneRecomp-Mods`'s `src/`

Clone [birabittoh/NocturneRecomp-Mods](https://github.com/birabittoh/NocturneRecomp-Mods);
mod **source** lives in `src/<name>/` there, separate from the built/shipped
`mods/<name>/` folder (which only exists locally after a build, or here in
this repo once you've copied a built mod over). Copy an existing mod as a
template: `src/sample_overlay/` is the minimal one:

```
src/sample_overlay/
  CMakeLists.txt
  mod_main.cpp
  mod.toml
  icon.png
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(sample_overlay LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(sample_overlay
    mod_main.cpp
)
```

`rexmod_add_plugin` (from `src/common/mod_cmake/rexmod.cmake`) builds a
shared library, sets C++23, and links `rex::runtime`, the same shared SDK
runtime the game exe links, so your mod shares its ImGui drawer, keybind
registry, and kernel state rather than getting its own copy.

`mod.toml`:

```toml
manifest_version = 1
name = "Sample Overlay"
version = "1.0.0"
description = "Minimal code-mod template: a keybind (F9) and a tiny ImGui overlay."
code = "sample_overlay"
platform = ""
```

`code` must match the CMake target name (and therefore the built DLL's stem).
Everything else is display metadata shown in the F1 mod manager overlay.

`platform` is *written by* `NocturneRecomp-Mods`'s `scripts/make_mods.py`,
not read by it; leave it empty in a fresh mod.toml. After a successful build
it's (re)set to a comma-separated list of whichever platform(s)
`mods/<name>/code/` currently ships a binary for (e.g. `"windows-x64"` after
a `--target windows-x64`-only build,
`"windows-x64,linux-x64,linux-arm64,mac-x64,mac-arm64,android-arm64"` once
every platform has been built into the same tree). It's purely a record of
what's actually on disk, not something you set by hand.

### 2. Implement the plugin ABI

`mod_main.cpp` exports two `extern "C"` functions and returns an
`IModPlugin` subclass:

```cpp
#include <rex/system/mod_plugin.h>

class MyMod : public rex::system::IModPlugin {
 public:
  // Called once, right after the ImGui drawer/overlay stack exists.
  // Register overlays/keybinds here.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}

  // Called once KernelState is fully live (guest module about to launch).
  // Use this for anything needing kernel apps/memory (e.g. filesystem scans).
  void OnModuleLaunched() override {}

  // Called before the host shuts down. Release resources here.
  void OnShutdown() override {}
};

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new MyMod();
}
```

All three `IModPlugin` overrides are optional (no-op by default); implement
only what you use. `ModHostContext` (passed to `rex_mod_create`) gives you
`runtime`, `app_context`, `window`, and `input_system` pointers, plus
`mod_root`/`mod_name` for loading your own bundled assets. Everything else,
including registering an overlay, binding a key, and reading guest memory, goes through
the same public SDK headers the base game uses (`rex/ui/imgui_dialog.h`,
`rex/ui/keybinds.h`, `rex/system/xmemory.h`, etc.).

The SDK never unloads a mod's DLL once loaded (guest threads may still be
running plugin code at shutdown), so don't rely on static destructors
running at process exit; use `OnShutdown()` instead.

### Example mods to copy from

- **`src/sample_overlay/`**: smallest possible template: one keybind
  (F9), one ImGui window. Start here for a new mod.
- **`src/memory_peek/`**: reads guest memory via
  `runtime->memory()->TranslateVirtual()` for a user-entered address (F10).
  A good reference for anything that inspects live guest state generically
  (no hardcoded addresses).
- **`src/music_player/`**: a full-featured example: owns a persistent
  singleton (`GetAudioPlayer()`), binds in `OnCreateDialogs`, and uses
  `OnModuleLaunched()` to scan the filesystem once KernelState exists.
- **`src/game_symbols/`**: a *library mod* with no UI of its own;
  publishes reverse-engineered guest addresses into the shared mod registry
  for other mods to depend on. See
  [Library mods and the shared registry](#library-mods-and-the-shared-registry).
- **`src/ui_color/`**: consumes `game_symbols`'s published address
  (`requires = "game_symbols >= 1.0.0"` in its `mod.toml`) instead of
  hardcoding or re-deriving it.
- **`src/event_ping/`** and **`src/event_pong/`**: a
  producer/consumer pair over the shared registry's event bus rather than
  addresses. `event_ping` has no UI: it uses `RegisterTick` to publish a
  `"sample.ping"` event once a second. `event_pong` (F11) declares
  `requires = "event_ping"`, subscribes to that event, and shows the last
  ping in an overlay -- it also republishes a couple of counters to
  `src/blackboard` with no `requires` on it at all, showing that
  Publish/Subscribe coupling can be looser than the `RegisterAddress`
  pattern.
- **`src/blackboard/`**: a shared key/value store (F12) any mod can
  write to purely by publishing `"blackboard.set"`/`"blackboard.delete"`/
  `"blackboard.clear"` events (bytes = `"key=value"` or `"key"`) -- no header
  or linked symbol needed, so even a binary-only third-party mod can
  participate.
- **`src/bus_inspector/`** (F5): subscribes to the events above and
  logs every one it sees. Since it piles a second/third subscriber onto
  event names `event_pong` and `blackboard` already subscribe to, it
  demonstrates that `Subscribe` supports fan-out to multiple listeners
  rather than last-one-wins.
- **`src/xex_patch_potion/`** and **`src/xex_patch_red_rust/`**:
  two independent, no-UI mods that each patch a different item
  name/description baked into `default.xex`'s static data directly in
  guest memory at startup, and coexist with no conflict. See
  [Patching static game text/data](#patching-static-game-textdata) below,
  and `src/common/include/rexmod/text_patch.h` for the shared helper
  both of them call instead of duplicating the read-only-page-unlock/
  zero-fill logic.

### 3. Build it

```
python scripts/make_mods.py
```

run from `NocturneRecomp-Mods`, not this repo. It configures and builds
every `src/<name>/` project and assembles the result into
`mods/<name>/code/<platform>/` (`<name>.dll` / `lib<name>.so`, plus
`mod.toml` and `icon.png` at the mod root). See that repo's README and the
script's own `--help`/docstring for flags (`--mod`, `--target
{windows-x64,linux-x64,linux-arm64,mac-x64,mac-arm64,android-arm64}`,
`--package`, `--sdk-dir`) and
cross-build details. Once built, copy `mods/<name>/` into this repo's
`mods/` as-is: `LoadModPlugin` checks `code/<platform>/<stem>...` (matching
the running host) before falling back to a flat `code/<stem>...`, so a mod
folder carrying every platform side by side (as a multi-platform
distribution from that repo's releases does) loads correctly with no
flattening step needed. A locally-built, single-platform mod's flat
`code/<stem>...` still works too.

Prebuilt mods (every platform, already zipped one-per-mod) are
attached to that repo's [releases](https://github.com/birabittoh/NocturneRecomp-Mods/releases)
if you just want to install one rather than build it yourself.

### 4. Enable it

Add a `[[mods]]` entry for the mod's folder name to `mods/mods.toml`. Order
matters: earlier entries take priority when multiple mods touch the same
file:

```toml
[[mods]]
enabled = true
id = 'music_player'

[[mods]]
enabled = true
id = 'sample_overlay'

[[mods]]
enabled = true
id = 'memory_peek'
```

Then run the game (`./nocturnerecomp.exe`) and press **F1** to open the mod
manager overlay; it lists every enabled mod, in load order, with its icon
and a `[code]` badge on mods that loaded a DLL. Check `logs/` if a code mod
doesn't show up loaded; the loader logs the exact reason (missing DLL, ABI
mismatch, missing exports) at startup.

`mod.toml` also supports three optional dependency fields, each a
comma-separated list of other mods' folder names:

```toml
requires   = "game_symbols"     # must be enabled AND loaded before this mod,
                                 # or the game fails to start
load_after = "some_other_mod"   # soft ordering hint: only warns if violated
conflicts  = "legacy_ui_hack"   # hard error if both this mod and any listed
                                 # one are enabled, regardless of order
```

A missing or misordered `requires` (or a violated `conflicts`) fails the game
at startup with a message naming the mods involved and the fix, instead of
silently loading in a broken state -- if the mod also uses the shared
registry (below) to depend on the other mod's data, this is what actually
guarantees that data exists by the time it's looked up. `load_after` only
warns; it doesn't gate startup.

Each `requires` entry can also pin a minimum version of the mod it names:

```toml
requires = "game_symbols >= 1.0.0"
```

The version is checked against the named mod's own `version` key (dotted
numeric, e.g. `1.0.0`; missing trailing components count as `0`, so `1.0` ==
`1.0.0`). This is a **hard failure** at Setup() only if the enabled
`game_symbols` is actually older. If `game_symbols` has no `version` key at
all (or the constraint itself isn't a valid dotted version), the check can't
be verified either way, so it's accepted with a warning rather than blocking
startup -- this keeps mods and dependencies that predate this feature
working unchanged. A bare `requires = "game_symbols"` (no `>=`) stays
unconstrained.

A mod can similarly require a minimum version of NocturneRecomp itself via
`game_version`, independent of any other mod:

```toml
game_version = "1.0.0"   # or, equivalently: game_version = ">= 1.0.0"
```

This is checked against the build's own version (`nocturnerecomp_app.h`'s
`OnPreSetup` sets it from `src/version.generated.h`, derived from the
nearest `vX.Y.Z` git tag at CMake configure time -- see `CMakeLists.txt`)
and is likewise a hard failure only when the build is actually older; if no
tag is reachable, the version falls back to `0.0.0` and the check is
accepted with a warning instead.

## Library mods and the shared registry

`rex::system::ModRegistry`, reached via `runtime->mod_registry()` from any
mod that holds a `Runtime*`, is a small registry for sharing reverse-engineered
addresses (and generic events) between mods, so that work doesn't have to be
redone, or copy-pasted, by every mod that needs it.

```cpp
// Producer: registers a name once, resolved to a vanilla or TU address
// depending on the running image (no is_patched() check needed by callers).
runtime->mod_registry()->RegisterAddress("ui.accent_color", kVanillaAddr, kTuAddr);

// Consumer: looks the address up by name instead of hardcoding it.
if (auto addr = runtime->mod_registry()->FindAddress("ui.accent_color")) {
  // use *addr
}
```

A **library mod** is a mod that only does this: no UI, no `code` consumers of
its own, just registration calls in `OnCreateDialogs`. `src/game_symbols/`
is exactly that: it registers `"ui.accent_color"` (the same struct
`accent_color.cpp` reads) for other mods to depend on. `src/ui_color/`
consumes it:

```toml
# src/ui_color/mod.toml
code = "ui_color"
requires = "game_symbols >= 1.0.0"
```

```cpp
// src/ui_color/mod_main.cpp, OnModuleLaunched (lazy lookup, not eager
// in OnCreateDialogs -- see "Ordering" below)
if (auto addr = runtime_->mod_registry()->FindAddress("ui.accent_color")) {
  addr_ = *addr;
}
```

`requires = "game_symbols"` (see above) is what makes this safe: the SDK
guarantees `game_symbols` is enabled and ordered before `ui_color`, so the
lookup can't silently return nothing because of a config mistake.

`ModRegistry` also has `Subscribe`/`Publish` for generic events (a payload of
a `uint64_t`, a `double`, and a byte span valid only for the duration of the
`Publish` call), and `RegisterTick`/`DispatchTick` for a callback fired once
per guest frame on GPU swap, useful for anything that needs to react every
frame rather than just at startup or launch. Ticks run on the
command-processor thread, not the render/UI thread.

**Ordering**: producers register in `OnCreateDialogs` (dispatched in
`enabled_mods` order, before `OnModuleLaunched`); consumers look up lazily,
on first use, rather than assuming a specific dispatch order themselves.
`requires` is what actually enforces the producer runs first, not the lookup
site.

**Threading**: `DispatchTick` (and therefore anything a tick callback
publishes) runs on the command-processor thread, not the render/UI thread,
and `Publish` invokes every subscriber synchronously on whatever thread
called it. So a `Subscribe` callback must never touch ImGui directly --
instead copy the payload (including the `bytes` span, which is only valid
for the duration of that one `Publish` call) into a mutex-guarded member,
and render from that snapshot in `OnDraw`, which does run on the UI thread.
See `src/event_pong/`, `src/blackboard/`, and
`src/bus_inspector/` for the pattern.

**Adding a Language dropdown entry**: the curated Settings overlay's
Language row (`src/settings.cpp`) is app code, not SDK code, so it isn't
something a mod can reach through `RegisterAddress`/`FindAddress`. Instead
NocturneRecomp itself subscribes, from `OnPostLoadXexImage()` (after
`Runtime` exists but before any mod's `OnCreateDialogs` runs), to a
`"settings.language_option"` event on the shared registry. A mod publishes
one entry per language it wants to add, from its own `OnCreateDialogs`:

```cpp
void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = 9;  // XLanguage id. e.g. 9 --> Portuguese
  const char* label = "Portuguese";
  payload.bytes = {reinterpret_cast<const uint8_t*>(label), std::strlen(label)};
  runtime_->mod_registry()->Publish("settings.language_option", payload);
}
```

The id is whatever the game itself understands for `user_language` (an
`XLanguage` value); publishing an id doesn't make the game render that
language, it only adds the option to the dropdown so a mod that *does* add
the actual translated text/assets can let the player select it. A duplicate
id (already built in, or already published by an earlier-loaded mod) is
dropped with a WARN log, same first-wins rule as `RegisterAddress`.

**Keybind collisions**: `rex::ui::RegisterBind` auto-resolves collisions
rather than silently shadowing one bind. If two mods both default to the
same key, the later-loaded one (lower `enabled_mods` priority) is moved to
the next free key from a small pool (F5-F12, then F13-F24 as overflow),
logged at WARN, and shown with a "moved" badge in the F1 mod manager. If the
pool is exhausted the bind is left on its requested key but flagged as an
unresolved conflict rather than silently colliding. A key the user has
explicitly set (via config or the F1 overlay's click-to-rebind control) is
never auto-moved. Give every mod's bind both a unique name (it doubles as
the backing CVar name) and, by convention, a unique default key -- the
auto-reassignment is a safety net, not a reason to stop picking distinct
defaults.

The F1 mod manager overlay also lists, per mod, the cvars it defines or
overrides (old -> new value) and flags cvars that two mods have set to
different values, so a silent `SetFlagByName` clash is at least visible --
this is detection only; the underlying cvar is still last-write-wins.

**Overlay visibility, the gamepad overlay menu, and window titles**:
`RegisterBind` takes two optional trailing parameters: `is_visible` -- a
`std::function<bool()>` returning whether the thing this bind toggles is
currently shown -- and `window_title`, the exact string your overlay passes to
`ImGui::Begin` (including any `##id` suffix):

```cpp
rex::ui::RegisterBind(
    "bind_sample_overlay", "F9", "Toggle sample overlay",
    [this] { visible_ = !visible_; },
    [this] { return visible_; },
    "Sample##overlay");
```

Passing `is_visible` costs nothing extra and makes your overlay show up, with
its live shown/hidden state, in two places: the F1 mod manager's per-mod
keybind list, and the gamepad-triggered overlay menu (default **Y**, with an
**Insert** keyboard fallback for controller-less testing; both are ordinary
rebindable binds) that lists every overlay -- vanilla and mod -- with
`is_visible` set, grouped by owner, selectable to toggle without touching a
keyboard.

Passing `window_title` as well makes your overlay fully gamepad-navigable
inside the SDK's two input modes (**Gameplay**, where the pad drives the game
as normal, and **UI**, where it drives the overlays -- toggled by the guide
button, with a **Home** keyboard fallback since guide is frequently
intercepted by Steam/the OS before it reaches the game). In UI mode, one
overlay is "active": left stick/D-pad and A drive ImGui's built-in gamepad nav
inside it, B closes it, Y opens/activates the overlay menu, X cycles the
active overlay among all currently-shown ones, right stick moves its window,
and left-trigger + right stick resizes it. Without `window_title` your overlay
can still be *toggled* from the overlay menu, it just can't be focused, moved,
or resized by the gamepad controller (`rex::ui::GamepadUiController`, see
`gamepad_ui.h`) the way the six base-app overlays are. A bind whose effective
key is a gamepad button name (set via the settings overlay, the mod manager's
rebind control, or by editing `nocturnerecomp.toml` directly) is dispatched on
press while in Gameplay mode only -- in UI mode the gamepad controller owns
the pad for navigation instead, so gamepad-keyed binds don't fire there.

**This requires rebuilding every mod, not just ones adopting `is_visible`/
`window_title`.** `RegisterBind` is a regular (mangled, not `extern "C"`)
exported symbol in `rexruntime(rd).dll`; adding a parameter -- even an
optional one with a default value -- changes that mangled name. Old code
compiles against the new header unchanged (the default fills in the missing
argument), but an **already-built** mod DLL that was never recompiled still
imports the old symbol signature, which no longer exists in the new DLL's
export table, and fails to load (not a graceful skip -- an OS-level
missing-entry-point failure). Rebuild every mod (`make_mods.py`, or the direct
`cmake --build` invocation if you're bypassing it for a config match -- see
"Both builds, one DLL" below) any time you update the SDK, whether or not
you're using anything new it added.

## Adding rows to the game's native Options screen

A mod can add its own rows to the in-game Options screen -- the real one, drawn
by the game's own text renderer and navigated with the game's own cursor, not
an ImGui overlay. There is only one Options screen; a row added this way shows
up both from the main menu and from the in-game status screen.

Options is two pages, paged with LB/RB. Page 1 (`ETERNALSONATA_PAGE_GAME`) is
the Options screen proper, where the game's own Subtitles and Voice rows live
alongside this project's Text row; page 2 (`ETERNALSONATA_PAGE_GRAPHICS`) is the
button-configuration screen, which the project uses for graphics settings --
Resolution and Frame Rate.

**A mod row lands on page 2 by default.** Page 1 is reserved for the game's own
settings and is expected to fill up with them.
`EternalSonataSetOptionRowPage(row, ETERNALSONATA_PAGE_GAME)` moves a row over
if you have a reason to.

The entry points are exported from the host executable, not from the SDK, so
you resolve them at runtime rather than linking against anything. Copy
`src/eternalsonata_options_api.h` from this repo into your mod for the
signatures and the full contract, then:

```cpp
#include "eternalsonata_options_api.h"

static const char* kValues[] = {"Off", "On"};
static int  MyGet(void* user)            { return g_my_setting ? 1 : 0; }
static void MySet(int index, void* user) { g_my_setting = (index == 1); }

void MyMod::OnModuleLaunched() {
  auto reg = reinterpret_cast<EternalSonataRegisterOptionRowFn>(
      GetProcAddress(GetModuleHandle(nullptr), "EternalSonataRegisterOptionRow"));
  if (!reg) {
    return;  // older host build -- degrade gracefully, don't fail to load
  }
  int row = reg("My Setting", kValues, 2, &MyGet, &MySet, nullptr);

  auto set_label = reinterpret_cast<EternalSonataSetOptionRowLabelFn>(
      GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSetOptionRowLabel"));
  if (set_label && row >= 0) {
    set_label(row, ETERNALSONATA_LANG_IT, "Mia impostazione");
  }

  // Optional: put the row on the game page instead of the graphics one.
  auto set_page = reinterpret_cast<EternalSonataSetOptionRowPageFn>(
      GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSetOptionRowPage"));
  if (set_page && row >= 0) {
    set_page(row, ETERNALSONATA_PAGE_GAME);
  }
}
```

Things worth knowing before you use it:

- **Always null-check the `GetProcAddress` result.** A mod built against a
  newer host has to keep loading on an older one. Call
  `EternalSonataOptionsAbiVersion()` if you need to branch on host capability.
- **Register from `OnModuleLaunched()`.** Rows registered later still appear,
  but only the next time the screen is built -- not on a screen already open.
- **Room is finite, and it is per page: 8 rows on page 1, 7 on page 2.** That
  is the game's limit, not an arbitrary one: a selectable group's item array is
  pre-allocated with 10 slots, of which page 1's stock Subtitles and Voice rows
  take 2 and page 2's three button rows take 3. Registration past that is
  refused and logged rather than corrupting the menu. The project's own rows
  take two of page 2's seven (Resolution, Frame Rate) and one of page 1's eight
  (Text).
- **Values are drawn side by side on one line, so keep them short.** They share
  the width between the row's value column and the end of the row rule, about
  630px. Columns are evenly spaced when that fits -- 200px each for two or
  three values, ~157px for four, ~126px for five -- and fall back to
  content-aware placement when the longest value would not fit an even column,
  giving each value the room it needs and sharing the remainder as equal gaps.
  Either way the budget is finite: a row with many values needs abbreviated
  ones, which is why the built-in Text row draws "EN"/"DE"/"FR"/"ES"/"IT"
  rather than language names. Labels have their own budget of about 13
  characters before they reach the value column.
- **Text is single-byte CP1252/Latin-1, not UTF-8.** The game's font draws one
  glyph per byte, so `"é"` written as UTF-8 renders as two garbled glyphs.
  Write `"\xE9"`.
- **Labels are localisable, values are not.** One label covers every language;
  `EternalSonataSetOptionRowLabel` overrides individual ones. Values (FPS
  figures, resolution names) are conventionally left untranslated, which is how
  the built-in rows behave too.
- **`get` is polled, `set` runs on the guest menu thread.** Neither may block,
  and `set` must not re-enter this API.

The built-in Resolution and Frame Rate rows go through this exact registration
path -- there is no privileged internal route -- so anything that works for
them works for a mod row. See `src/eternalsonata_options.cpp` for the
underlying display-list and selection work, and `docs/debug-hooks.md` §14 for
the reverse engineering it rests on.

## Reading and changing the party

A mod can ask who is in the party, read and write their stats, add and remove
members, reorder them and rename them, without knowing a single guest address.
Copy
`src/eternalsonata_party_api.h` from this repo into your mod for the
signatures and the full contract, then resolve the entry points out of the host
executable exactly as with the Options API:

```cpp
#include "eternalsonata_party_api.h"

auto add = reinterpret_cast<EternalSonataAddCharacterToPartyFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataAddCharacterToParty"));
if (add) {
  add(ETERNALSONATA_CHAR_VIOLA);
}
```

Things worth knowing before you use it:

- **Always null-check the `GetProcAddress` result**, and call
  `EternalSonataPartyAbiVersion()` if you need to branch on host capability. A
  mod built against a newer host has to keep loading on an older one.
- **Character stats include progression in ABI version 2.** The `exp` field is
  total earned EXP and `exp_to_next` is the status screen's derived Next value.
  `EternalSonataGetCharacterExp` and
  `EternalSonataGetCharacterExpToNextLevel` expose the same values directly;
  the curve lookup functions work without a loaded save.
- **Change EXP through the dedicated functions.** Stat writes deliberately
  leave EXP alone so a mod built against ABI version 1 cannot erase it through
  what used to be reserved space. Setting or adding EXP also updates the
  character's cached level, but does not apply the game's per-level stat gains.
- **Reads answer immediately; writes are queued.** Anything that has to run
  guest code (add, remove, reorder, stat writes) is queued onto the guest main
  thread and returns `ETERNALSONATA_PARTY_QUEUED`, because a guest call from
  the ImGui draw thread crashes the game. The rejections that can be decided
  without running guest code -- unknown character, no save loaded, battle in
  progress, already in the party -- are still returned immediately. Poll
  `EternalSonataIsCharacterInParty` to see a queued change land.
- **Party edits are refused during a battle.** The game's own join sequence
  crashes mid-battle; `EternalSonataIsPartyEditable()` is the flag to gate your
  UI on, and every mutation checks it anyway.
- **There are ten character slots and there is no eleventh.** The game's cast is
  fixed, the per-character tables are exactly ten wide with no room to grow, and
  the API does not pretend otherwise. `docs/party-system.md` has the details.
- **Renaming shows up on the menu screens only.** The status, equipment and
  party screens resolve names through a text lookup the host answers; the battle
  HUD reads the game's packed name tables directly and still shows the built-in
  name.
- **Names are single-byte CP1252/Latin-1, not UTF-8**, like every other string
  the game's own font draws.

`docs/party-system.md` documents the guest-side structures the API sits on, if
you need to know what a call actually does.

## Reading and changing items and gold

The item API covers inventory stacks, the battle Item Set, score pieces,
static item data, custom items, and the party's gold. Copy
`src/eternalsonata_item_api.h` into your mod and resolve its entry points as
with the party API.

```cpp
#include "eternalsonata_item_api.h"

auto add_gold = reinterpret_cast<EternalSonataAddGoldFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataAddGold"));
if (add_gold) {
  add_gold(500);
}
```

`EternalSonataGetGold` reads the shared purse. `EternalSonataSetGold` and
`EternalSonataAddGold` clamp it to `0..ETERNALSONATA_GOLD_MAX`, matching the
game's battle reward cap. A negative add charges the party and floors at zero.
These calls are immediate guest-memory accesses and do not queue.

See `docs/items.md` for the inventory, Item Set, score-piece, and gold layouts.

## Reacting to battle actions

The battle API publishes an event when either side starts an attack or ability,
and when a combatant consumes an item. Copy
`src/eternalsonata_battle_api.h` into your mod for the event names and payload
layout, then subscribe through the shared mod registry:

```cpp
runtime->mod_registry()->Subscribe(
    ETERNALSONATA_BATTLE_EVENT_ABILITY,
    [](const rex::system::ModRegistry::EventPayload& payload) {
      if (payload.bytes.size() != sizeof(EternalSonataBattleAction)) {
        return;
      }
      EternalSonataBattleAction action;
      std::memcpy(&action, payload.bytes.data(), sizeof(action));
      REXLOG_INFO("side {} slot {} used ability {}", action.actor_kind,
                  action.actor_slot, action.action_id);
    });
```

The names are `eternalsonata.battle.attack`, `.ability`, and `.item`. Each
event carries an `EternalSonataBattleAction` in `payload.bytes`. Its actor kind
and slot identify a unit on either side; `character` additionally gives the
party character id and is zero for an enemy. `action_id` is the concrete move
record chosen by the game for attacks and abilities, including distinct attack
variants selected by distance. For an item event it is the item id. The same id
is repeated in `payload.u64`.

`ability_strength` is the live Echo total used to strengthen an ability. It is
captured before the move changes that total. `harmony_chain` and
`counterattack` identify moves launched by those battle paths. The same action
is also published as `eternalsonata.battle.harmony_chain` or
`eternalsonata.battle.counterattack`, which lets a listener subscribe without
inspecting every ability.

`light_state` records whether the selected party move is light or dark. For an
enemy it records which of the enemy's two movesets was active. It is
`ETERNALSONATA_BATTLE_LIGHT_UNKNOWN` when the action does not expose either
answer, such as an item use. `range` and `distance` are reserved for actions
whose target range can be recovered independently; they currently report
`ETERNALSONATA_BATTLE_RANGE_UNKNOWN` and `-1.0f`. Use the concrete attack id to
distinguish the game's near and far attack variants.

Committed HP changes use `eternalsonata.battle.damage` and
`eternalsonata.battle.heal`. Their `payload.bytes` contains an
`EternalSonataBattleEffect`: source and target descriptors, the amount, the
action id, the captured ability strength, and flags for a critical hit, parry,
Harmony Chain, or counterattack. `amount` is positive for both event types and
is repeated in `payload.u64` and `payload.f64`. A parry can carry zero damage.
Critical hits and parries are also repeated on
`eternalsonata.battle.critical` and `eternalsonata.battle.parry` with the same
payload. Area abilities publish one result for each affected target.

### Deaths, status ailments and stat buffs

Five more events report what a battle does to a unit rather than what a unit
does. `eternalsonata.battle.down` fires when a party member is knocked out or
an enemy dies, and `eternalsonata.battle.revived` on the way back. Both carry
an `EternalSonataBattleDown` with the target descriptor, its HP pair, and its
status mask; `payload.u64` is the slot and `payload.f64` the current HP. The
liveness test is the same one `EternalSonataBattleUnit::alive` reports, so a
listener that also polls the unit will never see the two disagree.

`eternalsonata.battle.status_gained` and `.status_lost` carry an
`EternalSonataBattleStatus`. `status` is the game's own status id, 0 through 9,
repeated in `payload.u64`, and `status_mask` is the target's whole mask after
the change. `EternalSonataBattleUnit` now carries the same mask, so a mod can
read the current condition of any unit without waiting for an edge.

The header deliberately does not name the ten ids: which one is poison and
which is darkness has not been established from the executable, and a guessed
table would be wrong in a way nothing would catch. It does document what the
game's code does establish, including that ids 0 to 3 are mutually exclusive
and that every status expires after four of the afflicted unit's turns unless
equipment grants immunity.

`eternalsonata.battle.stat_changed` covers the three stats an ability is
allowed to buff or debuff, as an `EternalSonataBattleStatChange` with the stat
id, the previous and current values, and the signed delta (`payload.u64` is the
stat, `payload.f64` the delta). The game clamps each stat to within 30% of the
unit's pristine value, so the reported value always sits inside that band.

These five are sampled once per battle frame rather than intercepted at the
point of application, which is what lets one mechanism cover ailments, buffs,
deaths and the game's own end-of-turn expiry passes alike. A change a mod makes
itself is therefore reported too. The cost is attribution: `source_kind` and
`source_slot` name whoever held the turn on the frame the change was seen, and
report `ETERNALSONATA_BATTLE_ACTOR_NONE` with a slot of `-1` when nobody did.

The byte span is valid only until the callback returns. Copy it as above if the
mod needs to retain the action. These events are published from the guest
battle thread, so a callback must be thread-safe and must not touch ImGui.

## Overworld events

The overworld surface reports four edges on the shared event bus. Copy
`src/eternalsonata_overworld_api.h` into a mod for the event names, enums, and
the `EternalSonataOverworldEvent` payload shared by all four:

```cpp
#include <cstring>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_overworld_api.h"

void RegisterOverworldEvents(rex::Runtime* runtime) {
  runtime->mod_registry()->Subscribe(
      ETERNALSONATA_OVERWORLD_EVENT_ACTION,
      [](const rex::system::ModRegistry::EventPayload& payload) {
        if (payload.bytes.size() != sizeof(EternalSonataOverworldEvent)) {
          return;
        }
        EternalSonataOverworldEvent event{};
        std::memcpy(&event, payload.bytes.data(), sizeof(event));
        // area_id, animation_id and position identify the map interaction.
      });
}
```

`eternalsonata.overworld.area_entered` fires for a real field transition. It
does not fire for the speculative area preload performed when the player walks
near a gate. `area_id` is the canonical lowercase cfdata id and `area_name` is
the display name used by Discord presence. This pair distinguishes rooms that
share a place name while still providing text suitable for a toast or log.

`eternalsonata.overworld.action` fires when the field leader starts one of the
game's authored interaction motions. `animation_id` is the exact slot from 16
through 35, and `character` is the active party leader. The slots cover chest,
door, climbing, jumping, dropping, and similar map actions. Use the tuple of
`area_id`, `animation_id`, and position when a particular chest, vine, hole, or
other map action matters. `field_object_id` also carries the live scene handle.
The raw slot and coordinates are retained because several maps give the same
motion different meanings.

`eternalsonata.overworld.battle_started` fires after the game commits its field
to battle transition. `encounter_id` identifies the encounter selected by the
field script. `battle_target_id` is the map object id supplied for the enemy
that initiated it, or zero for a scripted battle. The other two fields retain
the game's music and transition rule arguments. `payload.u64` repeats the
encounter id. For an action event it repeats the animation id, while the area
event leaves it zero.

`eternalsonata.overworld.dialogue_started` fires as an overworld dialogue line
is prepared. `dialogue` contains the raw line, including its speaker and name
markup, so a listener can distinguish the NPC and the precise conversation.
Battle narration is excluded. Field cutscenes remain included because they use
the same dialogue path as an NPC conversation.

The byte span is valid only during the callback. These events run on the guest
thread, so callbacks must be thread-safe and must not touch ImGui. Copy the
payload before passing it to another thread.

## Rebalancing enemies

A mod can change what a monster hits for, what it takes to kill one, and what
killing it is worth. Copy `src/eternalsonata_enemy_api.h` from this repo into
your mod for the signatures and the full contract, then resolve the entry points
out of the host executable as with the other APIs:

```cpp
#include "eternalsonata_enemy_api.h"

auto set = reinterpret_cast<EternalSonataSetEnemyTypeStatMultiplierFn>(
    GetProcAddress(GetModuleHandle(nullptr),
                   "EternalSonataSetEnemyTypeStatMultiplier"));
if (set) {
  // every enemy hits 50% harder, for the rest of the session
  set(ETERNALSONATA_ENEMY_TYPE_ANY, ETERNALSONATA_ENEMY_STAT_ATTACK, 1.5f);
}
```

The settable stats are level, current and maximum HP, attack, defense, speed,
physical and magic resistance, critical rate, EXP, gold, two drop slots with
their chances, and move range, chase range and model scale as percentages.

Things worth knowing before you use it:

- **Per-type is the rebalance; per-instance is the escape hatch.** An override
  is a rule keyed by enemy type, held by the host and reapplied to every
  matching enemy in every battle from now on.
  `EternalSonataSetEnemyStat(slot, ...)` writes one live enemy's record instead;
  it lasts as long as the encounter does, and an override on the same stat
  overwrites it on the next frame.
- **Types are keyed by name id**, the same id `EternalSonataGetBattleEnemyName`
  turns into a display name and that `EternalSonataGetEnemyType(slot)` returns.
  Pass `ETERNALSONATA_ENEMY_TYPE_ANY` to hit every enemy in the game; a type's
  own override wins over it, so "everything ×1.5 except this boss" needs no
  ordering.
- **Multipliers apply to the shipped value, not the current one.** The host
  snapshots each enemy's untouched stats, so calling `set(..., 1.5f)` twice is
  still ×1.5, and clearing an override restores the original number. Prefer a
  multiplier to an absolute value: the game scales an encounter's enemies with
  the story, so ×1.5 keeps its meaning at every point in the game while `= 240`
  does not.
- **To remove an override use `EternalSonataClearEnemyTypeStat`**, not a
  multiplier of 1.0. The latter keeps reasserting the original value over any
  per-instance write.
- **Nothing here queues.** Unlike the party and item APIs, no entry point runs
  guest code, so reads, writes and override changes all return a real result
  immediately and are safe from any thread including the ImGui draw thread.
- **Overrides are memory-only.** Nothing touches the save; reapply your
  rebalance on load if you want it to survive a restart.
- **A battle yields at most three drops in total**, however many enemies it
  holds, so raising every drop rate to 100 does not hand out one per enemy.
- **There is no enemy magic attack stat.** An enemy's spell power comes from the
  ability rather than a stat, so `ATTACK` is the only offensive number to touch;
  the two resistances are what separate incoming physical from magical damage.

Changes are published on the shared mod registry bus as
`eternalsonata.enemy.override.set` and `.cleared`, with the type in the
payload's high 32 bits, the stat id in its low 32, and the multiplier or value
in `f64`.

`docs/enemies.md` documents the guest-side record the API sits on.

## Watching saves

A mod can ask whether the game would let the player save right now, read or
change play time, list what is in each save slot, and be told when a save
starts, finishes, or fails. Copy
`src/eternalsonata_save_api.h` from this repo into your mod for the signatures
and the full contract; the entry points resolve out of the host executable the
same way as the Options and party APIs.

The three points of a save are published on the shared mod registry bus
instead of through exported callbacks, so subscribing needs neither the header
nor a linked symbol:

```cpp
runtime->mod_registry()->Subscribe(
    "eternalsonata.save.completed",
    [](const rex::system::ModRegistry::EventPayload& payload) {
      REXLOG_INFO("saved to slot {}", payload.u64);
    });
```

The names are `eternalsonata.save.started`, `.completed` and `.failed`. In all
three the payload's `u64` is the slot (0..9); `.failed` also puts the reason in
`f64`, either `ETERNALSONATA_SAVE_FAIL_NOT_STARTED` (the game never got the
write off the ground) or `ETERNALSONATA_SAVE_FAIL_WRITE` (the write ran and
reported failure). Exactly one of `.completed`/`.failed` follows each
`.started`.

Things worth knowing before you use it:

- **The events do not come from the frame tick or the UI thread.** They are
  published from whichever guest thread reached that point of the save:
  `.started` from the thread driving the save menu, `.completed`/`.failed`
  from the content worker thread the game spawns for the write. A subscriber
  must be thread-safe, must not touch ImGui, and should hand anything
  expensive to its own `RegisterTick`.
- **`EternalSonataCanSave()` reports the game's gate, not storage.** It is 1
  when the pause menu's Save entry is selectable, i.e. the party is at a save
  point (or a mod is forcing that flag). A save can still fail afterwards,
  which is what `.failed` is for.
- **`EternalSonataListSaves()` does filesystem I/O.** It walks the current
  profile's save containers, so call it on demand, not every frame. Passing
  `NULL`/`0` returns the count without writing anything, so a caller can size
  its own array first.
- **`EternalSonataSetSaveAlwaysAllowed(1)` forces that gate**, making Save
  selectable away from a save point. This is a host call because the gate is a
  guest flag the field scripts rewrite on their own schedule: the host sets it
  immediately before the menu build reads it, which a mod writing guest memory
  from a per-frame tick cannot do reliably. Patching the branch is not an
  option either, since a recompilation translates the guest code at build time.
- **Play time is exposed in whole seconds.** `EternalSonataGetPlayTime`,
  `EternalSonataSetPlayTime`, and `EternalSonataAddPlayTime` convert the game's
  saved 300 Hz clock and clamp it at `99:59:59`, the status screen's maximum.
- **There is no save/load call.** The gate and play clock are writable, but
  making the game save or load a slot on demand is not exposed.

## Adding achievements

A mod can add achievements of its own. They behave like the title's twenty two
everywhere: the unlock toast, the SDK's achievement overlay, and the status
menu's Achievements screen, whose third tab (**Custom**) is drawn only when at
least one mod registered something. Copy
`src/eternalsonata_achievement_api.h` from this repo into your mod for the
signatures and the full contract:

```cpp
#include "eternalsonata_achievement_api.h"

auto reg = reinterpret_cast<EternalSonataRegisterCustomAchievementFn>(
    GetProcAddress(GetModuleHandle(nullptr),
                   "EternalSonataRegisterCustomAchievement"));
auto unlock = reinterpret_cast<EternalSonataUnlockAchievementFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataUnlockAchievement"));

EternalSonataCustomAchievementData data = {};
data.mod_id = "my_mod";        // your folder name under mods/
data.key = "flawless";         // stable name, so others can translate it
data.name = "Perfect Pitch";
data.description = "Win a battle without taking damage.";
data.gamerscore = 50;
const int id = reg ? reg(&data) : -1;  // keep this for the session
...
if (id > 0 && unlock) {
  unlock(id, /*show_toast=*/1);
}
```

Things worth knowing before you use it:

- **The host assigns the id**, like custom items: registration returns one from
  a range reserved for mods (`0x10000` up, 32 of them across all mods), so two
  mods cannot collide and nobody has to know what the title already uses. Hold
  on to the returned id for the rest of the session and key everything else off
  it.
- **The id is not stable across runs.** It depends on how many mods registered
  before yours, so never write it to a file. Unlock state is persisted by id in
  the profile's achievement save, which means a mod that changes how many
  achievements it registers can find old unlocks landing on the wrong entry; if
  that matters, record your own unlock state and re-unlock on startup.
- **Register once, early.** `OnModuleLaunched()` is the natural place.
  Registering later is legal: the Achievements screen rebuilds its rows every
  time it is opened, so a new entry appears the next time the player opens it.
- **`secret = 1` hides the name and the description until it is unlocked**,
  matching the title's own secret achievements: the row reads `???` until the
  player earns it or reveals it with the Y prompt. Leave it 0 and a locked row
  shows both, which is what an achievement the player is meant to aim for wants.
- **Your own translations come in with the registration.** Fill `translations`
  with one `EternalSonataAchievementTranslation` per language you have strings
  for; the host keeps the entry matching the language the game booted in and
  drops the rest, falling back to `name`/`description` for anything
  untranslated. To pick the strings yourself instead,
  `EternalSonataGetAchievementLanguage()` answers the boot language. The status
  menu folds them to Latin-1, so stay inside that (accents are fine, anything
  else draws as `?`).
- **Set `mod_id` and `key` so other people can translate it.** The
  `achv_name_<id>` table the title's own achievements use is no help on its own,
  since its keys are ids and a mod does not choose its id. Name your mod and the
  achievement instead (`mod_id = "shutterbug"`, the folder name under `mods/`,
  and `key = "five_photos"`); the host joins the two, so a translation mod
  addresses it from its `[language.strings]` as
  `achv_name_shutterbug_five_photos`, `achv_desc_shutterbug_five_photos` and
  `achv_desc_locked_shutterbug_five_photos`. Those win over the `translations`
  array, so a translator can also correct a language you did ship. Leave `key`
  null and the host numbers your achievements in registration order instead
  (`shutterbug_0`), which is fine until you reorder or conditionally skip a
  registration and every existing translation shifts onto the wrong one.
- **A language a mod added works for achievements too.** Both routes match on
  the id of the language the game booted in, and for a mod-added language that
  is whatever id the language mod declared, so `[language.strings]` entries
  under it reach achievements exactly as they reach anything else this project
  draws.
- **`gamerscore` is drawn in the row's left column**, which has room for four
  characters, so it reads well up to `100G`.
- **`icon_path` is for the toast and the overlay only.** The status menu draws
  its own icon art and ignores it.
- **Nothing here queues.** No entry point runs guest code, so every call answers
  immediately and is safe from any thread, including the ImGui draw thread.
- **Unregistering frees the id** but leaves the unlock in the save, so
  registering into the same id again restores it.
- To be notified of *any* unlock, including the title's own, use the SDK
  directly: `rex::system::kernel_state()->achievements().RegisterUnlockCallback()`.

## Patching static game text/data

Item/enemy names, descriptions, and similar flavor text aren't loaded from
a separate asset file the VFS can overlay -- they're baked into
`default.xex`'s static data (`.rdata`), copied into guest memory once at
startup by the same loader that runs the game's code. Byte layout, text
encoding, and the AES/compression format `default.xex` ships in on disk
are documented in `extracted/README.md`.

**Check first whether the string you want is actually one of these.** The
menu chrome, and about 2850 strings per language, live in `BTX ` blobs inside
the image and are patched declaratively with no code at all; see
[The game's own menu chrome](#the-games-own-menu-chrome-defaultxex). What is
left for this section is the flat static data that is not in a blob, such as
item and enemy name/description fields.

Two ways to change one of those strings:

**Replace `default.xex` itself** (`mods/<name>/game/default.xex`).
`XexModule::ReadImage` in the SDK doesn't verify a signature on the base
image load, and accepts a plain, unencrypted/uncompressed XEX2 file
(`scripts/re/rebuild_xex_unencrypted.py` builds one). A mod's
`game/default.xex` **replaces the whole file** with no merging: if two
mods each ship one, `enabled_mods` order picks a single winner and the
other mod's edits are gone, whether or not they touched the same bytes.
Use this only when a mod is the sole thing expected to touch game text,
or needs a structural change a same-length string swap can't do.

**Patch guest memory from a code mod instead**: any number of mods can
each own a different address with no conflict, the same way
`src/ui_color` pokes the accent-color struct. Use
`src/common/include/rexmod/text_patch.h`'s `ApplyTextPatch`
(description fields, plain ASCII) or `ApplyNameFieldPatch` (name fields,
which use a "big first letter" font encoding -- see
`extracted/README.md`) from `OnModuleLaunched()`. Requirements:

1. **The guest address must come from scanning a live, running process**
   (`scripts/re/scan_guest_memory.py`), not from offline-decrypting
   `default.xex` and computing a file offset. A title that ships a
   `default.xexp` (a title-update delta patch -- check the startup log
   for `XEX patch applied successfully`) has that patch applied over the
   base image on every launch, which can shift addresses after the
   patched region by an amount offline decryption of the base file alone
   won't account for.
2. **The target field is very likely in read-only memory.** Writing to it
   in-process access-violates (`0xC0000005`) unless the page is unlocked
   first: `runtime->memory()->LookupHeap(addr)->Protect(addr, size,
   kMemoryProtectRead | kMemoryProtectWrite, &old_protect)` before the
   write, then `Protect(addr, size, old_protect)` after --
   `ApplyTextPatch`/`ApplyNameFieldPatch` already do this.
3. **Field length must be measured from what actually follows the string
   in memory**, not assumed from `len(text) + 1`: some fields end in a
   single null before the next field, some have no terminator at all
   (butting directly against the next field or an entry-end marker), and
   the pattern isn't consistent enough to assume without checking the
   specific field being patched.

## Both builds, one DLL

The project ships two builds (vanilla and title-update) with different guest
addresses. A code mod built as described above works with both as long as it
never hardcodes *just one* build's address: either read guest state
generically (like `memory_peek` does), or look the address up from the
shared registry (like `ui_color` does; see
[Library mods and the shared registry](#library-mods-and-the-shared-registry)
above) instead of branching on `is_patched()` itself. All the sample mods
here follow one of those two rules, so `make_mods.py`'s output loads
unchanged into either build.

## Whole-file asset replacement

This is the low-level path: it swaps a **whole guest file**, with no merging.
For text, textures and meshes prefer
[Replacing single assets](#replacing-single-assets), which ships only what you
authored and lets two mods touch the same container. Use this one for assets
that are a file of their own (audio, DLC content), for a structural change no
in-place splice can do, and for GPU-side texture and shader overrides, which
are keyed by content hash and are not container-aware at all.

An asset mod is just a folder under `mods/<name>/` with any of these
subfolders (all optional; only the ones present are used):

```
mods/<name>/
  assets/      granular per-asset replacement (see above)
  game/        overlays the game data partition (game:\ / d:\)
  update/      overlays the update partition
  dlc/<name>/  overlays an installed DLC package
  textures/    texture replacements: <hash16>.dds or .png (flat dir)
  shaders/     shader replacements (DXBC/SPIR-V binaries)
  mod.toml     descriptive metadata (see below)
  icon.png     shown in the F1 mod manager overlay
```

Files under `game/`/`update/`/`dlc/` mirror the exact guest path they
replace, for example `mods/<mod>/game/DATA/sound/bgmusic.wma` replaces
`DATA/sound/bgmusic.wma`. If two mods ship the same guest path, the one
earliest in `mods.toml` wins outright and the other's file is simply not used,
whether or not the two changed the same bytes.

Texture files are named by a 16-hex-digit content hash (dump one with
`texture_dump_enabled = true` in the config to find the hash for a texture you
want to replace). This path keys on the bytes the GPU sees, so it works for any
texture the game uploads regardless of which container it came from, and it
composes fine with `assets/`: hash replacements are applied at upload time,
container patches at load time.

[dumping-and-replacing-assets.md](dumping-and-replacing-assets.md) covers the
texture and shader dump/replace cvars and workflow in detail.
