# Enemies

Guest-side layout of an enemy's stats, and how the host exposes them.

API: [`src/api/eternalsonata_enemy_api.h`](../src/api/eternalsonata_enemy_api.h),
implemented in `src/engine/enemy_system.cpp`, offsets in `src/engine/battle_layout.h`. For the
encounter those enemies are fighting in, see
[`src/api/eternalsonata_battle_api.h`](../src/api/eternalsonata_battle_api.h).

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
expression; `src/engine/party_system.cpp` names those.

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
`WriteStat` in `src/engine/enemy_system.cpp` does.

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
* **Player multipliers stack on top.** The `enemy_exp_multiplier`,
  `enemy_gold_multiplier`, `enemy_hp_multiplier` and `enemy_damage_multiplier`
  cvars scale every enemy's EXP, gold, max HP and damage after mod overrides resolve, so a mod rebalance and the
  player's setting compound instead of one replacing the other. They are a
  separate layer rather than an `ANY` override because a type's own rule would
  otherwise hide them. Mods read them with `EternalSonataGetEnemyGlobalMultiplier`.
  EXP-bonus equipment still applies after this.
* **Damage is a host-held stat.** `ETERNALSONATA_ENEMY_STAT_DAMAGE_PCT` has no
  guest field: the host keeps it per part and multiplies what `sub_821AFF40`
  returns when that enemy is the attacker, after criticals and guards, then
  reapplies the formula's 99999 cap. Scaling attack instead would not be
  linear, since the formula subtracts defense from it. Overrides, per-instance
  writes and the player multiplier all work on it like any other stat.

## Guarding and countering an enemy attack

When an enemy swings at a party member, the defender can guard with B or, from
party level 4, counter with A. Both are timed against a window the enemy's move
record carries, and the "chance" mark that pops up over the defender is that
window opening. Addresses below are off the battle manager `dword_824D0440`
unless they say otherwise; party record offsets are from the record start
(`battle_layout.h` `PartyRecord`).

### Buttons

`sub_821A8FD8` allocates a 20-byte button map and stores its pointer at manager+533144 (`0x82298`). The
masks are XInput button bits, tested against the pad's pressed-this-frame
mask by `sub_82199580`:

| Offset | Default | Use |
|---|---|---|
| +0x00 | `0x1000` A | confirm; counter |
| +0x04 | `0x2000` B | guard |
| +0x08 | `0x8000` Y | |
| +0x0C | `0x4000` X | |

`byte_8243FC01` swaps the first two. `sub_82189FD0` lights the HUD hint for the
guard button only while the guard window below is open.

### Arming

Every attack motion the enemy FSM `sub_821C0300` starts (the eight blocks that
set motion `0x22`, `0x23`, ...) does three things in order:

1. Clears, on every party record, the input byte (+81848), the outcome latch
   and outcome (+81772, +81776) and the counter request (+81797).
2. Rolls the enemy's counter state, `sub_821BED88` (see "Counter state").
3. Arms the cue: `sub_821BFF10` hands the actor's move record (vtable +20) and
   target list (vtable +24) to `sub_821A6818`.

`sub_821A6818` only arms when the move record's kind (+24) is 1, the target
list is non empty, its first target is a party member, and that member is
facing the attacker: the angle between its scene node's forward axis
(`sub_8217C368`) and the direction to the enemy's node has to be below the u16
at party record +81514 (manager+265162), in degrees. Arming sets the cue to
state 1 and zeroes its clock.

### The cue

The cue object lives at manager+533180 (`0x822BC`, `unk_825526FC`). The battle
tick `sub_8218BC50` runs `sub_821A69B8` on it every frame.

| Offset | Type | What |
|---|---|---|
| +0x00 | u32 | UI animation player |
| +0x04 | i32 | live animation instance, -1 for none |
| +0x08 | f32[3] | world position of the mark |
| +0x18 | i32 | state: 0 idle, 1 armed, 2 open, 3 closed |
| +0x1C | f32 | clock since arming |
| +0x20 | u8 | attacking enemy's slot |
| +0x24 | ptr | move record |
| +0x28 | ptr | target list (`{kind, slot}` entries, count at +64) |
| +0x2C | f32 | slow motion countdown |

The clock advances by `sub_82181728(&flt_824D0130, 0)`, the channel 0 game
clock, which counts 300 per second (`300 / fps` per frame, times the channel's
scale). The window is `[start, start + length]`, the floats at move record +12
and +16, in the same 1/300 s units.

* **Armed, clock inside the window.** If the target can guard right now (party
  controller vtable +0x90, `sub_821C4F38`, below), the cue opens: it creates the
  mark, guard only (`sub_821A6668`, asset at manager+0x2CAC8) or the counter
  version (`sub_821A6740`, manager+0x2CACC) when the attacker's counter state is
  1 and the counter window predicate holds, and plays its `FDIN` label. If the
  target cannot guard, the cue drops to state 0 and nothing shows for that
  swing.
* **Open, clock past the end.** Plays `MIOT` (miss out) and goes to 3.
* **Counter mark.** `sub_821A6740` also sets the countdown at +0x2C to 50.
  While it is positive, `sub_8218BC50` writes 0.5 into the channel 0 clock
  scale (`0x824D0134`) instead of 1.0: the battle runs at half speed while the
  counter chance is up.

The two marks are `BattleKeep.bop` slots 70 and 71 (manager+0x2C9B0 is the
slot array). Their chime is the mark's own `SE_0` bank, started by a sound
layer of the effect: screen space effects play through `sub_821428B0`, which
refuses a bank that is not on a 0x1000 boundary.

### Reading the press

`sub_821C6E68` (vtable +0xA0 of the party controller `off_82087108`) reads the
pad every frame. The party record fields it drives:

| Offset | Manager offset | Type | What |
|---|---|---|---|
| +81772 | 265420 | u8 | outcome latched for this swing |
| +81776 | 265424 | i32 | outcome: 0 hit, 1 guard, 2 counter |
| +81797 | 265445 | u8 | counter requested |
| +81848 | 265496 | u8 | input: 0 none, 1 timed, 2 locked out |

Two predicates gate it. Both require that it is not this member's turn, that the
acting unit is an enemy, that the member's HP is above zero, and that the
member is in the current action's target list (`sub_821AF5C8`).

* **Guard window**, `sub_821C4F38`: controller state (+4) is one of 8, 9, 25
  to 29, 48, 49, and the input byte is still 0.
* **Counter window**, `sub_821C5038`: no status 8; the attacker's current
  action is counterable (`sub_821AEFD0`: action code 11, 13, 15, 17, 31, 46,
  47, 63 to 65, 69, 71, 73, 75 or 77, and move kind 1 or 5); controller state
  is not 33 to 35; and the attacker's counter state is 1. It does not look at
  the input byte.

With "open" meaning the cue's state was 2 at the start of the frame:

| Press | Window | Cue | Result |
|---|---|---|---|
| B | guard | open | input 1, mark plays `SUOT` (success out) |
| A | guard | open | input 2, mark plays `MIOT` |
| A or B | guard | not open | input 2 |
| A | counter | open | input 1 and counter requested, `SUOT` |
| B | counter | open | unchanged (the guard row already ran) |
| A or B | counter | not open | input 2 |
| A or B | any | not open, input already 1 | input 2 |

A handled press also moves the controller to state 20. The guard window needs
the input byte at 0, so pressing before the mark appears locks the member out
for the whole swing: when the window then opens, the guard predicate fails and
the cue never shows. Pressing again after the mark is gone throws a successful
guard away.

### Resolving the hit

The damage formula `sub_821AFF40` calls `sub_821B38A8` on the defender, which
for a party member asks `sub_821AF670`. The latch at +81772 makes the answer
stick for every later hit of the same swing:

* input 1, the attacker an enemy with counter state 1, and a counter requested:
  outcome 2;
* input 1 otherwise: outcome 1;
* anything else: outcome 0, and the mark plays `MIOT` if it is still up.

| Outcome | Effect |
|---|---|
| 0 | normal hit reaction (`sub_821AA568`), full damage |
| 1 | damage x 0.25 (`sub_821AF2F8` tests the outcome field) |
| 2 | counter: manager+537177 = 1 and the formula returns 0 damage. The defender is flagged in the turn sequence (manager+533120 entry, +24), and the turn object at manager+533100 switches to mode 3 (+6464) with a time budget of max(50, its timer at +8480 / 6) in +8484. |

The counter flag at manager+537177 then runs through turn setup (battle FSM
state 6): 1 becomes 2 for the counterattack turn itself, and 2 becomes 0 the
turn after. While it is 2, `sub_821AFCA0` gives the turn a flat 600 (2 s)
instead of the party level's command time, and `sub_82189FD0` hides the Y
hint below party level 5.

A party defender that did not guard itself still takes the 0.25 and plays guard
motion 25 when any other party member's input byte is 1 (`sub_821AF4C8`), so one
well timed guard covers the whole party against a multi target swing.

Enemies guard too: their outcome lives at enemy record +32428 (manager+461996),
set by the AI, and `sub_821AF278` cancels it when the attacker's flag (party
record +81780, enemy record +32449) is set.

### Counter state and party level

`sub_821BED88` keeps a per enemy counter state, the i32 at enemy record +32392
(`+0x7E88`):

| Value | Meaning | On the next swing |
|---|---|---|
| 0 | not rolled | roll |
| 1 | this swing is counterable | becomes 2 |
| 2 | not counterable | unchanged |
| 3 | counters unavailable | unchanged |

The roll gives 3 when the party level forbids counters or the actor has no move
record, otherwise 1 when `rand() % 100` is at most the move record's i16 at +22
(a percent chance) and 2 above it. Turn setup (battle FSM state 6) zeroes the
field on every enemy, so each enemy turn rolls once on its first swing, and a
counterable swing makes the rest of that turn's swings uncounterable. The same
pass clears both sides' guard outcomes.

The party level gate is a six row table at `0x8238E2B8`, 20 bytes per level,
indexed by level - 1. Byte +3 is 0, 0, 0, 1, 1, 1, so counters start at party
level 4. The level that indexes it is the low byte of the u32 at `0x8243F3E8`;
see "Party level and its budget" in [party-system.md](party-system.md).

### Auto parry

The `auto_parry` setting (Balance tab) hooks `sub_821C6E68` and rewrites that
pad's pressed word at `0x824BB5C4 + 464 * pad` for the call, then restores it.
While an enemy acts and it is not that member's turn (party controller +8), it
drops the player's own A and B, because an early or late press locks the member
out; on the member's own turn A and B drive its attack and guard stance and are
left alone. Once the cue is open and the input byte is 0 it adds the A mask
(counter window open) or the B mask (guard only, so counters not unlocked). The
game then handles it as a real press.

### Host side

`battle_system.cpp` reports a hit as parried from the defender's own outcome
(`battle_layout.h` "Guard and counter"), not from the counter flag: the flag
stays nonzero through the whole counterattack turn, so every hit of the strike
back would read as parried. `ETERNALSONATA_BATTLE_EFFECT_PARRIED` means outcome
1 or 2, and `ETERNALSONATA_BATTLE_EFFECT_COUNTERED` is added for 2. The party
side also requires the outcome latch, so an outcome left from an earlier swing
is not reported against a later hit.
