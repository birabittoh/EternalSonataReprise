// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for observing the game's save system: whether the game would
// let the player save right now, what is in each save slot, and whether a save
// is running. The write side (making the game save or load a slot on demand)
// is deliberately not here yet.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the party and battle APIs are used:
//
//     auto can_save = reinterpret_cast<EternalSonataCanSaveFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataCanSave"));
//     if (can_save && can_save() == 1) { ... }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataSaveAbiVersion() before using anything added
// after version 1.
//
// Events. The three points of a save are published on the shared mod registry
// bus (rex::system::ModRegistry, reached via runtime->mod_registry()), so a
// mod subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_SAVE_EVENT_STARTED    "eternalsonata.save.started"
//     ETERNALSONATA_SAVE_EVENT_COMPLETED  "eternalsonata.save.completed"
//     ETERNALSONATA_SAVE_EVENT_FAILED     "eternalsonata.save.failed"
//
// In every one of them the payload's `u64` is the slot, 0..9, and `bytes` is
// empty. `failed` additionally carries the reason in `f64`, as one of the
// ETERNALSONATA_SAVE_FAIL_* values below. Exactly one of completed/failed
// follows each started.
//
//     runtime->mod_registry()->Subscribe(
//         "eternalsonata.save.completed",
//         [](const rex::system::ModRegistry::EventPayload& p) {
//           REXLOG_INFO("saved to slot {}", p.u64);
//         });
//
// Threading. The reads below are plain guest-memory loads and are safe from
// any thread, including the ImGui draw thread; they never run guest code.
// EternalSonataListSaves touches the host filesystem instead and is likewise
// safe anywhere, but it does I/O, so do not call it every frame.
//
// The events are a different matter: they are published from whichever guest
// thread reached that point of the save, NOT from the render/UI thread and not
// from the per-frame tick. `started` comes from the thread that drove the save
// menu; `completed`/`failed` come from the content worker thread the game
// spawns for the write. A subscriber must therefore be thread-safe, must not
// touch ImGui, and should hand anything expensive to its own tick.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_SAVE_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_SAVE_EVENT_STARTED "eternalsonata.save.started"
#define ETERNALSONATA_SAVE_EVENT_COMPLETED "eternalsonata.save.completed"
#define ETERNALSONATA_SAVE_EVENT_FAILED "eternalsonata.save.failed"

// The game's save slots. Its own menu offers ten, numbered 0..9, and the
// container each one lives in is named "savecontentNN" after that number.
#define ETERNALSONATA_SAVE_SLOT_COUNT 10

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_SAVE_OK = 0,

  // The guest module is not up yet, or the save system has not been bound to
  // the runtime. Every entry point answers this before the game is live.
  ETERNALSONATA_SAVE_ERR_UNAVAILABLE = -1,
  ETERNALSONATA_SAVE_ERR_INVALID_ARGUMENT = -2,
  // The save directory could not be read.
  ETERNALSONATA_SAVE_ERR_IO = -3
};

// Why a save failed, carried in the `failed` event's `f64`.
enum {
  // The game refused to start the write at all: no free content handle, the
  // storage device went away, or the request could not be built.
  ETERNALSONATA_SAVE_FAIL_NOT_STARTED = 1,
  // The write ran and reported failure (out of space, I/O error, container
  // could not be created).
  ETERNALSONATA_SAVE_FAIL_WRITE = 2
};

// What the last save this host saw ended as.
enum {
  ETERNALSONATA_SAVE_STATE_NONE = 0,     // no save attempted this run
  ETERNALSONATA_SAVE_STATE_RUNNING = 1,  // a save is in flight right now
  ETERNALSONATA_SAVE_STATE_COMPLETED = 2,
  ETERNALSONATA_SAVE_STATE_FAILED = 3
};

// One save slot on disk. `struct_size` is set by the host to the size of the
// struct it filled in, so a mod built against an older header can tell how
// much of a newer struct it actually got.
typedef struct EternalSonataSaveSlot {
  uint32_t struct_size;
  // 0..9, parsed from the container name. -1 if the name did not follow the
  // game's own "savecontentNN" convention.
  int32_t slot;
  // The container's file name, e.g. "savecontent00". NUL terminated.
  char file_name[44];
  // The container's display name, converted from UTF-16 to UTF-8. This is
  // what the game wrote when it saved (the slot label), and may be empty.
  char display_name[192];
  // Last write time of the slot's data file, in seconds since the Unix epoch,
  // or 0 if it could not be read.
  uint64_t modified_time;
  // Total bytes of every file in the container.
  uint64_t size_bytes;
} EternalSonataSaveSlot;

// Version of this ABI the running host implements.
uint32_t EternalSonataSaveAbiVersion(void);

// 1 if the game would let the player save right now, i.e. the pause menu's
// Save entry is selectable (the party is standing at a save point, or a mod
// has forced that flag). 0 if it would not. ETERNALSONATA_SAVE_ERR_UNAVAILABLE
// before the guest module is up.
//
// This reports the game's gate, not a prediction about storage: a save can
// still fail afterwards, which is what the `failed` event is for.
int EternalSonataCanSave(void);

// Forces the pause menu's Save entry to be selectable everywhere, instead of
// only while the party is standing at a save point. Returns
// ETERNALSONATA_SAVE_OK, or ETERNALSONATA_SAVE_ERR_UNAVAILABLE before the
// guest module is up. It is a host-side toggle and stays set until cleared or
// the process exits; it is not saved anywhere.
//
// It lives in the host because the gate is a guest flag the field scripts
// rewrite on their own schedule, and the host sets it immediately before the
// menu build reads it.
//
// Only the menu gate is forced; the save itself is the game's own, so a forced
// save still publishes the same events and can still fail.
int EternalSonataSetSaveAlwaysAllowed(int enabled);

// 1 if the gate above is currently forced, 0 if not.
int EternalSonataIsSaveAlwaysAllowed(void);

// 1 while a save is in flight, 0 otherwise, ETERNALSONATA_SAVE_ERR_UNAVAILABLE
// before the guest module is up. Between the `started` event and whichever of
// `completed`/`failed` follows it.
int EternalSonataIsSaveInProgress(void);

// One of ETERNALSONATA_SAVE_STATE_*, describing the most recent save this run.
int EternalSonataGetSaveState(void);

// The slot of the most recent save this run, or -1 if there has not been one.
int EternalSonataGetLastSaveSlot(void);

// How many slots the game's own save menu offers (ETERNALSONATA_SAVE_SLOT_COUNT).
int EternalSonataGetSaveSlotCount(void);

// Fills `out` with up to `max` entries, one per save container that actually
// exists on disk, ordered by slot. Returns the number written, or a negative
// error. Passing out = NULL / max = 0 returns the number of saves that exist
// without writing anything, so a caller can size its own array first.
//
// Each entry's `struct_size` is set to sizeof(EternalSonataSaveSlot) as the
// host knows it; a mod must not read past that.
int EternalSonataListSaves(EternalSonataSaveSlot* out, int max);

// Writes the host filesystem path of the directory the current profile's
// saves live in, NUL terminated, into `out` (of `max` bytes). Returns the
// number of bytes written excluding the terminator, or a negative error.
// Useful for backing up or inspecting a save from a mod.
int EternalSonataGetSaveDirectory(char* out, int max);

typedef uint32_t (*EternalSonataSaveAbiVersionFn)(void);
typedef int (*EternalSonataCanSaveFn)(void);
typedef int (*EternalSonataSetSaveAlwaysAllowedFn)(int);
typedef int (*EternalSonataIsSaveAlwaysAllowedFn)(void);
typedef int (*EternalSonataIsSaveInProgressFn)(void);
typedef int (*EternalSonataGetSaveStateFn)(void);
typedef int (*EternalSonataGetLastSaveSlotFn)(void);
typedef int (*EternalSonataGetSaveSlotCountFn)(void);
typedef int (*EternalSonataListSavesFn)(EternalSonataSaveSlot*, int);
typedef int (*EternalSonataGetSaveDirectoryFn)(char*, int);

#ifdef __cplusplus
}  // extern "C"
#endif
