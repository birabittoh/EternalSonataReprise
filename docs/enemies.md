# Enemies

Guest-side layout of an enemy's stats, and how the host exposes them.

API: [`src/eternalsonata_enemy_api.h`](../src/eternalsonata_enemy_api.h),
implemented in `src/enemy_system.cpp`, offsets in `src/battle_layout.h`. For the
encounter those enemies are fighting in, see
[`src/eternalsonata_battle_api.h`](../src/eternalsonata_battle_api.h).

## No per-type table

There is no master table keyed by enemy type. The stats exist only in the
per-instance record, populated when an encounter loads. A per-type rebalance
therefore has to be reasserted on every enemy of every battle;
`EnemySystemTick()` is that loop.

## The record

Enemy records start at `unk_82539240` (battle manager + 429568), stride 32456.
The live enemy count is the byte at `byte_824D0721`.

A record is two 16136-byte **part** sub-records back to back, one per boss form,
with an index selecting the live one. `sub_8219F4B8` memsets 32456 bytes and
initialises exactly two parts through `sub_8219F408`.

```
record + 0                  part 0
record + 16136              part 1
record + 32272 (0x7E10) i32 live part index, 0 or 1
record + 32280 (0x7E18) f32 cached current/max HP ratio
record + 32450          u8  set when the second part is in use
```

Every "which part" computation in the game is the same expression, e.g. in
`sub_8224EBD0` and `sub_821C48D0`:

```
part = record + 16136 * *(i32*)(record + 32272)
```

Because of that indirection, only two constant-displacement cross-references
from the array base exist (`sub_8218F038` and `sub_821BAB70`, both on
`unk_8253936C`). Sweeping for constants finds nothing else; sweep for
displacements off a register in functions that already hold a `part`.

`part + 0` is the type's name, a NUL-terminated string, one per part.
`sub_8219F698` formats `btldata\script\ai\<name>.e` from it to load the AI
script.

## The stat block

The 168-byte stat block is at `part + 300`. `sub_8218F038` exists only to
`memcpy` it out to the script VM (`sub_822CF5B0(dst, part + 300, 168)`), fixing
both its base and its length.

| Offset | Type | Field |
|---|---|---|
| `part + 300` | i16 | name id, 1-based |
| `part + 302` | i16 | level |
| `part + 304` | i32 | current HP |
| `part + 308` | i32 | maximum HP |
| `part + 312` | i16 | attack |
| `part + 314` | i16 | speed |
| `part + 320` | f32 | chase range |
| `part + 324` | i16 | defense |
| `part + 326` | i16 | AI reaction cone, low angle, degrees |
| `part + 328` | i16 | AI reaction cone, high angle, degrees |
| `part + 330` | i8 | physical resistance, percent |
| `part + 331` | i8 | magic resistance, percent |
| `part + 339` | i8 | critical rate, percent |
| `part + 340` | u32 | flag bits |
| `part + 344` | i32 | EXP awarded |
| `part + 348` | i32 | gold awarded |
| `part + 352` | i16 | drop item 1, master entity id |
| `part + 354` | i16 | drop item 2, master entity id |
| `part + 356` | i8 | drop 1 chance, percent |
| `part + 357` | i8 | drop 2 chance, percent |
| `part + 358` | i16 | drop flags |
| `part + 360` | f32 | model scale |
| `part + 364` | f32 | move range |

### Evidence

**Name id.** `sub_821ABE88` resolves display names for both sides from one
descriptor: a party member's character id against the BTX block at `0x823857D0`,
an enemy's i16 at `part + 300` against `0x82332D90`, both as `index - 1`.

**Level.** `sub_821BAB70`'s chatter picker takes the maximum over enemies of
`SLOWORD(*(i32*)(part + 300))`. Big-endian, so the low half of that dword is the
i16 at +302, not the one at +300 that `sub_821ABE88` reads. The two share a dword
and are easy to conflate.

**Current HP.** `sub_8224EBD0`'s "target the weakest" sort takes the minimum of
the party record's current HP and, for an enemy, `part + 304`, in one comparison
over both sides.

**Maximum HP.** `sub_821B3FC0` multiplies `(float)*(i32*)(part + 308)` by the
ratio at `record + 32280` and tests the product against 10.0. That product is
only HP if the field is the ratio's denominator.

**Attack, defense, resistances.** `sub_821AFF40` is the damage formula. It reads
attacker and defender stats side by side, branching on unit kind per term, so
each enemy field is pinned to the party field in the same slot of the
expression; `src/party_system.cpp` names those.

```
attacker term 1   enemy part+312   party stats+0x14 (attack)
attacker term 2                    party stats+0x16, scaled by (1 - part+330/100)
attacker term 3                    party stats+0x18, scaled by (1 - part+331/100)
defender          enemy part+324   party stats+0x18 (defense)
```

The party's attack is built from three terms and the enemy's from one: there is
no enemy magic attack, because an enemy's spell power comes from the ability
rather than a stat. There is no enemy magic defense either; `part + 324` defends
against everything, and the two resistance bytes are what separate physical from
magical damage.

**Speed.** `sub_821AB630` computes turn order, sorting party members by
`stats+0x1A` and enemies by `part + 314`, with the same ±10% jitter on both.

**Critical rate.** `sub_821AF7D0` rolls `rand() % 100` against `part + 339` plus
an equipment bonus; `sub_821AFF40` multiplies the final damage by 1.5 on a hit.
It is read off the defender, not the attacker.

**Flag bits.** `sub_821C48D0` tests bit 10 alongside the HP ratio for
targetability; `sub_821AED48` tests bit 17 to select the alternate ability list.
Same field as `record + 0x154` in older code.

**EXP and gold.** `sub_8218B6B8` builds the results screen. It sums `part + 344`
over live enemies into the results record, where equipment effect id 47 scales
it, and sums `part + 348` into the money global `dword_8243F3F0`, saturating at
99999999.

**Drops.** `sub_8218BA70` rolls `rand() % 100` against `part + 356` for the item
at `part + 352`, and against `part + 357` for the item at `part + 354`.
`part + 358` is a flag word: bit 0 (item 1) and bit 1 inverted (item 2) restrict
that drop to the enemy the battle picked as featured. A battle yields at most
three items in total however many enemies it holds, and the encounter record
contributes one of the three.

**Movement and scale.** `sub_821B24C0` reads `part + 360` as the model's scale
vector. `sub_82196C58` places a move's destination at
`start + (target - start) * (part + 364)`. `sub_82191EC8` passes `part + 320` to
the target search as a radius; the party's counterpart is `stats+0x24`.

## The pristine copy at `part + 468`

Attack, defense and speed are the three buffable stats, and each has an
untouched copy 168 bytes further on:

```
part + 480   base attack     (sub_821AFD08)
part + 482   base speed      (sub_821AFDF8)
part + 492   base defense    (sub_821AFD80)
```

`sub_821B8368` adds a buff delta into the live field, having first passed it
through `sub_821B8090` / `sub_821B7F60` / `sub_821B7E30`, which clamp the result
to `base * [0.7, 1.3]`. Anything writing one of the three has to write the base
copy too, or the next buff drags the value back to within 30% of the original.
`WriteStat` in `src/enemy_system.cpp` does.

## The HP ratio invariant

`record + 0x7E18` caches current/max HP as a float. Most liveness checks read it
rather than the counter: the battle-over predicate `sub_821B7450`, the
low-health chatter `sub_8218F4C8`, and the targetability checks in
`sub_82190900`, `sub_8219D330` and `sub_821CEB78`.

The game maintains `max * ratio == cur`. Anything moving either HP field must
rewrite the ratio.

## Host behaviour

The block is exposed as a flat list of stat ids, so a new stat is an additive
change rather than a new exported symbol. The three float fields are exposed as
percentages (100 = as shipped) so one integer ABI covers everything.

* **Overrides compute from a snapshot.** The host records each enemy's untouched
  stats the first time it sees the record in a battle, keyed by slot and part.
  A ×1.5 multiplier reapplied per frame stays ×1.5; clearing an override
  restores the original value. A record counts as new when its name id changes,
  or when the battle ends.
* **Both parts get the override**, so a two-form boss is rebalanced in each form.
* **Current HP is applied once**, when the record is first seen, rather than per
  frame; reasserting it every frame would make an enemy unkillable. It means the
  health the enemy enters battle with.
* **Setting maximum HP rescales current HP** to hold the ratio, so ×2 HP is a
  fight twice as long rather than one that starts half over.
