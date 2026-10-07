// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for reading and writing the game's party state: who is in the
// party, in what order, with what stats, plus adding and removing members.
//
// This exists so a mod never has to know a single guest address. Everything
// the party_overlay mod used to derive by hand (position tables, stat strides,
// the join sequence, the battle-party resync, the battle-safety gate) lives in
// the host now and is documented in docs/party-system.md.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options API is used (see eternalsonata_options_api.h):
//
//     auto add = reinterpret_cast<EternalSonataAddCharacterToPartyFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataAddCharacterToParty"));
//     if (add) { add(kAllegretto); }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataPartyAbiVersion() before using anything added
// after version 1.
//
// Threading. Every entry point here is safe to call from any thread, including
// the ImGui draw thread. Reads answer from guest memory immediately. Writes
// that need to run guest code (adding, removing, reordering, refreshing stats)
// are queued onto the guest main thread and applied on its next frame, because
// guest calls need a live guest ThreadState that the draw thread does not have
// - calling them from a draw hook crashes the game. Those functions therefore
// return ETERNALSONATA_PARTY_QUEUED rather than a final result, unless they
// are called from work already running on the guest main thread, in which case
// they run inline and return the real outcome. Poll
// EternalSonataIsCharacterInParty / EternalSonataGetCharacterPosition to
// observe the result of a queued change; the checks that can be made without
// running guest code (unknown character, no save, battle in progress, already
// in the party) are still reported immediately, before anything is queued.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_PARTY_ABI_VERSION 3u

// The game's cast. Character ids are 1-based and are the game's own numbering,
// which is also the order of its internal name table.
enum {
  ETERNALSONATA_CHAR_ALLEGRETTO = 1,
  ETERNALSONATA_CHAR_POLKA = 2,
  ETERNALSONATA_CHAR_BEAT = 3,
  ETERNALSONATA_CHAR_FREDERIC = 4,
  ETERNALSONATA_CHAR_VIOLA = 5,
  ETERNALSONATA_CHAR_SALSA = 6,
  ETERNALSONATA_CHAR_JAZZ = 7,
  ETERNALSONATA_CHAR_FALSETTO = 8,
  ETERNALSONATA_CHAR_CLAVES = 9,
  ETERNALSONATA_CHAR_MARCH = 10,
  // The PS3's two; on 360 data, free slots for modded characters.
  ETERNALSONATA_CHAR_CRESCENDO = 11,
  ETERNALSONATA_CHAR_SERENADE = 12,
  ETERNALSONATA_CHARACTER_COUNT = 12
};

// The ten every release has; ids above it are the added slots.
#define ETERNALSONATA_NATIVE_CHARACTER_COUNT 10

// The party's first three display positions are the ones that walk the field
// and fight; everything past that is a reserve.
#define ETERNALSONATA_ACTIVE_PARTY_SIZE 3

// The highest level the game's own curve goes to, and the EXP ceiling it
// clamps a character's total to.
#define ETERNALSONATA_LEVEL_MAX 99
#define ETERNALSONATA_EXP_MAX 99999999

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_PARTY_OK = 0,
  // The change was accepted and will be applied on the guest thread's next
  // frame. See the threading note at the top.
  ETERNALSONATA_PARTY_QUEUED = 1,

  // No save is loaded, or party state is not readable yet (e.g. at the title
  // screen). Every mutation refuses in this state.
  ETERNALSONATA_PARTY_ERR_UNAVAILABLE = -1,
  ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER = -2,
  // A battle is in progress. The game's own join sequence walks into
  // battle-model math that expects a character set up by the battle loader, so
  // party edits are refused for the duration (see docs/party-system.md).
  ETERNALSONATA_PARTY_ERR_IN_BATTLE = -3,
  // -4..-6 are no longer returned: the join they came from was the Item Set's.
  ETERNALSONATA_PARTY_ERR_ROSTER_FULL = -4,
  ETERNALSONATA_PARTY_ERR_PARTY_LEVEL = -5,
  ETERNALSONATA_PARTY_ERR_NOT_ELIGIBLE = -6,
  ETERNALSONATA_PARTY_ERR_ALREADY_IN_PARTY = -7,
  ETERNALSONATA_PARTY_ERR_NOT_IN_PARTY = -8,
  ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT = -10,
  // ABI 3: another character holds that slot, or none is free.
  ETERNALSONATA_PARTY_ERR_SLOT_TAKEN = -11,
  ETERNALSONATA_PARTY_ERR_NO_SLOTS = -12
};

// One character's stats, in the units the game's own status and equipment
// screens display.
typedef struct EternalSonataCharacterStats {
  int32_t level;
  int32_t hp;      // current HP
  int32_t hp_max;  // maximum HP
  int32_t attack;
  int32_t magic;
  int32_t defense;
  int32_t speed;
  // Added in ABI version 2, out of the reserved space, so the struct's size
  // and the fields above it did not move.
  //
  // The game treats EXP as the source of truth during battle awards. For ABI
  // compatibility EternalSonataSetCharacterStats leaves both EXP fields alone;
  // use the dedicated EXP functions to change them.
  int32_t exp;  // total EXP earned, 0..ETERNALSONATA_EXP_MAX
  // How much more EXP this character needs to reach its next level, which is
  // what the status screen prints as "next". Derived, so it is filled in on
  // read and ignored on write. 0 at ETERNALSONATA_LEVEL_MAX.
  int32_t exp_to_next;
  int32_t reserved[7];  // zero-filled; room for later additions
} EternalSonataCharacterStats;

// ---------------------------------------------------------------------------
// Capability and state
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataPartyAbiVersionFn)(void);

// True once a save is loaded and party state can be read. False at the title
// screen and during loading.
typedef int (*EternalSonataIsPartyAvailableFn)(void);

// True when the party can be modified right now: available, and no battle in
// progress. Check this before offering add/remove UI; the mutating calls check
// it too and return ETERNALSONATA_PARTY_ERR_IN_BATTLE otherwise.
typedef int (*EternalSonataIsPartyEditableFn)(void);

// ---------------------------------------------------------------------------
// Reading the party
// ---------------------------------------------------------------------------

// Display name of `character` - the custom name if one was set, otherwise the
// game's own English name. Never null for a valid character; returns "" for an
// unknown one. The pointer stays valid until the name is changed again.
typedef const char* (*EternalSonataGetCharacterNameFn)(int character);

// Number of characters currently in the party (active members plus reserves).
typedef int (*EternalSonataGetPartySizeFn)(void);

// Fills `out` with the party's characters in display order (position 1 first)
// and returns how many were written, or a negative error. Pass max = 0 to just
// count. The first ETERNALSONATA_ACTIVE_PARTY_SIZE entries are the active
// party.
typedef int (*EternalSonataGetPartyMembersFn)(int* out, int max);

// True if `character` is in the party at all (active or reserve).
typedef int (*EternalSonataIsCharacterInPartyFn)(int character);

// True if `character` is one of the three active members.
typedef int (*EternalSonataIsCharacterActiveFn)(int character);

// `character`'s 1-based display position, 0 if it is not in the party, or a
// negative error.
typedef int (*EternalSonataGetCharacterPositionFn)(int character);

// Party level (1..6), or a negative error.
typedef int (*EternalSonataGetPartyLevelFn)(void);

// The party level's total member budget, how much of it recruited members use,
// and what is left. Each character costs a fixed amount; a character can only
// join while the remainder covers its cost.
typedef int (*EternalSonataGetPartyLevelBudgetFn)(void);
typedef int (*EternalSonataGetPartyLevelBudgetUsedFn)(void);
typedef int (*EternalSonataGetPartyLevelBudgetFreeFn)(void);

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

// Reads the stats the status screen shows: the character's own stats with its
// equipment bonuses already folded in. Returns ETERNALSONATA_PARTY_OK or a
// negative error.
typedef int (*EternalSonataGetCharacterStatsFn)(int character,
                                                EternalSonataCharacterStats* out);

// Reads the character's own stats, without equipment bonuses. This is the set
// EternalSonataSetCharacterStats writes, so a get/modify/set round trip that
// uses this function keeps the numbers stable; one that uses
// EternalSonataGetCharacterStats instead adds the equipment bonus in each time.
typedef int (*EternalSonataGetCharacterBaseStatsFn)(int character,
                                                    EternalSonataCharacterStats* out);

// Writes the character's own stats and refreshes what the screens display.
// Values are clamped to what the game's own fields hold (HP is a signed 32-bit
// count, the four stats are capped at 999 exactly as the game caps them).
typedef int (*EternalSonataSetCharacterStatsFn)(int character,
                                                const EternalSonataCharacterStats* stats);

// Sets current HP to maximum for one character, or for the whole party.
typedef int (*EternalSonataHealCharacterFn)(int character);
typedef int (*EternalSonataHealPartyFn)(void);

// ---------------------------------------------------------------------------
// EXP (ABI version 2)
// ---------------------------------------------------------------------------
//
// One curve covers the whole cast: the EXP a level costs starts at 200 and
// grows by a second difference that itself grows by 9, and a character's level
// is however many of those steps its total EXP has paid for. The four
// functions below are the same numbers EternalSonataGetCharacterStats reports
// in `exp` / `exp_to_next`, reachable without reading a whole stat block.

// `character`'s total EXP, or a negative error.
typedef int (*EternalSonataGetCharacterExpFn)(int character);

// How much more EXP `character` needs to level, 0 at ETERNALSONATA_LEVEL_MAX,
// or a negative error.
typedef int (*EternalSonataGetCharacterExpToNextLevelFn)(int character);

// Sets `character`'s total EXP, clamped to 0..ETERNALSONATA_EXP_MAX, and
// updates its level to the one that total buys. Stats do not grow on their
// own: the game's per-level gains are applied by its own level-up routine as
// EXP is awarded in battle, so a mod that jumps a character forward this way
// should follow with EternalSonataSetCharacterStats.
typedef int (*EternalSonataSetCharacterExpFn)(int character, int exp);

// Adds `exp` (negative to take it away) to `character`'s total, otherwise the
// same as EternalSonataSetCharacterExp. Returns the new total, or a negative
// error.
typedef int (*EternalSonataAddCharacterExpFn)(int character, int exp);

// Curve lookups, independent of any character and of whether a save is loaded.
//
//   ...TotalExpForLevel(level)  total EXP a character needs to be `level`;
//                               0 for level 1, negative for out of range
//   ...LevelForExp(exp)         the level that total buys, 1..99
typedef int (*EternalSonataGetTotalExpForLevelFn)(int level);
typedef int (*EternalSonataGetLevelForExpFn)(int exp);

// ---------------------------------------------------------------------------
// Changing the party
// ---------------------------------------------------------------------------

// Adds `character` to the party, running the game's own join sequence: make it
// owned, add it to the roster against the party-level budget, give it the next
// free display position, and rebuild the battle party.
typedef int (*EternalSonataAddCharacterToPartyFn)(int character);

// Removes `character` from the party, closing the gap in the display order
// exactly as the game's own party menu does. The character stays recruited, so
// it can be added back.
typedef int (*EternalSonataRemoveCharacterFromPartyFn)(int character);

// Moves `character` to 1-based display `position`. Whoever held that position
// takes the mover's old one, so the party is never left with a gap. Position 1
// to ETERNALSONATA_ACTIVE_PARTY_SIZE is the active party, so this is also how
// you bench a member or promote a reserve.
typedef int (*EternalSonataSetCharacterPositionFn)(int character, int position);

// Exchanges two characters' display positions. Both must be in the party.
typedef int (*EternalSonataSwapCharacterPositionsFn)(int a, int b);

// Sets the party level (1..6) and recomputes the member budget from the game's
// own per-level table. Existing members are left alone even if the new level
// no longer covers them.
typedef int (*EternalSonataSetPartyLevelFn)(int level);

// ---------------------------------------------------------------------------
// Names and custom characters
// ---------------------------------------------------------------------------

// Renames a character everywhere the game draws its name: the status,
// equipment and party screens all resolve names through one text lookup, which
// the host answers with this string. Pass null or "" to restore the game's own
// name. Text is single-byte (CP1252/Latin-1), not UTF-8, because the game's
// font draws one glyph per byte - write "\xE9" rather than "é".
//
// The menu screens show the name in full. Screens that read the game's packed
// name tables directly, the battle HUD among them, are limited to the length of
// the name being replaced (Allegretto has room for ten characters, Beat for
// four) and show a truncated name if the new one is longer.
typedef int (*EternalSonataSetCharacterNameFn)(int character, const char* name);

// ---------------------------------------------------------------------------
// Modded characters (ABI version 3)
// ---------------------------------------------------------------------------
//
// On Xbox 360 data, ids 11 and 12 are vacant slots: the game's tables are
// twelve wide, but nobody is there, so they cannot join a party or be drawn.
// A mod defines a slot to put its own character in it. On PS3 data Crescendo
// and Serenade fill them and there are no vacant slots.
//
// A character brings its own model, battle file (motions, camera scripts,
// effects, specials), voice bank and portraits, and takes everything it does
// not bring from a `base` character of the retail cast: its stat template and
// growth, and its cases in the game's own per character switches (cloth, hit
// motions, battle HUD portrait). Costumes work as for anyone else
// (eternalsonata_costume_api.h); the definition's model and portraits are its
// default costume. docs/modded-characters.md has the file formats.
//
// The same can be declared without code, as [[character]] in a mod's
// assets.toml. Definitions are not saved: define on every run, from
// OnModuleLaunched at the latest, before a save loads.

typedef struct EternalSonataCharacterDefinition {
  // sizeof(EternalSonataCharacterDefinition).
  uint32_t struct_size;

  // Unique and stable, e.g. "my_mod/cadenza": saves name the character by it.
  const char* id;
  // Display name, single-byte CP1252 like EternalSonataSetCharacterName.
  const char* name;
  // Lowercase key for costume declarations and cvars; null derives it from
  // the name.
  const char* token;

  // 1..ETERNALSONATA_NATIVE_CHARACTER_COUNT; 0 means Allegretto.
  int32_t base;

  // A file path, Xbox 360 NOBJ: the body in the field, in battle and in
  // events. Null wears the base's model. Bone names must match the motions
  // the character plays, its own battle file's or the base's.
  const char* model_path;
  // The battle scene name given to the model, e.g. "bCDZ"; null takes the
  // base's.
  const char* scene_name;

  // The NNN of btldata/player/pcNNN.bop and btldata/voice/pcNNN.csf. 0
  // loads the slot's own (pc011.bop for character 11) when the game data or
  // a mod ships it, else the base's.
  int32_t battle_file;
  int32_t voice_file;

  // Portraits by ETERNALSONATA_COSTUME_PORTRAIT_* kind (status, panel, face,
  // small face): .dds or NTEX file paths, null for the base's.
  const char* portrait_paths[4];

  // Starting own stats, applied when the character joins a party while it
  // has no EXP. 0 lets the base's template decide.
  int32_t apply_stats;
  EternalSonataCharacterStats stats;

  uint32_t reserved[8];  // zero-fill
} EternalSonataCharacterDefinition;

// How many added slots this host has (defined or not), and the id of slot
// `index` in 0..count-1, or a negative error.
typedef int (*EternalSonataGetAddedSlotCountFn)(void);
typedef int (*EternalSonataGetAddedSlotFn)(int index);

// 1 if `character` exists on this data: the retail cast, the PS3's two, or
// a defined slot.
typedef int (*EternalSonataIsCharacterDefinedFn)(int character);

// Defines slot `character`. Defining it again with the same id replaces the
// definition; another id gets ETERNALSONATA_PARTY_ERR_SLOT_TAKEN. Returns the
// character id.
typedef int (*EternalSonataDefineCharacterFn)(int character,
                                              const EternalSonataCharacterDefinition* definition);

// Defines the first free slot (or this id's own), so mods that do not care
// which slot they get can coexist. Returns the character id, or
// ETERNALSONATA_PARTY_ERR_NO_SLOTS.
typedef int (*EternalSonataDefineNextCharacterFn)(
    const EternalSonataCharacterDefinition* definition);

// Gives a slot back. Refused with ETERNALSONATA_PARTY_ERR_ALREADY_IN_PARTY
// while the character is in the party.
typedef int (*EternalSonataUndefineCharacterFn)(int character);

#ifdef __cplusplus
}  // extern "C"
#endif
