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

A costume's file is read into physical guest memory the first time it is
worn and kept there. Models already built keep reading their buffer, so a
buffer is never reused for another costume; switching back and forth costs
nothing after the first load. Each costume costs its file size, about 4 MB
for a character.

## 3. Model files

A costume is a complete `NOBJ` chunk in the Xbox 360 layout: the same thing
`AppKeep.bmd` entries 0..9 are, or a PS3 `.p3obj` once `ps3_convert.py` has
converted it. Only the `NOBJ` tag and size are checked; anything else wrong
with the model shows up when the game builds it. The motions are the
character's own (bone names have to match its skeleton), and the field leader
hides the mesh named `weapon` and `tasuki_sw`, so a costume should keep the
character's mesh names.

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

The status portrait loads the first time the status page shows it, the others
when the costume is put on, into the AppKeep image slots from 446 on. Wearing
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

`mods/costume_test` in EternalSonataReprise-Mods dresses all twelve in a
costume that keeps each model and captions every portrait with its
character and kind; its art is generated from PS3 data at build time.

From C++, register a model held in memory or a file the host reads on first
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
the same locks as the API; 5027 reads them back. The PS3 changes them on a
camp menu page of its own; here X on the status page does, and is off while
the camp menu flag `+0x920` (5032) is, as that page is on the PS3. Their
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
