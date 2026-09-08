// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the Music menu, the three tab gallery of the game's OST:
// which tracks exist, which of them are unlocked, a way to lock or unlock any
// of them, and a way to start one playing.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options API is used (see eternalsonata_options_api.h):
//
//     auto get = reinterpret_cast<EternalSonataGetMusicTrackFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataGetMusicTrack"));
//     EternalSonataMusicTrack track;
//     if (get && get(1, &track) == ETERNALSONATA_MUSIC_OK) { ... }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataMusicAbiVersion() before using anything added
// after version 1.
//
// Events. Both directions of a change are published on the shared mod registry
// bus (rex::system::ModRegistry, reached via runtime->mod_registry()), so a mod
// subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_MUSIC_EVENT_UNLOCKED "eternalsonata.music.unlocked"
//     ETERNALSONATA_MUSIC_EVENT_LOCKED   "eternalsonata.music.locked"
//
// In both the payload's `u64` is the track id and `f64` is the track's tab
// (-1.0 for a track no tab lists). `bytes` is empty. They fire for a track the
// game unlocked as well as for one a mod changed through this API, on the frame
// after the change. Loading a save republishes nothing: whatever the save
// restores is adopted silently.
//
// Threading. Everything here except EternalSonataPlayMusicTrack is a plain
// guest-memory load or store and is safe from any thread, including the ImGui
// draw thread. Playback has to run guest code, so it is queued onto the guest
// main thread and answers ETERNALSONATA_MUSIC_QUEUED.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_MUSIC_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_MUSIC_EVENT_UNLOCKED "eternalsonata.music.unlocked"
#define ETERNALSONATA_MUSIC_EVENT_LOCKED "eternalsonata.music.locked"

// Track ids are 1..66 and double as the number in the track's own
// sound\cxs\MP1<nn>.cxs. The menu range-checks exactly this range.
#define ETERNALSONATA_MUSIC_TRACK_ID_MIN 1
#define ETERNALSONATA_MUSIC_TRACK_ID_MAX 66
#define ETERNALSONATA_MUSIC_TRACK_COUNT 66

// The menu has three tabs and cycles through them modulo 3.
#define ETERNALSONATA_MUSIC_TAB_COUNT 3

// Tab numbering, in the order the menu cycles them.
enum {
  ETERNALSONATA_MUSIC_TAB_EVENT = 0,   // 22 tracks, cutscene themes
  ETERNALSONATA_MUSIC_TAB_FIELD = 1,   // 31 tracks, town and dungeon themes
  ETERNALSONATA_MUSIC_TAB_BATTLE = 2,  // 9 tracks, battle and victory themes

  // Four of the 66 ids (2, 6, 51 and 66) are in no tab and have no title. The
  // menu never draws them, but the flag behind them is real and settable.
  ETERNALSONATA_MUSIC_TAB_NONE = -1,
  // Pass to EternalSonataGetMusicTracks to mean "every tab, and the unlisted
  // tracks too".
  ETERNALSONATA_MUSIC_TAB_ALL = -1
};

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_MUSIC_OK = 0,

  // Playback was queued onto the guest main thread and runs on its next frame.
  ETERNALSONATA_MUSIC_QUEUED = 1,

  // Not readable yet: no runtime bound, or the guest memory holding the flags
  // and the tab table is not mapped (before the title screen finishes loading).
  ETERNALSONATA_MUSIC_ERR_UNAVAILABLE = -1,
  ETERNALSONATA_MUSIC_ERR_NO_SUCH_TRACK = -2,
  // The track has no title and no tab, so there is nothing to play.
  ETERNALSONATA_MUSIC_ERR_NOT_LISTED = -3,
  // Playback was refused because the track is still locked, exactly as the
  // menu refuses a "???" row.
  ETERNALSONATA_MUSIC_ERR_LOCKED = -4,
  ETERNALSONATA_MUSIC_ERR_INVALID_ARGUMENT = -10
};

// Everything the game knows about one OST track. The title is not a string
// here because it lives in a packed UI text blob; use
// EternalSonataGetMusicTrackName, or resolve `title_text_id` yourself.
typedef struct EternalSonataMusicTrack {
  // 1..66. The stable identity, and the number in sound\cxs\MP1<nn>.cxs.
  int32_t track_id;

  // Which tab lists the track, or ETERNALSONATA_MUSIC_TAB_NONE.
  int32_t tab;
  // Position within that tab, 0-based, or -1 when no tab lists it.
  int32_t row;
  // The "No. NN" the menu prints in the left column, i.e. row + 1, or 0. It is
  // a position within the tab and is deliberately not the track id.
  int32_t number;

  // Whether the menu draws the title rather than "???", and whether selecting
  // the row plays anything. Both read the same flag.
  int32_t unlocked;

  // Text id of the title in the menu's own text blob. Always the track id; the
  // menu substitutes id 0, "???", while the track is locked.
  int32_t title_text_id;

  int32_t reserved[6];  // zero-filled; room for later additions
} EternalSonataMusicTrack;

// ---------------------------------------------------------------------------
// Capability and state
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataMusicAbiVersionFn)(void);

// True once the flags and the tab table are readable. False before the title
// screen has finished coming up.
typedef int (*EternalSonataIsMusicAvailableFn)(void);

// Always ETERNALSONATA_MUSIC_TRACK_COUNT. Present so a mod can size a buffer
// without hard-coding the constant it was built against.
typedef int (*EternalSonataGetMusicTrackCountFn)(void);

// How many tracks `tab` lists (22, 31, 9), or the count of unlisted tracks for
// ETERNALSONATA_MUSIC_TAB_NONE. Negative on error.
typedef int (*EternalSonataGetMusicTabTrackCountFn)(int tab);

// How many tracks are unlocked right now, 0..66, or a negative error.
typedef int (*EternalSonataGetUnlockedMusicTrackCountFn)(void);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Fills `out` with the track `track_id` (1..66). Returns
// ETERNALSONATA_MUSIC_OK or a negative error.
typedef int (*EternalSonataGetMusicTrackFn)(int track_id, EternalSonataMusicTrack* out);

// Fills `out` with up to `max` tracks and returns how many were written, or a
// negative error. `tab` is a tab number for that tab's tracks in menu order,
// or ETERNALSONATA_MUSIC_TAB_ALL for all 66 in track id order. Pass max = 0 to
// just count.
typedef int (*EternalSonataGetMusicTracksFn)(EternalSonataMusicTrack* out, int max, int tab);

// 1 if the track is unlocked, 0 if it is not, or a negative error.
typedef int (*EternalSonataIsMusicTrackUnlockedFn)(int track_id);

// The track's title in the language the game is running in, or "" for an id
// with no title. Never null; the string is owned by the host and stays valid.
// It is the real title whether or not the track is unlocked, so a mod can list
// what is still missing.
typedef const char* (*EternalSonataGetMusicTrackNameFn)(int track_id);

// ---------------------------------------------------------------------------
// Changing
// ---------------------------------------------------------------------------

// Locks or unlocks one track. The flag is the one the save carries, so the
// change survives a save and reload. Returns ETERNALSONATA_MUSIC_OK or a
// negative error, and the matching event follows on the next frame tick if the
// state actually changed.
typedef int (*EternalSonataSetMusicTrackUnlockedFn)(int track_id, int unlocked);

// The same for every track, including the four no tab lists. Returns how many
// changed (tracks already in the requested state are skipped), or a negative
// error.
typedef int (*EternalSonataSetAllMusicTracksUnlockedFn)(int unlocked);

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

// Starts sound\cxs\MP1<nn>.cxs as the current BGM, the same call the menu makes
// when a row is chosen, and like the menu it refuses a locked track with
// ETERNALSONATA_MUSIC_ERR_LOCKED. Returns ETERNALSONATA_MUSIC_QUEUED when it
// had to be queued onto the guest main thread, ETERNALSONATA_MUSIC_OK when it
// ran inline. Whatever normally owns the BGM (the field or battle music
// manager) replaces it at the next transition.
typedef int (*EternalSonataPlayMusicTrackFn)(int track_id);

#ifdef __cplusplus
}  // extern "C"
#endif
