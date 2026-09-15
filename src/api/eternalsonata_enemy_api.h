// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's enemies: what a monster hits for, what it takes
// to kill one, and what killing it is worth. This is the rebalance API.
//
// It is a separate ABI from eternalsonata_battle_api.h on purpose. That one
// describes a battle that exists for a few seconds and is gone; this one
// describes the numbers behind every future encounter, and the two version
// independently. Where they overlap (an enemy's current HP), the battle API
// stays the place to look, because it is a property of the battle rather than
// of the monster.
//
// This exists so a mod never has to know a single guest address. The stat block
// is reverse-engineered in src/battle_layout.h; the host owns the offsets, the
// clamping, and the reapplication.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the item API is used (see eternalsonata_item_api.h):
//
//     auto set = reinterpret_cast<EternalSonataSetEnemyTypeStatMultiplierFn>(
//         GetProcAddress(GetModuleHandle(nullptr),
//                        "EternalSonataSetEnemyTypeStatMultiplier"));
//     if (set) {
//       // every enemy hits 50% harder, for the rest of the session
//       set(ETERNALSONATA_ENEMY_TYPE_ANY, ETERNALSONATA_ENEMY_STAT_ATTACK, 1.5f);
//     }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataEnemyAbiVersion() before using anything added
// after version 1.
//
// ---------------------------------------------------------------------------
// Per-type versus per-instance
// ---------------------------------------------------------------------------
//
// Two different things a mod might mean by "change an enemy's attack", and they
// are separate halves of this header.
//
// Per-type is the rebalance. An override is a rule keyed by enemy type ("every
// Fungus has 1.5x defense"), held by the host and reapplied to every matching
// enemy on every frame of every battle from now on. Overrides are the headline
// feature and the only half that outlives the battle it was set in.
//
// Per-instance is the escape hatch: EternalSonataSetEnemyStat writes one live
// enemy's record right now. It is meaningful only during a battle, it is
// forgotten when the encounter ends, and an override on the same stat will
// overwrite it on the next frame.
//
// Overrides are held in memory only; nothing here touches the save. A mod that
// wants its rebalance to survive a restart reapplies it on load.
//
// ---------------------------------------------------------------------------
// How an override is applied
// ---------------------------------------------------------------------------
//
// The host snapshots each enemy's untouched stats the first time it sees the
// record in a battle, and every subsequent application computes from that
// snapshot rather than from the current value. So a x1.5 multiplier is x1.5
// forever and not x1.5 per frame, and clearing an override restores the
// original number rather than leaving whatever was last written.
//
// Absolute and multiplier forms both exist because rebalancing is usually
// "x1.5" and not "= 240": the game scales an encounter's enemies with the
// story, so a multiplier keeps its meaning at every point in the game while an
// absolute value does not. Setting one form replaces the other.
//
// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
//
// Changes are published on the shared mod registry bus
// (rex::system::ModRegistry, reached via runtime->mod_registry()), so a mod
// subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_ENEMY_EVENT_OVERRIDE_SET     "eternalsonata.enemy.override.set"
//     ETERNALSONATA_ENEMY_EVENT_OVERRIDE_CLEARED "eternalsonata.enemy.override.cleared"
//
// In both the payload's `u64` packs the type in its high 32 bits and the
// ETERNALSONATA_ENEMY_STAT_* id in its low 32 bits, and `f64` is the
// multiplier or the absolute value that was set (0 when cleared). `bytes` is
// empty. They fire for a mod's changes only; the game has no way to set one.
//
// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------
//
// Every entry point here is safe to call from any thread, including the ImGui
// draw thread, and every one of them answers immediately: reads and writes are
// both plain guest-memory accesses, and setting an override only touches a
// host-side table. Nothing here runs guest code, so nothing here queues and
// nothing returns a "queued" result. The reapplication of overrides happens on
// the guest main thread once per frame, so a per-type change set from a UI
// takes effect on the next frame.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_ENEMY_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_ENEMY_EVENT_OVERRIDE_SET "eternalsonata.enemy.override.set"
#define ETERNALSONATA_ENEMY_EVENT_OVERRIDE_CLEARED "eternalsonata.enemy.override.cleared"

// An enemy type is identified by its name id, the same 1-based id
// EternalSonataGetBattleEnemyName resolves to a display name and the same one
// EternalSonataEnemyStats::name_id reports. Pass this instead to have an
// override apply to every enemy in the game; a type's own override wins over
// it. Name ids are 1-based, so 0 is free for this.
#define ETERNALSONATA_ENEMY_TYPE_ANY 0

// The game fields no more than this many enemies at once. The live count is
// always the real answer (EternalSonataGetBattleEnemyCount); this is a bound
// for callers that want a fixed array.
#define ETERNALSONATA_ENEMY_MAX_SLOTS 16

// An enemy record carries two "part" sub-records, because a boss can change
// form mid-battle, and only one is live at a time. Everything in this header
// works on the live part unless it says otherwise; per-type overrides are
// applied to both, so a boss is rebalanced in each of its forms.
#define ETERNALSONATA_ENEMY_PART_COUNT 2

// The stats an override can name. Every one of them is an int32 in this ABI,
// including the three the game stores as floats, which are exposed as a
// percentage (100 means "as shipped") so that one enum covers everything and
// new stats stay additive.
enum {
  // Level, as the battle chatter and the results screen read it. Changing it
  // does not rescale anything else: the other stats are stored outright, not
  // derived from the level.
  ETERNALSONATA_ENEMY_STAT_LEVEL = 0,
  // Current and maximum HP. Setting the maximum keeps the current at the same
  // fraction of it, which is what makes "x2 HP" mean a fight twice as long
  // rather than one that starts half over.
  ETERNALSONATA_ENEMY_STAT_HP = 1,
  ETERNALSONATA_ENEMY_STAT_HP_MAX = 2,
  // The three stats the damage formula reads, and the three the game's own
  // abilities can buff. The host rewrites the pristine copy the game clamps
  // buffs against, so a multiplier here is not undone by the next buff.
  ETERNALSONATA_ENEMY_STAT_ATTACK = 3,
  ETERNALSONATA_ENEMY_STAT_DEFENSE = 4,
  ETERNALSONATA_ENEMY_STAT_SPEED = 5,
  // Percent damage reduction, applied to the two halves the party's own attack
  // is built from. There is no enemy "magic attack": an enemy's spells take
  // their power from the ability rather than from a stat, so ATTACK is the
  // only offensive number a rebalance has to touch.
  ETERNALSONATA_ENEMY_STAT_PHYSICAL_RESIST = 6,
  ETERNALSONATA_ENEMY_STAT_MAGIC_RESIST = 7,
  // Percent chance that a hit taken by this enemy lands as a critical, which
  // multiplies the damage it takes by 1.5. It is a property of the target, not
  // of the attacker.
  ETERNALSONATA_ENEMY_STAT_CRIT_RATE = 8,
  // What killing this enemy is worth. EXP is summed over the encounter and
  // then scaled by any EXP-bonus equipment; gold is added straight to the
  // party's purse, which saturates at 99999999.
  ETERNALSONATA_ENEMY_STAT_EXP = 9,
  ETERNALSONATA_ENEMY_STAT_GOLD = 10,
  // The two drop slots: an item id (the 1..512 master entity id the item API
  // documents, 0 for "nothing") and a percent chance for each. A battle yields
  // at most three items in total however many enemies it holds, so raising
  // every rate to 100 does not hand out one drop per enemy.
  ETERNALSONATA_ENEMY_STAT_DROP_ITEM_1 = 11,
  ETERNALSONATA_ENEMY_STAT_DROP_RATE_1 = 12,
  ETERNALSONATA_ENEMY_STAT_DROP_ITEM_2 = 13,
  ETERNALSONATA_ENEMY_STAT_DROP_RATE_2 = 14,
  // Percentages of the shipped value, 100 meaning unchanged. MOVE_RANGE is how
  // far one move carries the enemy across the arena, which is the closest
  // thing its record has to a movement speed; CHASE_RANGE is how far away it
  // will still pick a target; SCALE is the model's size, which is cosmetic.
  // A multiplier on one of these and a multiplier on the underlying float mean
  // the same thing, so x1.5 works as expected either way.
  ETERNALSONATA_ENEMY_STAT_MOVE_RANGE_PCT = 15,
  ETERNALSONATA_ENEMY_STAT_CHASE_RANGE_PCT = 16,
  ETERNALSONATA_ENEMY_STAT_SCALE_PCT = 17,

  ETERNALSONATA_ENEMY_STAT_COUNT = 18
};

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_ENEMY_OK = 0,

  // No battle is in progress, or the host is not bound yet (title screen,
  // loading). Only the per-instance half reports this: a per-type override can
  // be set at any time and simply waits for a battle.
  ETERNALSONATA_ENEMY_ERR_UNAVAILABLE = -1,
  // The slot is past the live enemy count.
  ETERNALSONATA_ENEMY_ERR_INVALID_SLOT = -2,
  // The stat id is not one of the ETERNALSONATA_ENEMY_STAT_* values.
  ETERNALSONATA_ENEMY_ERR_INVALID_STAT = -3,
  // This stat cannot be written (nothing is read-only in version 1; the code
  // is here so a later stat can be).
  ETERNALSONATA_ENEMY_ERR_READ_ONLY = -4,
  // No override matches the type and stat asked about.
  ETERNALSONATA_ENEMY_ERR_NO_OVERRIDE = -5,
  ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT = -10
};

// How an override computes the value it writes.
enum {
  // value = original * multiplier, rounded to nearest. Survives the game's own
  // level scaling, and is what a rebalance normally wants.
  ETERNALSONATA_ENEMY_OVERRIDE_MULTIPLY = 0,
  // value = the number given, whatever the original was.
  ETERNALSONATA_ENEMY_OVERRIDE_ABSOLUTE = 1
};

// Everything one live enemy's record holds, in the units the stat ids above
// use. Filled from whichever part of the record is currently live.
typedef struct EternalSonataEnemyStats {
  // Where the enemy sits on its side of the battle, and which of the record's
  // two parts is live (0 or 1).
  int32_t slot;
  int32_t part;

  // The type's identity. `name_id` is what an override is keyed by and what
  // EternalSonataGetBattleEnemyName turns into a display name.
  int32_t name_id;

  int32_t level;
  int32_t hp;
  int32_t hp_max;
  int32_t attack;
  int32_t defense;
  int32_t speed;
  int32_t physical_resist;
  int32_t magic_resist;
  int32_t crit_rate;
  int32_t exp;
  int32_t gold;
  int32_t drop_item_1;
  int32_t drop_rate_1;
  int32_t drop_item_2;
  int32_t drop_rate_2;
  // Percentages of the shipped value; see the stat ids above.
  int32_t move_range_pct;
  int32_t chase_range_pct;
  int32_t scale_pct;

  // The record's flag bits, as EternalSonataBattleUnit::flags reports them.
  int32_t flags;

  int32_t reserved[6];  // zero-filled; room for later additions
} EternalSonataEnemyStats;

// One per-type override, as EternalSonataGetEnemyTypeOverride reports it.
typedef struct EternalSonataEnemyOverride {
  int32_t type;  // name id, or ETERNALSONATA_ENEMY_TYPE_ANY
  int32_t stat;  // ETERNALSONATA_ENEMY_STAT_*
  int32_t mode;  // ETERNALSONATA_ENEMY_OVERRIDE_*
  int32_t value;      // the absolute value, when mode is ABSOLUTE
  float multiplier;   // the factor, when mode is MULTIPLY
  int32_t reserved[3];  // zero-filled
} EternalSonataEnemyOverride;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataEnemyAbiVersionFn)(void);

// True while a battle is in progress and its enemy records are readable, i.e.
// while the per-instance half of this header will do anything. The per-type
// half does not need it.
typedef int (*EternalSonataIsEnemySystemAvailableFn)(void);

// A short name for a stat id, e.g. "attack", "exp", "drop rate 1". Never null;
// "" for anything outside the enum, so a caller can fall back to the number.
// Prefer this to a table of your own, which would go stale as stats are added.
typedef const char* (*EternalSonataGetEnemyStatNameFn)(int stat);

// ---------------------------------------------------------------------------
// Reading and writing one live enemy
// ---------------------------------------------------------------------------

// Fills `out` with the enemy in `slot`, 0-based within the enemy side and
// below the live enemy count. Returns ETERNALSONATA_ENEMY_OK or a negative
// error, with `out` zeroed on failure.
typedef int (*EternalSonataGetEnemyStatsFn)(int slot, EternalSonataEnemyStats* out);

// One stat of one live enemy, or a negative error. Note that every stat is
// non-negative in practice, so a negative return is always an error.
typedef int (*EternalSonataGetEnemyStatFn)(int slot, int stat);

// Writes one stat of one live enemy, clamped to what the game's own field
// holds. This is the escape hatch, not the rebalance: it lasts as long as the
// encounter does, and a per-type override on the same stat overwrites it on
// the next frame. Setting HP_MAX rescales the current HP to hold the ratio,
// and either HP write also refreshes the cached ratio the game's own liveness
// checks read (see EternalSonataSetBattleEnemyHp for why that matters).
//
// Returns ETERNALSONATA_ENEMY_OK or a negative error. Takes effect
// immediately.
typedef int (*EternalSonataSetEnemyStatFn)(int slot, int stat, int32_t value);

// The enemy type of the enemy in `slot`: its name id, for keying an override,
// and the internal type name the game loads its AI script by ("fungus" and
// such). The name is never null and stays valid for the life of the process;
// prefer EternalSonataGetBattleEnemyName for anything shown to a player, since
// this one is not translated.
typedef int (*EternalSonataGetEnemyTypeFn)(int slot);
typedef const char* (*EternalSonataGetEnemyTypeNameFn)(int slot);

// ---------------------------------------------------------------------------
// Per-type overrides: the rebalance
// ---------------------------------------------------------------------------

// Makes every enemy of `type` have `multiplier` times its shipped `stat`, in
// this battle and every one after it. `type` is a name id, or
// ETERNALSONATA_ENEMY_TYPE_ANY for all of them; a type's own override wins
// over an ANY one. The multiplier is applied to the value the enemy shipped
// with, not to whatever it currently holds, so calling this twice with 1.5 is
// still 1.5x and not 2.25x.
//
// A multiplier of 0 is allowed and means the stat is zeroed; to remove an
// override use EternalSonataClearEnemyTypeStat rather than passing 1.0, which
// would keep reasserting the original value over any per-instance write.
//
// Returns ETERNALSONATA_ENEMY_OK or a negative error. No battle need be in
// progress.
typedef int (*EternalSonataSetEnemyTypeStatMultiplierFn)(int type, int stat, float multiplier);

// The same, with an absolute value: every enemy of `type` gets exactly
// `value`, whatever it shipped with. Replaces any multiplier on the same stat.
typedef int (*EternalSonataSetEnemyTypeStatValueFn)(int type, int stat, int32_t value);

// Removes one override, restoring the shipped value on the next frame.
// Returns ETERNALSONATA_ENEMY_OK, or ETERNALSONATA_ENEMY_ERR_NO_OVERRIDE if
// nothing matched.
typedef int (*EternalSonataClearEnemyTypeStatFn)(int type, int stat);

// Removes every override on `type`, or every override there is. Both return
// how many were removed, or a negative error.
typedef int (*EternalSonataClearEnemyTypeOverridesFn)(int type);
typedef int (*EternalSonataClearAllEnemyOverridesFn)(void);

// Fills `out` with the override on `type`'s `stat`. Answers only the exact
// pair asked for, so an ANY override is not reported for a specific type; ask
// for ETERNALSONATA_ENEMY_TYPE_ANY to see that one. Returns
// ETERNALSONATA_ENEMY_OK, or ETERNALSONATA_ENEMY_ERR_NO_OVERRIDE.
typedef int (*EternalSonataGetEnemyTypeOverrideFn)(int type, int stat,
                                                   EternalSonataEnemyOverride* out);

// How many overrides are set in total, and a copy of up to `max` of them.
// EternalSonataGetEnemyOverrides returns how many were written, or a negative
// error; pass max = 0 to just count. The order is unspecified and not stable
// across changes.
typedef int (*EternalSonataGetEnemyOverrideCountFn)(void);
typedef int (*EternalSonataGetEnemyOverridesFn)(EternalSonataEnemyOverride* out, int max);

#ifdef __cplusplus
}  // extern "C"
#endif
