# Modded characters

On Xbox 360 data the party is twelve wide ([party-system.md](party-system.md),
"Twelve wide storage") but slots 11 and 12 are vacant: no name, no model, and
the party API refuses them. A mod fills a slot by defining a character in it.
On PS3 data Crescendo and Serenade hold both slots and nothing can be defined.

`src/engine/character_roster.cpp` is the one list of who each slot is: the
built-in cast's names and tokens, the PS3 pair's localized names, and the
definitions. Every other file asks it rather than spelling the cast out.

## What a definition is

A character brings what it has and takes the rest from a **base**, one of the
ten retail characters:

| Part | Own | Otherwise |
|---|---|---|
| Name | `name` | (required) |
| Stat template, growth | | the base's row of `0x82016150` |
| Starting stats | `level`, `hp`, ... | the template |
| Equipment it may wear | | the base's (master table bit `2 + base` copied to `2 + c`) |
| Magic | | the base's records: same ids, levels, costs and order |
| Body: field, battle, events | `model` (NOBJ) | the base's model |
| Battle scene name | `scene` | the base's (`bJRB` for Jazz) |
| Battle file: motions, cameras, effects, specials | `pc011.bop` shipped, or `battle_file` | the base's `pcNNN.bop` |
| Voice bank | `pc011.csf` shipped, or `voice_file` | the base's |
| Status, panel, face portraits | `portrait`, ... | the base's art |
| Cloth chains, hit motion set, motion blend rows | | the base's case |
| Battle HUD portrait | | the base's group |
| Costumes | `[[costume]]` like anyone | |

The definition's model and portraits are the slot's **default costume**, so
the costume system loads and swaps them like any other: costumes for a modded
character work on every screen a retail character's do, X on the status page
included.

## Declaring one

With no code, from `mods/<name>/assets.toml`:

```toml
[[character]]
name = "Cadenza"                    # CP1252, what every screen draws
key = "cadenza"                     # optional; id "<mod folder>/cadenza"
slot = 11                           # optional; the first free one otherwise
base = "jazz"                       # or 1..10
model = "chars/cadenza.nobj"        # optional, Xbox 360 NOBJ
scene = "bCDZ"                      # optional battle scene name
battle_file = 7                     # optional: borrow pc007.bop
voice_file = 7                      # optional: borrow pc007.csf
portrait = "chars/cadenza_status.dds"          # optional, see costumes.md
panel_portrait = "chars/cadenza_panel.dds"     # optional
face_portrait = "chars/cadenza_face.dds"       # optional
small_face_portrait = "chars/cadenza_face2.dds" # optional
level = 12                          # optional starting stats
hp = 900
attack = 60

[[costume]]
character = "cadenza"               # the key, once the character exists
name = "winter"
model = "chars/cadenza_winter.nobj"
```

A battle file of its own is an ordinary whole file replacement: ship
`assets/btldata/player/pc011.bop` (and `assets/btldata/voice/pc011.csf`) for
slot 11. The host loads the slot's own number whenever the game data or a mod
has that file, so a mod that does not know its slot in advance should name it
with `slot`. With neither, the base's files load.

From C++, `EternalSonataDefineCharacter` / `EternalSonataDefineNextCharacter`
in [`eternalsonata_party_api.h`](../src/api/eternalsonata_party_api.h) take the
same fields; define from `OnModuleLaunched`. `[[costume]]` tables naming a
character a code mod defines later wait for it. `demo_characters` in
EternalSonataReprise-Mods is the worked example.

## Files

* Model: a complete `NOBJ` chunk in the Xbox 360 layout, as for costumes
  ([costumes.md](costumes.md) §3). Its bones must match the motions it plays:
  its own battle file's, or the base's.
* Portraits: DDS or NTEX, sizes as in [costumes.md](costumes.md) §4. The one to
  three member panel of a modded character takes its base's case in the
  builders' switches, so its frame is the base's: its own panel art (from a
  Viola or later base, its status art), else the base's.

## Magic

Each magic record (`0x82015380`) has one owner. A modded slot is treated as
its base's owner by the three readers: the equipment list `sub_821E93B0`
(owner check and `word_8202C8A8` row), the level up list `sub_821E8930` and
the equipment API (`equipment_system.cpp`). The level each record unlocks at
is checked against the modded character's own level. Joining with no magic
equipped gives it the first light and dark magic it has learned.

## Saves

The guest save holds ten characters; slots 11 and 12 live in the slot's
`reprise.txt` (`party.*` keys) together with `roster.11` / `roster.12`, the id
that held each slot. Definitions themselves are not saved. A load that finds a
different character in a slot logs a warning and keeps the numbers, so a mod
should keep its slot stable (`slot`, or a fixed load order). Costumes are keyed
by the character's key, not its slot.

## Not done yet

* **Magic of its own.** A modded slot casts its base's records, so its battle
  file must key its actions by the base's magic ids. Records of its own would
  need appended records (raising the count as `EternalSonataPs3MagicCount`
  does), a twelve row order copy and text for the new ids.
* **Events.** Every retail event names its cast with constant model ids
  (`lib.e` symbol 20043, native 1141), never through a party lookup, so a
  modded character appears in none. Showing one needs a mod's own event
  script and a 1141 id for it; the negative ids only cover the PS3 pair.
* **Battle HUD art of its own**: the HUD portrait is a group of BattleKeep slot
  41's layout, built the way the PS3 pair's are lent (`party_battle.cpp`).
* **Cloth chains for an own model.** A modded character with its own model has
  none; the base's chain names would not match its meshes.
* **Save rows** show the base's face.
