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
//
// Still 1: nothing has been published against this ABI, so the mods in
// ../EternalSonataReprise-Mods are simply rebuilt against the current header
// rather than being asked to cope with two versions of it. Bump this the first
// time a release goes out.
#define ETERNALSONATA_PARTY_ABI_VERSION 1u

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

  // The retail game ships ten characters. Ids 11 and 12 are real slots in every
  // per-character table -- the port widens those tables and teaches the game's
  // loops and range checks to count that far -- but the host puts no content in
  // them at all. They start **vacant**: no name, no stat template, and every
  // gate that would let one into the party stays shut, so a build with no mod
  // loaded behaves exactly like the retail ten-character game.
  //
  // A mod claims a vacant slot with EternalSonataDefineCharacter, which is what
  // supplies the name and the stat template and opens those gates. The mod is
  // expected to ship its own assets too: an asset mod overlays the game
  // partition, so mods/<name>/game/btldata/player/pc011.bop gives character 11
  // a battle model. See EternalSonataCharacterDefinition below.
  ETERNALSONATA_CHAR_FIRST_ADDED = 11,

  ETERNALSONATA_CHARACTER_COUNT = 12
};

// How many characters the retail game itself has. Anything above this is an
// added slot: use it to tell "the original cast" from "the widened cast", for
// instance when walking the game's own ten-entry name tables.
#define ETERNALSONATA_NATIVE_CHARACTER_COUNT 10

// The party's first three display positions are the ones that walk the field
// and fight; everything past that is a reserve.
#define ETERNALSONATA_ACTIVE_PARTY_SIZE 3

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
  // The game's roster (32 entries) is full.
  ETERNALSONATA_PARTY_ERR_ROSTER_FULL = -4,
  // The character costs more party-level budget than is left.
  ETERNALSONATA_PARTY_ERR_PARTY_LEVEL = -5,
  // The story has not made this character recruitable, and the host could not
  // make it eligible.
  ETERNALSONATA_PARTY_ERR_NOT_ELIGIBLE = -6,
  ETERNALSONATA_PARTY_ERR_ALREADY_IN_PARTY = -7,
  ETERNALSONATA_PARTY_ERR_NOT_IN_PARTY = -8,
  ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT = -10,

  // The character is one of the added slots and no mod has defined it yet.
  // Every gate that lets a character into the party refuses a vacant slot.
  ETERNALSONATA_PARTY_ERR_SLOT_VACANT = -11,
  // Every added slot is already defined.
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
  int32_t reserved[9];  // zero-filled; room for later additions
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
// Vacant slots
// ---------------------------------------------------------------------------
//
// Ids above ETERNALSONATA_NATIVE_CHARACTER_COUNT are vacant slots. The host
// holds the widened tables for them and nothing else: until a mod defines one,
// it has no name, no stat template, and the game's own id gates reject it
// exactly as retail did, so it cannot join a party, be drawn on a screen, or
// have a model asked for.
//
// Defining a slot does three things: it records the name every screen resolves
// through, it seeds the slot's entry in the game's per-character stat template
// table (copied from `template_source` so growth curves and starting stats are
// something rather than zero), and it opens the id gates for that one slot.
// Definitions are not persisted in a save: define the slot on every run, before
// touching the party. Defining an already-defined slot with the same owner is
// how you change a definition; use EternalSonataUndefineCharacter to give it up.

// A vacant slot's content. Zero-initialise it, set `struct_size` to
// sizeof(EternalSonataCharacterDefinition), then fill in what you need.
typedef struct EternalSonataCharacterDefinition {
  // sizeof(EternalSonataCharacterDefinition). Lets the host tell which fields a
  // mod built against an older header actually set.
  uint32_t struct_size;

  // Display name, single-byte (CP1252/Latin-1) like
  // EternalSonataSetCharacterName. Required: a slot with no name would draw as
  // a blank the user can neither identify nor click. The host copies it.
  const char* name;

  // Which retail character's stat template the slot starts from, 1..
  // ETERNALSONATA_NATIVE_CHARACTER_COUNT. This is the table the game's own
  // character init and level-up paths read, so it decides starting stats and
  // growth. 0 means ETERNALSONATA_CHAR_ALLEGRETTO.
  int32_t template_source;

  // The pcNNN.bop battle model to load, i.e. the NNN in
  // "btldata\player\pc%03d.bop". 0 means the slot's own id, so character 11
  // asks for pc011.bop and a mod that ships that file needs nothing here. Set
  // it to borrow an existing character's model while the real one is not ready.
  int32_t model_id;

  // Starting own-stats, applied when the character first joins the party.
  // Leave `apply_stats` 0 to let the template decide instead.
  int32_t apply_stats;
  EternalSonataCharacterStats stats;

  uint32_t reserved[8];  // zero-fill; room for later additions
} EternalSonataCharacterDefinition;

// How many vacant slots this host has in total (defined or not), and the
// character id of slot `index` in 0..count-1, or a negative error.
typedef int (*EternalSonataGetAddedSlotCountFn)(void);
typedef int (*EternalSonataGetAddedSlotFn)(int index);

// 1 if `character` has content: always true for the retail cast, true for an
// added slot only once some mod has defined it.
typedef int (*EternalSonataIsCharacterDefinedFn)(int character);

// Claims `character`, which must be an added slot, and fills it in. Defining a
// slot that is already defined replaces the definition, since that is how a mod
// changes its own character; the host cannot tell that apart from two mods
// fighting over one slot, so it logs the replacement and the last caller wins.
// A mod that does not care which slot it gets should call
// EternalSonataDefineNextCharacter instead.
typedef int (*EternalSonataDefineCharacterFn)(
    int character, const EternalSonataCharacterDefinition* definition);

// Claims the first free added slot. Returns its character id (>= 1), or
// ETERNALSONATA_PARTY_ERR_NO_SLOTS when every slot is taken. This is what a mod
// that just wants "a new character" should call, so two such mods can coexist.
typedef int (*EternalSonataDefineNextCharacterFn)(
    const EternalSonataCharacterDefinition* definition);

// Gives a slot back: removes it from the party if it is in one, clears its name
// and template, and closes its id gates again.
typedef int (*EternalSonataUndefineCharacterFn)(int character);

#ifdef __cplusplus
}  // extern "C"
#endif
