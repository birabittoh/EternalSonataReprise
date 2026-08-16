# The party system

How Eternal Sonata stores its party, and what this project does with that.

The reverse engineering below used to live in the `party_overlay` mod, which
re-derived it all from raw guest addresses. It lives here now: the exe reads and
writes party state itself and exposes it to mods through
[`src/eternalsonata_party_api.h`](../src/eternalsonata_party_api.h), implemented
in `src/party_system.cpp`. A mod should never need an address from this page;
it is here so the next person can check the implementation against the binary.

## Ten retail slots, and two vacant ones

The retail cast is ten characters, numbered 1..10 in the game's own order:

| # | Character | # | Character |
|---|---|---|---|
| 1 | Allegretto | 6 | Salsa |
| 2 | Polka | 7 | Jazz |
| 3 | Beat | 8 | Falsetto |
| 4 | Frederic | 9 | Claves |
| 5 | Viola | 10 | March |

Every per-character table is keyed by that number, and in the retail image ten
is a hard limit rather than a convention: the tables are ten entries wide and
`sub_821E7898`, which computes the stats every screen draws, returns without
doing anything for an index outside 1..10. The tables cannot simply be widened
in place:

* Every per-character array is exactly ten entries and butts straight up against
  the next one. The equipment-adjusted stats at `0x8243FD08` are ten 48-byte
  structs, and `0x8243FD08 + 480` is `0x8243FEE8`, where the character's own
  stats begin. The position table at `0x8243FC08` is ten `u32` and ends exactly
  at `0x8243FC30`, which is live data too. There is no padding anywhere to grow
  into.
* Their base addresses are materialized constants in roughly two hundred
  instructions spread over about sixty functions (`0x8243FC08` alone has 60+
  code references, `0x8243FD08` 44, `0x8243FEE8` 29), including
  `sub_8221B5A0`, which is 28 KB on its own. Relocating the arrays to a wider
  block would mean reworking every one of them.

So a twelve-character party is not reachable by extending the game's tables
where they are. The port gets there anyway, by relocating the two coldest
arrays into host-owned guest memory so the two hottest can grow into the holes,
and by rewriting every site that materialises a moved base with a mid-ASM hook.
That machinery is described in `src/party_relocation.h`, and the loop counts,
save spans and id gates that come with it in `src/party_counts.cpp`,
`src/party_save.cpp` and `src/party_bounds.cpp`. The bound checks
(`sub_821E7898` rejecting anything outside 1..10, `sub_821A03D0` rejecting
`id - 1 > 9`) were the least of it; the data layout was the wall.

### The two added slots are vacant, not new characters

Characters 11 and 12 are real slots in every widened table, and nothing more.
The host puts no content in them: no name, no stat template (their template
entries are left zeroed), and the three id gates above stay shut for them. With
no mod loaded the game therefore behaves exactly like the retail
ten-character one.

A mod fills a slot in through `EternalSonataDefineCharacter` (or
`EternalSonataDefineNextCharacter`, which claims whichever slot is free so two
such mods can coexist). Defining a slot:

* records the name, published through the same rename path a renamed retail
  character uses, so every menu screen resolves it;
* seeds the slot's entry in the relocated stat template table from a retail
  character's, which is what gives it starting stats and growth curves;
* opens the id gates for that one slot, by setting its bit in the registry the
  hooks in `src/party_bounds.cpp` and `src/party_counts.cpp` read.

`src/party_slots.cpp` is the registry, and it is the only place that knows
which ids are added ones; everything else asks it rather than testing against 11
and 12. Undefining a slot reverses all three steps and takes the character out
of the party if it is in one.

The battle model is the one piece a mod is expected to ship itself: an asset mod
overlays the game partition, so `mods/<name>/game/btldata/player/pc011.bop`
gives character 11 a model, and `sub_821A03D0` asks for `pc%03d.bop` by
character id. A definition that has no model yet can point `model_id` at an
existing character's instead. `../EternalSonataReprise-Mods/src/demo_characters`
is the worked example.

Definitions are host state, not save state: a mod defines its slots on every
run.

Note the ordering: Polka is 2, Beat 3, Frederic 4. An earlier revision of the
overlay had 2/3/4 as Beat/Frederic/Polka because it validated names against max
HP while misreading the position table; the binary's own order is the one
above, and it is the order the name strings are stored in too.

## Position, not order

`dword_8243FC08` is a `u32[10]`, and its value is **character c's 1-based
display position**, `0` meaning "not in the party":

```
dword_8243FC08[c - 1] = display position of character c
```

It is *not* a list of character ids in screen order. Evidence:

* `sub_820E78B8` (the party menu's add) writes `FC08[c-1] = (number of nonzero
  entries) + 1`: it appends the new member at the next free *position* while
  leaving its slot fixed.
* `sub_820E7948` (remove) and `sub_821E6428` both locate a member with "walk
  FC08 until `*p == P`, take the 1-based index" - a reverse lookup of "who is
  at position P", which only type-checks if the stored value is a position.
* IDA's `dword_8243FC04` is a 1-based alias of the same words: `FC04[c]` and
  `FC08[c-1]` are the same four bytes. There is no second table. An earlier
  revision could not reconcile the two accessors; the contradiction was purely
  this aliasing.

The **active party is positions 1, 2 and 3**, which is exactly what
`sub_821E6428` searches for when it rebuilds the battle party. It is not a
fixed set of slots. `byte_8243FC3A[3]` looks like the natural place to find the
active set, but nothing in the binary ever writes real values into it, which is
why `sub_821E6428` always takes its fallback path and searches FC08 instead.

`word_8243FC3E` (`u16[32]`, party base +0x856) looks like a menu roster of
recruited ids, and `sub_821E6740` does append to it, but it is only ever
written by a live join during the current run and never restored from a save
(the reset paths `sub_821E5D68` / `sub_821E5A38` just zero it). After loading a
save where everyone was recruited in a past session it holds whatever the heap
left there. Do not read it to answer "who is in the party"; read FC08.

## Stats

Two parallel arrays of 48-byte structs, both indexed by character number - 1:

| Address | What |
|---|---|
| `0x8243FEE8` | the character's own stats, what a save holds |
| `0x8243FD08` | the same with equipment folded in, what the screens draw |

`sub_821E7898(c, 0x8243FD08 + 48*(c-1))` recomputes the second from the first:
it copies the struct across, walks the character's five equipment ids through
the master entity table at `0x82017630` (stride 100, id at +0, party-level cost
at +0x36, table ends at `0x82023DCC`) adding each item's bonuses, clamps, and
finally rescales current HP by however much maximum HP moved.

Struct layout, confirmed against the equipment screen's own painter
`sub_822352F8`, which draws `+0x10`, `+0x0C`, then `+0x14/+0x16/+0x18/+0x1A`:

| Offset | Type | Field |
|---|---|---|
| `+0x00` | u32 | level |
| `+0x0C` | u32 | current HP |
| `+0x10` | u32 | maximum HP |
| `+0x14` | u16 | attack |
| `+0x16` | u16 | magic |
| `+0x18` | u16 | defense |
| `+0x1A` | u16 | speed |
| `+0x1C`..`+0x25` | u16[5] | equipment ids |
| `+0x2C`, `+0x2E` | u16 | two further capped stats, not drawn by the equipment screen |

The four u16 stats are clamped to 999 by the game itself, so writes clamp the
same way rather than storing a number the next recompute would cut down.

That rescale is why `EternalSonataSetCharacterStats` writes *both* structs
before asking for a recompute: with the new maximum already in the live struct
the ratio is exactly 1, so the recompute only adds the equipment bonus back
instead of moving current HP around.

## Party level and its budget

| Address | What |
|---|---|
| `0x8243F3EC` | u32 party level, 1..6 (low byte at `0x8243F3EF`) |
| `0x8202CA70` | `u16[]` budget cap, indexed by level - 1 |
| `0x8243FCC4` | u8 budget left |
| `0x8243FCC5` | u8 budget spent |

Each character costs a fixed amount (master table +0x36) and can only join
while the remainder covers it; `sub_821E6740` keeps the two bytes summing to
the level's cap.

## Joining and leaving

Joining is three steps, and the first is the one that is easy to miss:

1. `sub_821FBFC0(&dword_8255EED8, id, 1, 0)` - **own** the character.
   Characters share an id space with items in the owned-entity table at
   `0x8255EF08` (512 records of `{u16 id, u8 count, u8 spoken for}`, count at
   `word_8255FF08`), and the roster add's gate `sub_821FBF20` is really "do you
   have one of these". Anyone the story has not handed you yet fails it with
   code 3. This table is the "party-member DB" an early version of the overlay
   displayed raw; it is an inventory, and showing its bytes was never
   meaningful.
2. `sub_821E6740(id)` - the roster add: validates against the master table,
   charges the party-level budget, updates the counters. Returns 0 added,
   1 roster full, 2 budget insufficient, 3 not owned.
3. `sub_820E78B8(&index)` - gives the character the next free display position
   and rebuilds the battle party. `sub_821E6740` does *not* do this; a join
   that stops after step 2 leaves a member the status screen cannot see.

Leaving is `sub_820E7948(&index)`: it shifts everyone behind the leaver down a
position, clears the leaver's own, and rebuilds the battle party. It does not
touch the roster or the budget, so a removed character can be added back.

Both take a **pointer** to the character index (id - 1), not the index, which
is why `party_system.cpp` keeps a small guest scratch buffer.

`sub_821E6428`, the battle-party rebuild both end in, repacks
`unk_824D0480` / `unk_824D13C0` from the position table. Its file-I/O legs are
gated on battle data being loaded, so outside a battle it degenerates to memory
operations.

### Not during a battle

The whole join sequence crashes mid-battle with
`STATUS_FLOAT_INVALID_OPERATION` (0xC000008F), reproduced under lldb: deep
inside `sub_821E6428`'s battle-model maths, bottoming out in a `vmaddfp` that
reads what looks like an uninitialised transform for the new character's battle
object, because it never went through the game's own `.bop` load path. Every
mutating entry point therefore refuses with
`ETERNALSONATA_PARTY_ERR_IN_BATTLE` while a battle is running.

Battle state is *not* derivable from a single guest-memory poll - that was
tried and it was wrong (`dword_8244B9A0` is a live heap pointer, not a magic
value, and reads nonzero outside battle too). The answer comes from the same
hook-driven tracking Discord presence uses, `RoomPresence::IsBattleActive`.

## Names

Character names come from the packed UI message blob in the xex, the same one
the Options screen's labels live in (see `docs/debug-hooks.md` §14). Each
language block starts with the ten names, twice: once plain, once prefixed with
the `<r>` ruby marker. Six blocks ship in the retail image; the Italian one
spells Viola "Arpa" and Falsetto "Mazurka", so only the first four names are
common to all of them.

That blob is not the only copy. A second family of blocks sits near
`0x82385900`, each carrying its own `{id, byte offset}` index in front of the
packed strings, and one of them calls the fourth character Chopin rather than
Frederic. These have no code cross-references at all: they are reached through
computed pointers from record tables, and they are what the battle side reads,
which is why overriding the text lookup alone left a renamed character called
"Claves" once a battle started. A third table at `0x8209EC00` is not UI text but
localized asset name stems (`alg_`, `plk_`, `mch_`, ... crossed with `Itary`,
`German`, `France`, `Spain`).

Every menu screen resolves a name through `sub_8223B780(blob, string_id) -> char*`,
which the project already hooks for the Options rows. Renaming a character is
therefore not a matter of patching the string in place (which would cap the new
name at the old one's length): the hook recognises the address the stock lookup
returned and answers with a host-allocated guest string instead, in whichever
of the two forms - plain or `<r>`-prefixed - was asked for.

The lookup override cannot reach the readers that copy bytes out of a block
directly, the battle HUD among them, so a renamed character still shows its
built-in name there. Nothing patches those bytes in place: the game is built
around a fixed cast, and rewriting its data to disguise one character as another
is not something this project does.

The blocks are located by scanning for the names themselves rather than by
hardcoded address, because this title applies a title-update delta patch over
the base image on every launch and addresses past a patched region can shift.
A candidate is only accepted if all ten strings after the anchor look like
names, so a coincidental hit cannot lead the patcher into unrelated bytes. The
scan runs once, and only after some mod has actually renamed somebody.

## What is deliberately not modelled

* **Content for the added slots.** The host widens the tables and holds two
  vacant slots; it does not invent a character to put in one. Names, stats and
  assets come from a mod, through the definition API above.
* **Field models.** Who walks the overworld is a separate mechanism entirely;
  see `src/field_player_model_override.h`.
