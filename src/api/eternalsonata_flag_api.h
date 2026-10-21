// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's flag system: the bit bank the field scripts use
// to remember what has happened, plus the scenario counter that follows it.
// Both work the same on 360 and PS3 data (docs/flags.md §7).
//
// The flags are the game's own progress state. Every chest opened, event seen,
// door unlocked and party change is a bit in one 2048-byte array, and the
// scripts are the only thing that reads or writes it: no code in the
// executable touches the array, it is handed to the `.e` bytecode as an
// exported symbol. So a mod that sets a flag is talking to the scripts in
// their own language, and the script that next checks that flag will believe
// it.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable:
//
//     auto get = reinterpret_cast<EternalSonataGetFlagFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataGetFlag"));
//     if (get) { ... }
//
// Always null-check, and check EternalSonataFlagsAbiVersion() before using
// anything added after version 1.
//
// Threading: every entry point is a plain guest-memory access and can be made
// from any thread. The events are published from the mod registry's frame
// tick.
//
// There is no name-to-index table: the game ships none, and the flag numbers
// are only meaningful to the scripts. The practical way to find the flag you
// want is to save either side of the event you care about and diff the bank
// with EternalSonataCopyFlags.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning.
#define ETERNALSONATA_FLAGS_ABI_VERSION 3u

// Flags in the bank. Flag `n` is bit `n & 7` of byte `n >> 3`, counting from
// the low bit: the order the game fills them in.
#define ETERNALSONATA_FLAG_COUNT 16384

// Bytes of the bank, for EternalSonataCopyFlags.
#define ETERNALSONATA_FLAG_BYTES 2048

enum {
  ETERNALSONATA_FLAG_OK = 0,
  // No runtime yet, or guest memory is not up. Before the title screen the
  // bank does not exist.
  ETERNALSONATA_FLAG_ERR_UNAVAILABLE = -1,
  // Outside 0..ETERNALSONATA_FLAG_COUNT-1, or a byte range outside the bank.
  ETERNALSONATA_FLAG_ERR_INVALID_INDEX = -2,
  // Not a value the field accepts.
  ETERNALSONATA_FLAG_ERR_INVALID_VALUE = -3,
  // Version 3. The save slot is empty or its file is not a game save.
  ETERNALSONATA_FLAG_ERR_NO_SAVE = -4
};

// Save point location ids, the string a save slot shows under the chapter.
// Every save point passes its own; these two are special.
#define ETERNALSONATA_SAVE_LOCATION_DEBUG_SAVE 0
// The save the ending offers. Loading one asks whether to start Encore.
#define ETERNALSONATA_SAVE_LOCATION_GAME_CLEAR 1

// The Encore counter saturates here.
#define ETERNALSONATA_ENCORE_COUNT_MAX 255

// Bits of EternalSonataGetEncoreState. A slot with either bit set wears the
// reward icon in the load menu.
#define ETERNALSONATA_ENCORE_STATE_GAME_CLEAR 1  // loading it offers Encore
#define ETERNALSONATA_ENCORE_STATE_ENCORE 2      // an Encore playthrough

// Published on the mod registry bus when a flag changes, whoever changed it:
// a script, a mod, or a new game clearing the bank. Payload u64 is the flag
// index, payload f64 is its new value (0 or 1).
#define ETERNALSONATA_FLAG_EVENT_CHANGED "eternalsonata.flag.changed"

// Published instead of a storm of the above when a whole swathe of the bank
// changes at once, which in practice means a new game or a reset. Payload u64
// is how many flags changed. A subscriber that mirrors flag state should treat
// this as "re-read everything".
#define ETERNALSONATA_FLAG_EVENT_BULK_CHANGED "eternalsonata.flags.bulkchanged"

// Published when the scenario counter changes. Payload f64 is its new value.
#define ETERNALSONATA_FLAG_EVENT_SCENARIO_CHANGED \
  "eternalsonata.flags.scenario.changed"

// Published when the Scrapbook's unlocked state changes. Payload f64 is its
// new value (0 or 1).
#define ETERNALSONATA_FLAG_EVENT_SCRAPBOOK_CHANGED \
  "eternalsonata.flags.scrapbook.changed"

// The same for the costumes blocked state.
#define ETERNALSONATA_FLAG_EVENT_COSTUMES_BLOCKED_CHANGED \
  "eternalsonata.flags.costumesblocked.changed"

// Published when the Encore counter changes. Payload f64 is its new value.
#define ETERNALSONATA_FLAG_EVENT_ENCORE_COUNT_CHANGED \
  "eternalsonata.flags.encorecount.changed"

// Published when the save location changes, which a save point does every
// time the party steps on it. Payload f64 is the new location id.
#define ETERNALSONATA_FLAG_EVENT_SAVE_LOCATION_CHANGED \
  "eternalsonata.flags.savelocation.changed"

// A loaded save replaces the whole bank. That is adopted silently rather than
// reported, so none of the events fire for it.

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataFlagsAbiVersionFn)(void);

// 1 once the bank is readable, 0 before that.
typedef int (*EternalSonataAreFlagsAvailableFn)(void);

// ETERNALSONATA_FLAG_COUNT, so a mod compiled against an older header still
// asks the host how many there are.
typedef int (*EternalSonataGetFlagCountFn)(void);

// 0 or 1, or a negative error. Values are never negative, so the two never
// collide.
typedef int (*EternalSonataGetFlagFn)(int index);

// Sets a flag to 0 or 1. Returns ETERNALSONATA_FLAG_OK or a negative error.
typedef int (*EternalSonataSetFlagFn)(int index, int value);

// Copies `byte_count` bytes of the bank starting at byte `first_byte` into
// `dst`. This is the diffing entry point: read it before and after an event to
// find that event's flag. Returns ETERNALSONATA_FLAG_OK or a negative error.
typedef int (*EternalSonataCopyFlagsFn)(uint8_t* dst, int first_byte,
                                        int byte_count);

// Clears every flag, leaving the scenario counter alone. This is what the
// debug room's "specify 0 and every flag resets" does, and it is as
// destructive as it sounds: the scripts will replay the whole game's worth of
// events.
typedef int (*EternalSonataResetFlagsFn)(void);

// The scenario counter: the coarse "how far in are we" number the debug room
// lets you set directly, chapter times 1000 plus progress within it (1130 is
// early chapter 1). Set accepts 0..INT32_MAX. Returns the value or a negative
// error.
typedef int (*EternalSonataGetScenarioCounterFn)(void);
typedef int (*EternalSonataSetScenarioCounterFn)(int value);

// Version 2. Whether the camp menu shows the Scrapbook, PS3 data only: the
// camp menu flag Bel01.e sets through native 5029, kept outside the bank and
// saved with the game. The strip reads it when the camp menu opens. Get
// answers 0 or 1, set takes 0 or 1; both answer
// ETERNALSONATA_FLAG_ERR_UNAVAILABLE on 360 data.
typedef int (*EternalSonataGetScrapbookUnlockedFn)(void);
typedef int (*EternalSonataSetScrapbookUnlockedFn)(int value);

// Version 2. Whether the story has costume changes off, PS3 data only: the
// camp menu flag scripts set through native 5032, kept and answered like the
// Scrapbook's above. Nothing in the host acts on it yet.
typedef int (*EternalSonataGetCostumesBlockedFn)(void);
typedef int (*EternalSonataSetCostumesBlockedFn)(int value);

// Version 3. New game plus (docs/flags.md §8), on both targets.
//
// The Encore counter is how many times this playthrough's lineage started
// Encore: 0 on a first run, one more each time a Game Clear save is loaded
// and Encore accepted. While it is nonzero enemies have 1.3 times their
// stats and deal 1.5 times the damage, and the Party Level menu lets the
// player pick any level reached so far.
// Set takes 0..ETERNALSONATA_ENCORE_COUNT_MAX. Both return the value or a
// negative error.
typedef int (*EternalSonataGetEncoreCountFn)(void);
typedef int (*EternalSonataSetEncoreCountFn)(int value);

// The location id the next save records. Save points set it as the party
// reaches them and the ending sets ETERNALSONATA_SAVE_LOCATION_GAME_CLEAR, so
// to turn a save into a Game Clear one, set it while the save menu is open.
// A save made away from a save point records
// ETERNALSONATA_SAVE_LOCATION_DEBUG_SAVE whatever this holds. Set takes
// 0..INT32_MAX. Both return the value or a negative error.
typedef int (*EternalSonataGetSaveLocationFn)(void);
typedef int (*EternalSonataSetSaveLocationFn)(int value);

// ETERNALSONATA_ENCORE_STATE_* bits for the game in memory, or a negative
// error.
typedef int (*EternalSonataGetEncoreStateFn)(void);

// The same bits for the save in `slot` (0..9, the menu's number minus one),
// read from the file on disk exactly as the load menu does. Returns
// ETERNALSONATA_FLAG_ERR_NO_SAVE for an empty slot.
typedef int (*EternalSonataGetSaveSlotEncoreStateFn)(int slot);

#ifdef __cplusplus
}  // extern "C"
#endif
