# Costumes

A costume is another model for one of the ten party characters, worn in the
field, in battle and in cutscenes. The PS3 release ships four (Allegretto's,
two of Polka's, Beat's) behind a "Costumes" camp menu; the Xbox 360 releases
ship none. This host keeps the feature on every release, so mods can add
costumes of their own, for any character.

Costumes are picked in the F11 overlay or by a mod through
`src/api/eternalsonata_costume_api.h`. The choice is not saved yet: every
session starts on the defaults, unless the `costumes` cvar says otherwise.

## 1. How the game picks a model

`sub_82162058` loads `AppKeep.bmd` at boot and stores a pointer to each of its
entries in `dword_82420AFC` onwards. Entries 0..9 are the ten field character
models, so `dword_82420AF8[1..10]` reads as one model per character:

| slot | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| model | ALG | PLK | BET | CPN | VOL | SLS | JRB | FST | MCH | CLV |

March and Claves are the other way round from the party numbering (Claves is
character 9, March 10). Every place that builds a party member reads this
table and builds the model in place from the raw `NOBJ` it points at: the
field leader (`sub_820F9828`, `sub_820FCF80`), battle (`sub_8218E558`,
`sub_8218E7F0`) and events through native 1141 (`sub_820E8B10`).

## 2. How a costume is worn

`src/engine/costume_system.cpp` keeps a list per character, the default first.
Wearing a costume writes its model's guest address into the character's slot,
so whatever reads the slot next builds it, exactly what the PS3 does with its
`WEAR*` slots. Battles and events build their models when they start; the
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
character = "polka"                 # or 1..10, the party numbering
name = "swimsuit"                   # the id becomes "<mod folder>/swimsuit"
label = "Swimsuit"                  # what the overlay shows
model = "costumes/plk_swim.nobj"    # relative to the mod folder
```

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

## 5. Choosing at boot

```
--costumes=polka=ps3/3,allegretto=my_mod/swimsuit
```

Character names are the lowercase English ones (`allegretto` .. `march`) or
their numbers. The ids are those `EternalSonataGetCostumeId` reports:
`default`, `ps3/2` and `ps3/3` for the PS3's, `<mod folder>/<name>` for an
`assets.toml` costume.

## 6. The PS3's costumes

On PS3 data the host registers `pc{alg,plk,bet}_v2.p3obj` and `pcplk_v3.p3obj`
as `ps3/2` and `ps3/3`, labelled as the PS3's menu names them. PS3 scripts ask
for the costume through native 5028, which answers the PS3 variant of the
costume worn and 1 for anything else, and events use ids -10..-12 for the
worn costume of Allegretto, Polka and Beat, which native 1141 reads from the
same slots ([ps3-assets.md](ps3-assets.md) §2).

Not yet: unlocks gating the list (`lib.e` grants them through 5026, kept in
host memory only), saving the choice, and the camp menu page.
