// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the in-game camera's photo album: the twelve photographs
// the status menu's Photos tab lists, every field the game stores about each
// one, and a way to finish a photograph's development immediately.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options API is used (see eternalsonata_options_api.h):
//
//     auto get = reinterpret_cast<EternalSonataGetPhotoFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataGetPhoto"));
//     EternalSonataPhoto photo;
//     if (get && get(0, &photo) == ETERNALSONATA_PHOTO_OK) { ... }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataPhotoAbiVersion() before using anything added
// after version 1.
//
// Events. Two points in a photograph's life are published on the shared mod
// registry bus (rex::system::ModRegistry, reached via runtime->mod_registry()),
// so a mod subscribes by name and needs neither this header nor a linked
// symbol:
//
//     ETERNALSONATA_PHOTO_EVENT_ADDED      "eternalsonata.photo.added"
//     ETERNALSONATA_PHOTO_EVENT_DEVELOPED  "eternalsonata.photo.developed"
//
// In both the payload's `u64` is the record slot (0..11, the same value
// EternalSonataPhoto::record carries) and `f64` is the photograph's display
// index at the time, so a subscriber can go straight to EternalSonataGetPhoto.
// `bytes` is empty.
//
//     runtime->mod_registry()->Subscribe(
//         "eternalsonata.photo.developed",
//         [](const rex::system::ModRegistry::EventPayload& p) {
//           REXLOG_INFO("photo in slot {} finished developing", p.u64);
//         });
//
// `added` fires once per new photograph, on the frame after the game commits
// it to the album. Loading a save republishes nothing: the album the save
// restores is adopted silently, however many photographs it holds.
//
// `developed` fires when a photograph's development reaches completion, which
// the game models as a continuous value that decays with elapsed time and
// party progress rather than as a countdown. The host evaluates it once per
// frame with the game's own formula, so the event lands when development
// actually completes, not when the player next opens the album.
//
// Threading. Every read here is a plain guest-memory load and is safe from any
// thread, including the ImGui draw thread; nothing here runs guest code, so
// there is no queued-write path and no QUEUED result. The two events are
// published from the mod registry's frame tick. Subscribers still have to be
// thread-safe, because the tick is not the ImGui draw thread.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_PHOTO_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_PHOTO_EVENT_ADDED "eternalsonata.photo.added"
#define ETERNALSONATA_PHOTO_EVENT_DEVELOPED "eternalsonata.photo.developed"

// The album holds twelve photographs and the game offers no way to grow it:
// the record array, the display-order table, the per-slot texture handles and
// the album screen's own widget arrays are all twelve entries wide.
#define ETERNALSONATA_PHOTO_CAPACITY 12

// Up to six subjects are recorded per photograph, in two groups of three. The
// game fills the first group from what the lens was pointed at and the second
// from a parallel pass; both are exposed, in that order.
#define ETERNALSONATA_PHOTO_SUBJECT_SLOTS 6

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_PHOTO_OK = 0,

  // The album is not readable yet: no runtime bound, or guest memory for the
  // record array is not mapped (before the title screen finishes loading).
  ETERNALSONATA_PHOTO_ERR_UNAVAILABLE = -1,
  // No photograph at that display index, or no record in that slot.
  ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO = -2,
  ETERNALSONATA_PHOTO_ERR_INVALID_ARGUMENT = -10
};

// A photograph's quality tier, the three-way split the album's own icon uses.
// The game derives it from `score` alone, at 0.5 and 0.2.
enum {
  ETERNALSONATA_PHOTO_RANK_BEST = 0,    // score > 0.5
  ETERNALSONATA_PHOTO_RANK_MIDDLE = 1,  // 0.2 < score <= 0.5
  ETERNALSONATA_PHOTO_RANK_WORST = 2    // score <= 0.2
};

// One thing the camera caught. Twenty bytes in the game's own record, exposed
// whole: the id is what the entity tables are keyed by, and the four values
// are the framing data the score was computed from (their individual meanings
// are not decoded, so they are passed through raw rather than named wrongly).
typedef struct EternalSonataPhotoSubject {
  int32_t id;         // -1 when the entry is empty
  int32_t attribute;  // the second u16 of the entry
  int32_t values[4];
} EternalSonataPhotoSubject;

// Everything the game stores about one photograph. The image itself is not
// here: it lives in a guest texture surface that only guest code can lock, so
// `image_handle` is exposed instead of the pixels.
typedef struct EternalSonataPhoto {
  // Position in the album's display order, 0-based. The newest photograph is
  // always 0; the game pushes new ones onto the front.
  int32_t index;
  // Which of the twelve record slots holds it. This is the stable identity -
  // display indices shift as photographs are added and sold - and it is what
  // both events carry.
  int32_t record;

  // Raw flag word. Bit 1 means the record holds no picture, which is the state
  // a photograph is in between being taken and being committed; the album
  // draws those black with a "developing" caption.
  int32_t flags;
  int32_t is_blank;  // flags bit 1

  // Quality, 0..1. The album prints score_percent next to the photograph and
  // picks its icon from rank.
  float score;
  int32_t score_percent;  // (int)(score * 100)
  int32_t rank;           // one of ETERNALSONATA_PHOTO_RANK_*

  // What the shop pays, before the album scales it by how far the photograph
  // has developed. The final price is a guest computation and is not exposed.
  float price_scale;

  // The two snapshots the development formula runs on: the value of the
  // game's own clock when the photograph was taken, and the party-progress
  // counter at the same moment (-1 on a record that was never filled).
  uint32_t taken_time;
  int32_t taken_progress;

  // How much haze is left over the picture: 1 is freshly taken, 0 is fully
  // developed. Recomputed live, so it is current even when the album screen
  // has not been opened since the photograph was taken.
  float development;
  int32_t is_developed;  // development == 0

  // Guest handle of the 416x256 R5G6B5 surface holding the picture, or 0 when
  // no surface is resident.
  uint32_t image_handle;

  // How many of `subjects` have a non-empty id. The filled entries are not
  // necessarily the leading ones, so walk all of them and skip id < 0.
  int32_t subject_count;
  EternalSonataPhotoSubject subjects[ETERNALSONATA_PHOTO_SUBJECT_SLOTS];

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataPhoto;

// ---------------------------------------------------------------------------
// Capability and state
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataPhotoAbiVersionFn)(void);

// True once the album's guest storage is readable. False before the title
// screen has finished coming up.
typedef int (*EternalSonataIsPhotoAlbumAvailableFn)(void);

// How many photographs the album holds right now, or a negative error.
typedef int (*EternalSonataGetPhotoCountFn)(void);

// Always ETERNALSONATA_PHOTO_CAPACITY. Present so a mod can size a buffer
// without hard-coding the constant it was built against.
typedef int (*EternalSonataGetPhotoCapacityFn)(void);

// ---------------------------------------------------------------------------
// Reading photographs
// ---------------------------------------------------------------------------

// Fills `out` with the photograph at 0-based display `index`. Returns
// ETERNALSONATA_PHOTO_OK or a negative error.
typedef int (*EternalSonataGetPhotoFn)(int index, EternalSonataPhoto* out);

// The same, keyed by record slot 0..11 - the identity the events carry, and
// the one that does not move when the album is reordered.
typedef int (*EternalSonataGetPhotoByRecordFn)(int record, EternalSonataPhoto* out);

// Fills `out` with up to `max` photographs in display order and returns how
// many were written, or a negative error. Pass max = 0 to just count.
typedef int (*EternalSonataGetPhotosFn)(EternalSonataPhoto* out, int max);

// Development of the photograph at display `index`, 1 freshly taken down to 0
// fully developed, or a negative error. Shorthand for reading the field of the
// same name.
typedef float (*EternalSonataGetPhotoDevelopmentFn)(int index);

// ---------------------------------------------------------------------------
// Changing photographs
// ---------------------------------------------------------------------------

// Finishes the photograph at display `index` immediately: its development
// becomes 0 and stays there. If it had not already finished,
// ETERNALSONATA_PHOTO_EVENT_DEVELOPED follows on the next frame tick, like any
// other development that completes.
//
// The two snapshots the formula reads are rewritten to their fully-aged
// values, so the change survives a save and reload. Early in a playthrough
// those values alone do not quite reach zero - the formula also needs elapsed
// clock - so the host additionally pins the result for the rest of the
// session, which is what makes the album draw the photograph clean right away.
typedef int (*EternalSonataDevelopPhotoFn)(int index);

// The same for every photograph in the album. Returns how many were changed
// (photographs already developed are skipped), or a negative error.
typedef int (*EternalSonataDevelopAllPhotosFn)(void);

#ifdef __cplusplus
}  // extern "C"
#endif
