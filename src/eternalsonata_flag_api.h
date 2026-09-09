// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's flag system: the bit bank the field scripts use
// to remember what has happened, plus the scenario counter that sits at the
// end of it.
//
// The flags are the game's own progress state. Every chest opened, event seen,
// door unlocked and party change is a bit in one 2046-byte array, and the
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

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_FLAGS_ABI_VERSION 1u

// Flags in the bank. Flag `n` is bit `n & 7` of byte `n >> 3`, counting from
// the low bit: the order the game fills them in.
#define ETERNALSONATA_FLAG_COUNT 16368

// Bytes of the bank, for EternalSonataCopyFlags. The last two bytes of the
// 2048-byte array are the scenario counter and are not flags, which is why
// this is 2046 and not 2048.
#define ETERNALSONATA_FLAG_BYTES 2046

enum {
  ETERNALSONATA_FLAG_OK = 0,
  // No runtime yet, or guest memory is not up. Before the title screen the
  // bank does not exist.
  ETERNALSONATA_FLAG_ERR_UNAVAILABLE = -1,
  // Outside 0..ETERNALSONATA_FLAG_COUNT-1, or a byte range outside the bank.
  ETERNALSONATA_FLAG_ERR_INVALID_INDEX = -2,
  // Not a value the field accepts.
  ETERNALSONATA_FLAG_ERR_INVALID_VALUE = -3
};

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

// A loaded save replaces the whole bank. That is adopted silently rather than
// reported, so none of the three events fire for it.

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

// The scenario counter, 0..65535: the coarse "how far in are we" number the
// debug room lets you set directly. Returns the value or a negative error.
typedef int (*EternalSonataGetScenarioCounterFn)(void);
typedef int (*EternalSonataSetScenarioCounterFn)(int value);

#ifdef __cplusplus
}  // extern "C"
#endif
