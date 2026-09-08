// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the Piano Music menu: the seven Chopin piano pieces the
// game lists there, whether each one is unlocked, and a way to lock or unlock
// any of them.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options API is used (see eternalsonata_options_api.h):
//
//     auto get = reinterpret_cast<EternalSonataGetPianoMusicFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataGetPianoMusic"));
//     EternalSonataPianoMusic piece;
//     if (get && get(0, &piece) == ETERNALSONATA_PIANO_MUSIC_OK) { ... }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataPianoMusicAbiVersion() before using anything
// added after version 1.
//
// Events. Both directions of a change are published on the shared mod registry
// bus (rex::system::ModRegistry, reached via runtime->mod_registry()), so a mod
// subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_PIANO_MUSIC_EVENT_UNLOCKED "eternalsonata.pianomusic.unlocked"
//     ETERNALSONATA_PIANO_MUSIC_EVENT_LOCKED   "eternalsonata.pianomusic.locked"
//
// In both the payload's `u64` is the piece index (0..6, the same value
// EternalSonataPianoMusic::index carries) and `f64` is the piece's flag id, so
// a subscriber can go straight to EternalSonataGetPianoMusic. `bytes` is empty.
//
//     runtime->mod_registry()->Subscribe(
//         "eternalsonata.pianomusic.unlocked",
//         [](const rex::system::ModRegistry::EventPayload& p) {
//           REXLOG_INFO("piano music {} unlocked", p.u64);
//         });
//
// They fire for a piece the game unlocked as well as for one a mod changed
// through this API, on the frame after the change. Loading a save republishes
// nothing: whatever the save restores is adopted silently.
//
// Threading. Every entry point here is a plain guest-memory load or store and
// is safe from any thread, including the ImGui draw thread; nothing here runs
// guest code, so there is no queued-write path and no QUEUED result. The
// events are published from the mod registry's frame tick. Subscribers still
// have to be thread-safe, because the tick is not the ImGui draw thread.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_PIANO_MUSIC_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_PIANO_MUSIC_EVENT_UNLOCKED "eternalsonata.pianomusic.unlocked"
#define ETERNALSONATA_PIANO_MUSIC_EVENT_LOCKED "eternalsonata.pianomusic.locked"

// The menu lists exactly seven pieces and the game offers no way to grow it:
// the id table it walks is seven entries wide and the screen's own row layout
// is built from the same walk.
#define ETERNALSONATA_PIANO_MUSIC_COUNT 7

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_PIANO_MUSIC_OK = 0,

  // Not readable yet: no runtime bound, or the guest memory holding the flags
  // is not mapped (before the title screen finishes loading).
  ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE = -1,
  ETERNALSONATA_PIANO_MUSIC_ERR_NO_SUCH_PIECE = -2,
  ETERNALSONATA_PIANO_MUSIC_ERR_INVALID_ARGUMENT = -10
};

// Everything the game knows about one piece of piano music. The piece's title
// and its Chopin history pages are not strings here: they live in the packed
// UI text blob and are reached by numeric id, so the ids are exposed instead.
typedef struct EternalSonataPianoMusic {
  // Position in the menu, 0..6, and the stable identity: the list never
  // reorders. 0 is Raindrops and 6 is Heroic.
  int32_t index;

  // The piece's slot in the game's 100-entry collectible flag array, 80..86.
  // It doubles as the number in the piece's own sound\cxs\MP1<n>.wav.
  int32_t flag_id;

  // Whether the menu draws the piece's title rather than "???".
  int32_t unlocked;
  // The two ways that can be true, exposed separately. `saved` is the flag the
  // save file carries; `session` is the extra bit the game sets for a piece it
  // has made available for this session only, which does not survive a reload.
  int32_t unlocked_saved;
  int32_t unlocked_session;

  // Text ids into the packed UI blob. `title_text_id` is the piece's name and
  // is always index; the menu substitutes `locked_title_text_id` while the
  // piece is locked. The history pages run from `first_story_text_id` for
  // `story_page_count` consecutive ids.
  int32_t title_text_id;
  int32_t locked_title_text_id;
  int32_t first_story_text_id;
  int32_t story_page_count;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataPianoMusic;

// ---------------------------------------------------------------------------
// Capability and state
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataPianoMusicAbiVersionFn)(void);

// True once the flag array is readable. False before the title screen has
// finished coming up.
typedef int (*EternalSonataIsPianoMusicAvailableFn)(void);

// Always ETERNALSONATA_PIANO_MUSIC_COUNT. Present so a mod can size a buffer
// without hard-coding the constant it was built against.
typedef int (*EternalSonataGetPianoMusicCountFn)(void);

// How many pieces are unlocked right now, 0..7, or a negative error.
typedef int (*EternalSonataGetUnlockedPianoMusicCountFn)(void);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Fills `out` with the piece at `index` (0..6). Returns
// ETERNALSONATA_PIANO_MUSIC_OK or a negative error.
typedef int (*EternalSonataGetPianoMusicFn)(int index, EternalSonataPianoMusic* out);

// Fills `out` with up to `max` pieces in menu order and returns how many were
// written, or a negative error. Pass max = 0 to just count.
typedef int (*EternalSonataGetAllPianoMusicFn)(EternalSonataPianoMusic* out, int max);

// 1 if the piece at `index` is unlocked, 0 if it is not, or a negative error.
// Shorthand for reading the field of the same name.
typedef int (*EternalSonataIsPianoMusicUnlockedFn)(int index);

// ---------------------------------------------------------------------------
// Changing
// ---------------------------------------------------------------------------

// Locks or unlocks the piece at `index`. Unlocking sets the saved flag, so it
// survives a save and reload; locking clears both that flag and the
// session-only bit, so the menu draws "???" again straight away. Returns
// ETERNALSONATA_PIANO_MUSIC_OK or a negative error, and the matching event
// follows on the next frame tick if the state actually changed.
typedef int (*EternalSonataSetPianoMusicUnlockedFn)(int index, int unlocked);

// The same for every piece. Returns how many changed (pieces already in the
// requested state are skipped), or a negative error.
typedef int (*EternalSonataSetAllPianoMusicUnlockedFn)(int unlocked);

#ifdef __cplusplus
}  // extern "C"
#endif
