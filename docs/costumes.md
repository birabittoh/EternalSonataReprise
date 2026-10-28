# Costumes

A costume is another model for a party character, worn in the field, in
battle and in cutscenes, or only other camp portraits for it. All twelve can
have them on PS3 data; on the Xbox 360's, the ten it has and any modded
character. The PS3 release
ships four (Allegretto's, two of Polka's, Beat's) behind a "Costumes" camp
menu; the Xbox 360 releases ship none. This host keeps the feature on every release, so mods can add
costumes of their own, for any character.

On the status page, X puts on the shown character's next unlocked costume,
and every camp portrait follows the costume worn: the status page's, the one
to three member panel's and the faces of the larger layouts, the swaps and
the item target list. Its X Costume prompt replaces the
LB / RB Switch Character one, and shows only while X would change something;
the shoulder buttons still switch. A mod can pick any costume through
`src/api/eternalsonata_costume_api.h`; the `costume_overlay` mod in
EternalSonataReprise-Mods (F11) does that, and locks or unlocks them. Each save keeps the costumes worn
(§7); the title screen and a new game start from the defaults, or from what
the `costumes` cvar says.

## 1. How the game picks a model

`sub_82162058` loads `AppKeep.bmd` at boot and stores a pointer to each of its
entries in `dword_82420AFC` onwards. Entries 0..9 are the ten field character
models, so `dword_82420AF8[1..10]` reads as one model per character:

| slot | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| model | ALG | PLK | BET | CPN | VOL | SLS | JRB | FST | MCH | CLV |

March and Claves are the other way round from the party numbering (Claves is
character 9, March 10). Crescendo and Serenade (11, 12) have no slot: on PS3
data their models are `appkeep2.bmd` entries 0 and 1. Every place that builds a party member reads this
table and builds the model in place from the raw `NOBJ` it points at: the
field leader (`sub_820F9828`, `sub_820FCF80`), battle (`sub_8218E558`,
`sub_8218E7F0`) and events through native 1141 (`sub_820E8B10`).

## 2. How a costume is worn

`src/engine/costume_system.cpp` keeps a list per character, the default first.
Wearing a costume writes its model's guest address into the character's slot,
so whatever reads the slot next builds it, exactly what the PS3 does with its
`WEAR*` slots. For Crescendo and Serenade the host keeps the model worn, and
their readers (the field leader, the battle model switch and native 1141's
-20 and -21) ask `CostumeModel` for it. A costume without a model wears the
character's own. Battles and events build their models when they start; the
field leader is the one model that lives across a change, so it is respawned
on the next field frame outside a cutscene (`FieldPlayerModelOverride::
RequestRespawn`, which keeps pad control, ground state and shade).

A costume's file is read into physical guest memory when it is worn. Scene
objects read their model in place, so the host counts the objects built from
each costume buffer (`sub_82176398` makes one, `sub_82175620` tears it down).
A costume taken off keeps its buffer until the last of those objects is gone
and half a second has passed for the GPU, and the next load frees it; so do
the models of a slot 11 or 12 occupant a mod redefines. Guest
memory then holds about one model per character in costume (some 4 MB each),
whatever the number of costumes installed; free physical memory is scarce, so
nothing more can be kept.

## 3. Model files

A costume is a complete `NOBJ` chunk in the Xbox 360 layout: the same thing
`AppKeep.bmd` entries 0..9 are, or a PS3 `.p3obj` once the installer has
converted it. Only the `NOBJ` tag and size are checked; anything else wrong
with the model shows up when the game builds it. The motions are the
character's own (bone names have to match its skeleton), and the field leader
hides the mesh named `weapon` and `tasuki_sw`, so a costume should keep the
character's mesh names.

`scripts/models.py` makes these files: `list` shows the models of an
`AppKeep.bmd` (or a `.p3obj`) with their meshes and textures, `dump
AppKeep.bmd:1 dir --png` splits one into chunk files plus PNGs of its
textures, and `pack dir out.nobj` rebuilds it from the chunks, so an edited
chunk replaces the original. A chunk may only change size in multiples of 4096,
as an `NTX2`'s pixels are found by 4 KB alignment. `recolor_texture` rewrites
the DXT endpoints in place, which needs no untiling and keeps every size.
For a plain recolour, `tint` (§4) does the same at run time with no files.

## 4. Adding costumes from a mod

With no code, from `mods/<name>/assets.toml`:

```toml
[[costume]]
character = "polka"                 # or 1..12, the party numbering
name = "swimsuit"                   # the id becomes "<mod folder>/swimsuit"
label = "Swimsuit"                  # what menus show
model = "costumes/plk_swim.nobj"    # optional, relative to the mod folder
locked = true                       # optional: a new game starts it locked
portrait = "costumes/plk_swim.dds"  # optional: the status page portrait
panel_portrait = "costumes/plk_swim_panel.dds"      # optional, see below
face_portrait = "costumes/plk_swim_face.dds"        # optional
small_face_portrait = "costumes/plk_swim_face2.dds" # optional
```

Each portrait is a DDS file (or an `NTEX` chunk, which is the same DDS behind
an 8 byte header) the size of the character's own; a kind left out keeps the
character's own. A costume with no `model` keeps the character's model and
changes only these. On Xbox 360 data a `[[costume]]` table for a
modded character ([modded-characters.md](modded-characters.md)) names it by
its key and waits until the character is defined; one for Crescendo or
Serenade never applies. From C++, `EternalSonataSetCostumePortraitFile` sets
one by kind:

| kind | key | where |
|---|---|---|
| `STATUS` | `portrait` | the status page |
| `PANEL` | `panel_portrait` | the one to three member panel, in the camp, its swap and the item target list |
| `FACE` | `face_portrait` | the four or more member layouts, the swaps and the item target list pick one face set or the other; a bust filling the image |
| `SMALL_FACE` | `small_face_portrait` | as above; a smaller head |

Each character's layouts are made for its own art, so sizes differ (width x
height, PS3 data, all DXT5 with mipmaps):

| character | `STATUS` | `PANEL` | faces |
|---|---|---|---|
| Allegretto | 512 x 512 | 256 x 512 | 256 x 256 |
| Polka, Beat | 512 x 512 | 512 x 512 | 256 x 256 |
| Frederic | 512 x 512 | 256 x 512 | 256 x 256 |
| Viola, Jazz, Crescendo | 1024 x 512 | as status | 256 x 256 |
| Salsa, Falsetto, Claves, March, Serenade | 512 x 512 | as status | 256 x 256 |

From Viola on, the game's panel shows the status page's art, so a costume of
theirs without a `PANEL` portrait shows its `STATUS` one there.

The status portrait loads when the status page shows it, the others when the
costume is put on, each into one of the free AppKeep image slots (456 to
511). `sub_821E8F58` caches the texture it decodes by image id until the menu
closes, so every portrait needs an id of its own: a slot goes back to the pool
once its costume is off and that cache holds nothing for it, and any number
of costumes fits as long as one menu does not show more than 56 of their
portraits. Wearing
points the twelve wide copies of the face tables `0x8202CA28` / `0x8202CA3C`
at the faces, and taking the costume off puts back what they held at boot.
The panel is swapped where it is built: after `sub_821DDD00` (its image
record at `list + 48`), after each case of the item target list's copy in
`sub_8221B5A0`, and before each case's `sub_821E8F58` in the swap
`sub_82237A68` (`party_camp_menu.cpp`).

Not covered: the save rows, which draw each save's own party from
`0x822FF530` and would need that save's costumes, and the battle HUD, whose
portraits are layout groups of BattleKeep slot 41 (the PS3 adds its costumes
as groups 18..21).

### Tints

A costume can instead start from one the character already has, and replace
some of its colours, so a mod ships no files at all:

```toml
[[costume]]
character = "polka"
name = "blue"
label = "Blue"
base = "ps3/2"              # optional: the costume to start from, else "default"

[[costume.tint]]            # any number: colours of the model
from = "#B4443A"            # the colour to replace
to = "#3E6EC0"              # what it becomes
textures = [2, 4, "hood.tga"]  # optional: which textures, else all of them
hue_range = 18              # optional: degrees either side of from, default 20
min_saturation = 0.38       # optional, 0..1: default 65% of from's
max_saturation = 1          # optional, 0..1
min_value = 0               # optional, 0..1: brightness bounds
max_value = 1               # optional

[[costume.portrait_tint]]   # any number: colours of the portraits
from = "#B4443A"
to = "#3E6EC0"
kinds = ["status", "face"]  # optional: status, panel, face, small_face; else all
region = "outfit"           # optional: a [[portrait_region]] of the character
face_exclude = [[0, 0, 140, 180]]  # optional: region keys of its own, see below
```

A colour of a texture is replaced by as much as its hue is within
`hue_range` of `from`'s and its saturation reaches `min_saturation`, with a
soft edge; `from` itself becomes `to`, and its lighter, darker and greyer
shades follow, so shading survives. The saturation and value bounds take
colours out that share the hue, such as skin beside brown leather (Jazz's
portraits keep `max_value = 0.38`). A grey `from` (saturation under 5%) has
no hue to match: its rule paints `to`'s hue and saturation over everything
its bounds catch, scaling brightness as usual, which is how Claves's grey
outfit takes a colour. Textures are named as
`scripts/models.py list -v` shows them, or by their index in that list:
Xbox 360 models name theirs, PS3 models (`.p3obj`, the PS3 costumes) do not,
and a character's model lists its textures in the same order on both.
`scripts/tint_preview.py` previews a rule on a model and portraits, and
`--mask` shows what it catches. Portrait tints are separate from model ones,
since the same outfit is usually a different shade in the art.

Each DXT block's four palette colours are tinted, which tints every pixel,
and the block is encoded again on its own, so nothing is untiled and every
size stays
(`src/engine/costume_tint.cpp`). The base's model and portraits are copied at
boot, before the game builds anything from them. A tint of the default model
is written over the default's own buffer when worn and put back when taken
off; other bases get a tinted copy; a costume that ships its own `model` or a portrait
file uses that instead. Where another mod replaced a base portrait
through a texture patch (`assets/<container>/textures/`), the costume starts
from the game's own art instead: the rules were made for it, and the mod's art
stays the base's. A base is any costume of the character but another
tinted one. One whose base the game data lacks, such as `ps3/2` on Xbox 360
data, is skipped with a log line. `polka_colors` in EternalSonataReprise-Mods
is eight tints of Polka and two of her PS3 costume, and `<name>_colors`
does the same for every other character.

What those mods tint, as a starting point for others (texture indices as
`models.py list -v` orders them; colours as `from`, hue range, bounds):

| character | model textures | model rule | portrait rule |
|---|---|---|---|
| Allegretto | 7 vest | `#7F9997` 25°, sat ≥ 0.08 | `#85A6A6` 14°, sat 0.06..0.3 |
| Polka | 2, 4, 5, 7, 10 red; 15 umbrella | `#B4443A` 18°, sat ≥ 0.38 | same red |
| Beat | 1, 4 coat, 6 sleeves | `#CCCA85` 14°, sat ≥ 0.28 | `#CED96C` 14°, sat ≥ 0.3, value ≥ 0.65 |
| Frederic | 1, 2, 3 coat | `#252238` 25°, sat ≥ 0.2 | `#2D2A4C` 20°, sat ≥ 0.38, value ≥ 0.12 |
| Viola | 2 skirt, 9 shoes, 11 stole | `#D4A974` 12°, sat ≥ 0.3 | `#997245` 8°, sat ≥ 0.45, value ≤ 0.75 |
| Salsa | 0 hat band, 8 top | `#BF7143` 12°, sat ≥ 0.45 | `#CC7833` 7°, sat ≥ 0.6 |
| Jazz | 0 boots, 2 vest, 6 gloves | `#4C2A1F` 15°, sat ≥ 0.45 | same, value ≤ 0.38 |
| Falsetto | 1, 3, 4, 7, 9 navy | `#2C3E5C` 18°, sat ≥ 0.3 | `#2E4773` 18°, sat ≥ 0.35 |
| Claves | 1, 4 grey outfit | grey `#4C4C4C`, sat ≤ 0.12, value ≤ 0.6 | grey, sat ≤ 0.12, value 0.08..0.5 |
| March | 7 top | `#6C9E91` 22°, sat ≥ 0.18 | `#74A69D` 28°, sat ≥ 0.1 |
| Crescendo | 3, 4, 5 navy trim | `#2D3859` 20°, sat ≥ 0.25 | `#454973` 25°, sat ≥ 0.2 |
| Serenade | 3, 4, 7, 8, 9 dress | `#D9D2AD` 18°, sat 0.08..0.35 | `#DBD3AC` 6°, sat 0.08..0.35, in a dress region, as her skin is the dress's cream |

Every texture of the default models, by index. Hair is a texture of its
own, so a hair tint needs only its index; silver and black hair (Allegretto,
Frederic, Jazz) wants a grey `from`. Eyebrows and the hairline are painted
on the face texture and keep their colour. Names are the Xbox 360 model's;
Crescendo and Serenade exist only as PS3 models, whose textures have no
names, so theirs are described by look.

| character | hair | face, eyes | outfit | weapon | alpha masks |
|---|---|---|---|---|---|
| Allegretto | 5, 6 | 4, 0 | 1 bottom, 2 hand, 3 arm, 7 vest, 8 feather, 9 and 12 bag, 10 boot, 11 boot plate, 13 ribbon, 15 jewel, 16 belt, 17 belt heart | 14 | 18, 19, 20 |
| Polka | 3 | 1, 0 | 2 ribbon, 4 skirt, 5 shoes, 6 apron, 7 stole, 8 arm, 9 vest, 10 hood, 11 frill, 12 accessories, 13 feather, 14 leg | 15 canopy, 16, 17 | 18 |
| Beat | 3 | 2, 5 | 0 feet, 1 and 4 coat, 6 hands, 7 camera | 8 | |
| Frederic | 9 | 10, 11 | 0 hands, 1, 2 and 3 coat, 4 and 5 lace, 6 metal fittings | 7, 8 | 12, 13 |
| Viola | 4 | 10, 6 | 0 accessories, 1 top, 2 skirt, 3 hands, 5 belt, 7 legs, 8 fringe, 9 shoes, 11 stole | 12, 13, 14 | 15, 16 |
| Salsa | 4 | 5, 7 | 0 and 11 hat, 1 trousers, 2 necklace, 3 accessories, 6 lining, 8 top, 9 legs, 12 hands | 10 | |
| Jazz | 12 | 11, 10 | 0 boots, 1 shirt, 2 vest, 3 coat, 4 skirt, 5 and 8 metal, 6 gloves, 7 shoes, 9 chain | 13, 14, 15 | |
| Falsetto | 6 | 5, 0 | 1, 3 and 4 outfit, 2 fringe, 7 lining | 8, 9 | 10 |
| Claves | 5 | 7, 2 | 0 trims, 1 lower body, 3 legs, 4 upper body, 6 shoes, 8 hands, 9 gauntlet | 10 | |
| March | 9 | 4, 6 | 0 hands, 1 trousers, 2 necklace, 3 accessories, 5 lining, 7 top, 8 legs | 10 | |
| Crescendo | 1 | 0, 8 | 2 gloves, 3 coat, 4 gold trim, 5 cape, 6 white cloth, 7 crystal and gold | 10 | 9 |
| Serenade | 2 | 1, 0 | 3 accessories, 4 skirt, 5 gold ornaments, 6 skin, 7 and 8 dress, 9 gloves | 12 | 10, 11 |

Portrait AppKeep entries for the preview are image id minus one: status
194 + n, panel 204 + n (n up to 4), faces 208 + n and 218 + n, for party
number n. Crescendo and Serenade's are PS3 `appkeep.bmd` 204 and 205 and
`campdata/camp_char.bmd` 39, 40, 48 and 49.

`mods/costume_test` in EternalSonataReprise-Mods dresses all twelve in a
costume that keeps each model and captions every portrait with its
character and kind; its art is generated from PS3 data at build time.

### Portrait regions

Colour alone rarely isolates one part of a portrait: eyes, lips and skin
share hues with outfits, and a weapon may be the colour of a dress. A
portrait rule can be confined to a region of the art, so one mod tints
Polka's hair and another only her umbrella:

```toml
[[portrait_region]]               # any number, named per character
character = "polka"
name = "outfit"
exclude = [[148, 138, 252, 254]]  # every kind: pixels to leave
face_rects = [[0, 160, 256, 256]] # one kind: pixels to tint in
status_mask = "regions/polka_hood.png"
```

Coordinates are pixels of the portrait's full size, `x0, y0` inclusive and
`x1, y1` exclusive, top left origin; an array may run over several lines.
Three keys say where: `rects` (inside any of them, or anywhere when there
are none), `exclude` (outside all of them) and `mask` (a PNG or 32 bit DDS,
stretched over the portrait; white tints fully, grey partly, black or
transparent not at all). They multiply with the colour match. Each takes a
kind prefix (`status_`, `panel_`, `face_`, `small_face_`) to apply to that
kind alone, because each kind frames the art differently. A key without one
applies to every kind, and a prefixed one replaces it for its kind.
Characters 5 to 12 show the status art as their panel, so their `panel_`
keys repeat the `status_` ones. Leaving a kind out entirely takes an
`exclude` that covers it, such as `[[0, 0, 4096, 4096]]`.

A rule names a region with `region`, and may add the same keys of its own
on top, which replace the region's. Tinting works on DXT blocks,
so a region resolves to 4 by 4 pixel blocks, and a block on its edge is
tinted by the share of it inside. Regions apply to portraits only; a model
rule picks its textures instead.

`preview_costume_tints.py` in EternalSonataReprise-Mods renders a mod's
portrait tints on the game's art, region included, with `--mask` to show
what each costume catches and `--grid` for a pixel grid to read rectangles
off.

### From C++

Register a model held in memory or a file the host reads on first
wear:

```cpp
#include "eternalsonata_costume_api.h"

auto add = reinterpret_cast<EternalSonataRegisterCostumeFileFn>(
    GetProcAddress(GetModuleHandle(nullptr), "EternalSonataRegisterCostumeFile"));
if (add) {
  const int costume = add(ETERNALSONATA_COSTUME_CHAR_POLKA, "my_mod/swimsuit", "Swimsuit",
                          "mods/my_mod/costumes/plk_swim.nobj");
}
```

Costumes keep their registration order: default, the PS3's (PS3 data only),
then code mods in load order, then `assets.toml` declarations. Ids must be
unique per character. `EternalSonataWearCostume` puts one on from any thread,
and `eternalsonata.costume.changed` reports every change on the mod registry
bus.

### Locks

A locked costume is skipped by the status page's X and cannot be picked. Locks belong to
the game in progress, like an item would: `EternalSonataUnlockCostume` and
`EternalSonataLockCostume` change them, they are saved with the game (§7), and
loading a save or starting a new game puts them back to that game's state.
Locking the costume worn puts the default back on; the default cannot be
locked. `eternalsonata.costume.unlocked` and `eternalsonata.costume.locked`
report each change, with the same payload as `changed`.

What a new game starts with is the costume's own setting: a mod's costumes
start unlocked unless `assets.toml` says `locked = true` or the mod calls
`EternalSonataSetCostumeStartsLocked` right after registering. A mod that
unlocks its costume at some point of the story calls
`EternalSonataUnlockCostume` then. Wearing through the API or the `costumes`
cvar ignores locks.

## 5. Choosing at boot

```
--costumes=polka=ps3/3,allegretto=my_mod/swimsuit
```

Character names are the lowercase English ones (`allegretto` .. `serenade`)
or their numbers. The ids are those `EternalSonataGetCostumeId` reports:
`default`, `ps3/2` and `ps3/3` for the PS3's, `<mod folder>/<name>` for an
`assets.toml` costume.

## 6. The PS3's costumes

On PS3 data the host registers `pc{alg,plk,bet}_v2.p3obj` and `pcplk_v3.p3obj`
as `ps3/2` and `ps3/3`, labelled as the PS3's menu names them. PS3 scripts ask
for the costume through native 5028, which answers the PS3 variant of the
costume worn and 1 for anything else, and events use ids -10..-12 for the
worn costume of Allegretto, Polka and Beat, which native 1141 reads from the
same slots ([ps3-assets.md](ps3-assets.md) §2).

They start locked, and `lib.e` unlocks them through 5026, which goes through
the same locks as the API; 5027 reads them back. The grant is the chest
routine's (`lib.e` `0x5E50`): items 427..430 are kept out of the inventory and
unlock ALG v2, PLK v2, PLK v3 and BET v2. The chests are in `tcd04` (427),
`lam05` (428), `tnt40`/`tnt45` (429) and `rty04` (430), so this needs no host
code beyond 5026. The PS3 changes them on a
camp menu page of its own, which the camp menu flag `+0x920` (5032) turns off;
here X on the status page does, whenever the character has another unlocked
costume, regardless of that flag. Their
portraits are `campdata/camp_char.bmd` entries, which the PS3 picks with
`sub_1E5840(character, kind)`: kind n of variant v is entry 3n + 12v +
character - 13 (Polka's v3: n + 24), kind 0 the panel, 1 and 2 the face sets,
3 the status page. The status portraits (21, 22, 27, 23) are placed in AppKeep
slots 430..433, the panels and faces in 434..445.

## 7. Saving

The guest save has no room for costumes, so the host writes them next to it:
`reprise.txt` inside the slot's `savecontentNN` container, one `key = value`
line each (`src/engine/save_record.cpp`). The record is taken when the save
starts and written once the container is; loading a save puts its costumes
and locks back. Costumes are saved by id: `costume.polka = ps3/3` for the one
worn, and `unlocked.polka.ps3/3 = 1` for each lock that differs from a new
game's. One whose mod is gone falls back to the default. A save without a
record, from the 360 or from before this, loads as a new game would.
