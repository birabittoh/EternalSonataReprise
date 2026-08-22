// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for reading the state of an in-progress battle, and for the one
// battle action the host knows how to perform safely: forcing a win.
//
// This exists so a mod never has to know a single guest address. The battle
// manager's layout, the per-unit FSM and the asymmetry in the game's own
// battle-over predicate all live in the host now (src/battle_layout.h and
// src/battle_system.cpp); a mod asks questions instead of chasing pointers.
//
// This is a separate ABI from eternalsonata_party_api.h on purpose. The party
// API describes who is recruited and what their stats are, which is save state
// and outlives any battle; this one describes a battle that exists for a few
// seconds and is gone. They version independently.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the party API is used (see eternalsonata_party_api.h):
//
//     auto win = reinterpret_cast<EternalSonataWinBattleFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataWinBattle"));
//     if (win) { win(); }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataBattleAbiVersion() before using anything added
// after version 1.
//
// Threading. Every entry point here is safe to call from any thread, including
// the ImGui draw thread. The readers answer from guest memory immediately, so
// a UI can call them every frame. EternalSonataWinBattle queues its work onto
// the guest main thread and returns ETERNALSONATA_BATTLE_QUEUED; see its own
// comment for why it may then sit queued for several frames.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_BATTLE_ABI_VERSION 2u

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_BATTLE_OK = 0,
  // The action was accepted and will be applied on a later guest frame. See
  // EternalSonataWinBattleFn.
  ETERNALSONATA_BATTLE_QUEUED = 1,

  // No battle is in progress, or the host is not bound yet (title screen,
  // loading). Every reader below reports this rather than inventing zeroes.
  ETERNALSONATA_BATTLE_ERR_UNAVAILABLE = -1,
  // The slot is past the live unit count for its side.
  ETERNALSONATA_BATTLE_ERR_INVALID_SLOT = -2,
  // The battle is not in its intro, so there is nothing to skip. Check
  // fsm_state against ETERNALSONATA_BATTLE_FSM_INTRO before offering the UI.
  ETERNALSONATA_BATTLE_ERR_NOT_IN_INTRO = -3,
  ETERNALSONATA_BATTLE_ERR_INVALID_ARGUMENT = -10
};

// Which side holds the turn. The game substitutes NONE when nothing is acting.
enum {
  ETERNALSONATA_BATTLE_ACTOR_PARTY = 0,
  ETERNALSONATA_BATTLE_ACTOR_ENEMY = 1,
  ETERNALSONATA_BATTLE_ACTOR_NONE = 2
};

// The game fields at most three party members in a battle. There is no
// equivalent hard cap on the enemy side; this is a sanity bound for callers
// that want a fixed array, and the live count is always the real answer.
#define ETERNALSONATA_BATTLE_MAX_PARTY 3
#define ETERNALSONATA_BATTLE_MAX_ENEMIES 16

// The per-unit FSM value the game returns for "no such unit".
#define ETERNALSONATA_BATTLE_UNIT_STATE_NONE 101

// The battle state machine's states run 1..23. The ones worth naming:
//
// INTRO is the opening flourish, the camera circling while both sides say
// their lines. TURN is the battle actually being played, and covers the whole
// of every turn, the player's and the enemies' alike, so it is where a battle
// spends nearly all of its time; TURN_END follows it for the hand-off between
// turns. ENDING is the start of the wrap-up: end of battle, the level-up
// screens, a conditional party-level-up screen, the write-back of battle HP
// into the save, then teardown at TEARDOWN.
//
// Compare fsm_state >= ENDING rather than against any single wrap-up state:
// two of them are skipped unless the encounter earns a party level, and the
// write-back lasts a frame or two. Nothing can be forced from there.
#define ETERNALSONATA_BATTLE_FSM_INTRO 5
#define ETERNALSONATA_BATTLE_FSM_TURN 12
#define ETERNALSONATA_BATTLE_FSM_TURN_END 13
// Where the three endings are told apart: the game reaches OUTCOME once and
// leaves it, in the same frame, for exactly one of DEFEAT, ESCAPED or ENDING.
#define ETERNALSONATA_BATTLE_FSM_OUTCOME 14
// The battle was lost, or the party ran. Neither is part of the wrap-up chain
// below, so neither is covered by fsm_state >= ENDING; test for them
// separately. DEFEAT is terminal in the battle FSM itself: the game-over
// hand-off happens elsewhere and the state never advances again. ESCAPED waits
// out a short timer, or a button press, and then jumps straight to the HP
// write-back, skipping the results and rewards entirely.
#define ETERNALSONATA_BATTLE_FSM_DEFEAT 15
#define ETERNALSONATA_BATTLE_FSM_ESCAPED 16
#define ETERNALSONATA_BATTLE_FSM_ENDING 18
#define ETERNALSONATA_BATTLE_FSM_TEARDOWN 23

// A snapshot of the whole battle, as of the moment of the call.
typedef struct EternalSonataBattleState {
  int32_t active;  // 1 while a battle is in progress
  // Battle state machine state, 1..23. The one worth recognising is
  // ETERNALSONATA_BATTLE_FSM_TURN: it covers the whole of every turn, both
  // sides', and is where a battle spends nearly all of its time.
  int32_t fsm_state;
  int32_t party_count;  // live party units
  int32_t enemy_count;  // live enemy units
  int32_t actor_kind;   // ETERNALSONATA_BATTLE_ACTOR_*
  int32_t actor_slot;   // the acting unit's slot on its own side
  // 1 when a forced win would take effect on this frame. The game only checks
  // whether the enemies are dead while a party member holds the turn and no
  // action is mid-resolution, so this is 0 for most of an enemy's turn. A
  // queued EternalSonataWinBattle waits for this by itself; the flag is here
  // so a UI can say why nothing has happened yet.
  int32_t can_win_now;
  int32_t reserved[9];  // zero-filled; room for later additions
} EternalSonataBattleState;

// One unit on either side. Fields the host cannot resolve for that side are
// left at the "unknown" values noted below rather than being faked.
typedef struct EternalSonataBattleUnit {
  int32_t slot;       // index within its own side
  int32_t character;  // party: character id 1..10 (see the party API). enemy: 0
  // Level and HP, for both sides. On the enemy side these live in whichever
  // of the record's two "part" sub-records is currently live (bosses that
  // change form are why there are two), so all three are left at -1 in the
  // rare case that index reads out of range.
  int32_t level;
  int32_t hp;
  int32_t hp_max;
  // Current/max HP as a 0.0 to 1.0 fraction, for both sides, or -1.0f if it
  // could not be determined. Use this for a health bar rather than dividing
  // hp by hp_max yourself: on the enemy side it is a value the game caches
  // and keeps in step with the pair (max * ratio == hp), and most of its own
  // liveness checks, including the battle-over predicate, read it rather than
  // the raw counter. On the party side the game has no cached copy and
  // computes the same quotient on demand, which is what is done here.
  float hp_ratio;
  int32_t alive;       // 1 while the game still counts this unit as standing
  int32_t acting;      // 1 if this unit currently holds the turn
  int32_t unit_state;  // per-unit FSM state, ETERNALSONATA_BATTLE_UNIT_STATE_NONE if absent
  int32_t resolving;   // 1 while this unit's action is mid-resolution
  int32_t flags;       // enemy: the record's flag bits (bit 9 = targetable). party: 0
  // Enemy: the id its display name is looked up by; 0 if unavailable. Pass
  // the slot to EternalSonataGetBattleEnemyName rather than using this
  // directly. Party: 0, since `character` already identifies those.
  int32_t name_id;
  int32_t reserved[4];  // zero-filled; room for later additions
} EternalSonataBattleUnit;

// ---------------------------------------------------------------------------
// Capability and state
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataBattleAbiVersionFn)(void);

// True while a battle is in progress. This predates the rest of this header
// and is the same symbol mods were already using; battle state is not reliably
// derivable from guest memory alone, because the field stays loaded underneath
// a battle and the game's scene-mode register only exposes a battle's end.
typedef int (*EternalSonataIsBattleActiveFn)(void);

// Fills `out` with a snapshot of the current battle. Returns
// ETERNALSONATA_BATTLE_OK, or ETERNALSONATA_BATTLE_ERR_UNAVAILABLE with `out`
// zeroed when no battle is in progress.
typedef int (*EternalSonataGetBattleStateFn)(EternalSonataBattleState* out);

// A short human-readable name for a battle state machine state, e.g. "intro",
// "playing", "end of turn". Never null; "" for anything outside the machine's
// 1..23 range, so a caller can fall back to printing the number. All 23 states
// are named, but do not rely on any particular wording: the names describe
// reverse-engineered behaviour and get sharper as the understanding does.
//
// Prefer this to switching on fsm_state yourself. A mod carrying its own copy
// of the table goes stale, which is not hypothetical: states 5, 12, 13 and 15
// were identified after this API was first written, and the remaining eleven
// after that.
typedef const char* (*EternalSonataGetBattleStateNameFn)(int state);

// Live unit counts, or a negative error. Same figures as the snapshot's
// party_count / enemy_count, for callers that only want the one number.
typedef int (*EternalSonataGetBattlePartyCountFn)(void);
typedef int (*EternalSonataGetBattleEnemyCountFn)(void);

// Fills `out` with one unit's state. `slot` is 0-based within its own side and
// must be below that side's live count. Returns ETERNALSONATA_BATTLE_OK or a
// negative error, with `out` zeroed on failure.
typedef int (*EternalSonataGetBattlePartyUnitFn)(int slot, EternalSonataBattleUnit* out);
typedef int (*EternalSonataGetBattleEnemyFn)(int slot, EternalSonataBattleUnit* out);

// Display name of the enemy in `slot`, from the game's own text. Never null.
//
// Returns "" the first time a given enemy type is asked for, and its name from
// the next frame onward: resolving it means calling into the guest, which
// cannot happen on the thread a UI draws from, so the lookup is queued and
// cached. Calling this every frame is the intended use and is cheap after the
// first hit. The text is single-byte (CP1252/Latin-1), not UTF-8, the same as
// the party API's names.
//
// Party members have no equivalent here on purpose: use the party API's
// EternalSonataGetCharacterName with the unit's `character`, which also
// honours any renames a mod has applied.
typedef const char* (*EternalSonataGetBattleEnemyNameFn)(int slot);

// ---------------------------------------------------------------------------
// Acting on the battle
// ---------------------------------------------------------------------------

// The snapshot's can_win_now, for callers that want nothing else. 1, 0, or a
// negative error.
typedef int (*EternalSonataCanWinBattleNowFn)(void);

// Ends the current battle in the player's favour: takes every live enemy's
// health to zero and lets the game's own win detection run its normal victory,
// rewards and field transition.
//
// Returns ETERNALSONATA_BATTLE_QUEUED when accepted, or
// ETERNALSONATA_BATTLE_ERR_UNAVAILABLE if no battle is in progress. Queued
// means queued: the write is held back until a frame where a party member
// holds the turn and no action is resolving, because the game's own
// battle-over predicate only looks at the enemies' health on such a frame, and
// killing them while an enemy is acting wedges the battle instead of ending it
// (see src/battle_system.cpp). Pressing this during an enemy's turn is
// therefore safe; it simply takes effect once the turn comes back round. The
// request is dropped if that never happens within about fifteen seconds, or if
// the battle ends on its own first. Poll EternalSonataIsBattleActive to
// observe the result.
typedef int (*EternalSonataWinBattleFn)(void);

// Cuts the battle's opening flourish short and drops straight into play, by
// running the same routine the game's own skip button reaches. Valid only
// while fsm_state is ETERNALSONATA_BATTLE_FSM_INTRO; returns
// ETERNALSONATA_BATTLE_ERR_NOT_IN_INTRO otherwise.
//
// Returns ETERNALSONATA_BATTLE_QUEUED when accepted, but unlike a forced win
// this takes effect on the very next guest frame whatever point the intro has
// reached: it cuts the enemies' lines off mid-sentence just as readily as the
// party's. It also works on the story battles whose intros the game itself
// refuses to skip. An in-flight voice line still plays out over the start of
// the battle, exactly as it does when the game's own skip button is used.
typedef int (*EternalSonataSkipBattleIntroFn)(void);

// Sets a live party unit's current HP, in place, inside the battle. This
// writes the battle side's own raw current HP dword directly (see
// EternalSonataBattleUnit::hp), not the overworld party's stats: the two are
// separate copies for the length of a battle, and the overworld one is only
// overwritten from the battle's copy during the FSM's write-back step as the
// battle ends (see ETERNALSONATA_BATTLE_FSM_ENDING). Setting the overworld
// copy mid-battle therefore has no visible effect until the battle is already
// over, which is why this exists as its own entry point rather than reusing
// the party API's EternalSonataSetCharacterStats.
//
// `hp` is clamped to [0, hp_max]; hp_max itself is not settable here (the
// party API's stat editor is the place for that, and it is not a per-battle
// quantity the way current HP is). `slot` is 0-based within the party side
// and must be below the live party count, as with EternalSonataGetBattlePartyUnit.
//
// Returns ETERNALSONATA_BATTLE_OK, ETERNALSONATA_BATTLE_ERR_UNAVAILABLE if no
// battle is in progress, or ETERNALSONATA_BATTLE_ERR_INVALID_SLOT. This is a
// plain memory write with no turn-order hazard (nothing reads party HP the
// way the battle-over predicate reads enemy HP), so it takes effect
// immediately and is not queued.
typedef int (*EternalSonataSetBattlePartyHpFn)(int slot, int32_t hp);

// Sets a live enemy unit's current HP, in place. Writes both the raw current
// HP dword (EternalSonataBattleUnit::hp) and the cached current/max ratio the
// game keeps alongside it: the ratio, not the raw counter, is what every
// "is this enemy still up" check reads, including the battle-over predicate
// (see src/battle_system.cpp), so the two are kept in the same invariant the
// game itself maintains (hp_max * ratio == hp) rather than left to disagree.
//
// `hp` is clamped to [0, hp_max]. `slot` is 0-based within the enemy side and
// must be below the live enemy count.
//
// Returns ETERNALSONATA_BATTLE_OK, ETERNALSONATA_BATTLE_ERR_UNAVAILABLE, or
// ETERNALSONATA_BATTLE_ERR_INVALID_SLOT. This is a plain memory write and
// takes effect immediately, EXCEPT for driving an enemy to 0 HP during that
// same enemy's own turn: the battle-over predicate only scans enemy health on
// a frame where a party member holds the turn and no action is mid-resolution
// (see EternalSonataWinBattle, which queues for exactly that reason). Zeroing
// an enemy outside that window will not wedge the battle by itself as long as
// at least one enemy is left alive or the acting enemy's own turn is allowed
// to finish, but zeroing every live enemy this way reproduces the same hang
// EternalSonataWinBattle was written to avoid. Prefer EternalSonataWinBattle
// over calling this in a loop to end a battle.
typedef int (*EternalSonataSetBattleEnemyHpFn)(int slot, int32_t hp);

#ifdef __cplusplus
}  // extern "C"
#endif
