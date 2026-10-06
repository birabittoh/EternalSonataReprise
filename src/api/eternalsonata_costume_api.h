// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for character costumes: alternative models a character wears
// in the field, in battle and in cutscenes.
//
// Every character has its default model, and may have more. The PS3 release
// ships four (Allegretto, Polka twice, Beat), which this host offers when it
// runs from PS3 data; the Xbox 360 releases ship none, so on them every
// costume comes from a mod. A mod adds one either from C++ through this
// header or with no code at all, from a [[costume]] table in its assets.toml
// (docs/costumes.md).
//
// A costume is a whole field character model: one NOBJ in the Xbox 360
// layout, the same thing AppKeep.bmd holds for each character. It replaces
// the character's model everywhere the game builds one from then on.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable:
//
//     auto wear = reinterpret_cast<EternalSonataWearCostumeFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataWearCostume"));
//     if (wear) { wear(ETERNALSONATA_COSTUME_CHAR_POLKA, 1); }
//
// Always null-check, and check EternalSonataCostumeAbiVersion() before using
// anything added after version 1.
//
// Threading: every entry point can be called from any thread, the ImGui draw
// thread included, and before the game has booted. A costume worn before the
// boot goes on as the game loads its models.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_COSTUME_ABI_VERSION 2u

// Characters, 1 based, the same numbering as eternalsonata_party_api.h.
enum {
  ETERNALSONATA_COSTUME_CHAR_ALLEGRETTO = 1,
  ETERNALSONATA_COSTUME_CHAR_POLKA = 2,
  ETERNALSONATA_COSTUME_CHAR_BEAT = 3,
  ETERNALSONATA_COSTUME_CHAR_FREDERIC = 4,
  ETERNALSONATA_COSTUME_CHAR_VIOLA = 5,
  ETERNALSONATA_COSTUME_CHAR_SALSA = 6,
  ETERNALSONATA_COSTUME_CHAR_JAZZ = 7,
  ETERNALSONATA_COSTUME_CHAR_FALSETTO = 8,
  ETERNALSONATA_COSTUME_CHAR_CLAVES = 9,
  ETERNALSONATA_COSTUME_CHAR_MARCH = 10,
  ETERNALSONATA_COSTUME_CHARACTER_COUNT = 10
};

// Costume 0 of every character is its default model, id "default".
#define ETERNALSONATA_COSTUME_DEFAULT 0

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_COSTUME_OK = 0,
  ETERNALSONATA_COSTUME_ERR_INVALID_CHARACTER = -2,
  // No costume with that index or id.
  ETERNALSONATA_COSTUME_ERR_INVALID_COSTUME = -3,
  // Not a NOBJ, truncated, or the file could not be read.
  ETERNALSONATA_COSTUME_ERR_INVALID_MODEL = -4,
  // The character already has a costume with that id.
  ETERNALSONATA_COSTUME_ERR_DUPLICATE_ID = -5,
  // Guest memory for the model ran out.
  ETERNALSONATA_COSTUME_ERR_NO_MEMORY = -6,
  // The default costume is always unlocked.
  ETERNALSONATA_COSTUME_ERR_DEFAULT_LOCK = -7,
  ETERNALSONATA_COSTUME_ERR_INVALID_ARGUMENT = -10
};

// Published on the mod registry bus when a character changes costume, by a
// mod, the overlay or anything else. Payload u64 is the character, payload f64
// the new costume index.
#define ETERNALSONATA_COSTUME_EVENT_CHANGED "eternalsonata.costume.changed"

// Published when a costume is unlocked or locked, by a mod or by the game's
// own scripts (the PS3's grants), but not when a save or new game resets the
// locks. Payload u64 is the character, payload f64 the costume index.
#define ETERNALSONATA_COSTUME_EVENT_UNLOCKED "eternalsonata.costume.unlocked"
#define ETERNALSONATA_COSTUME_EVENT_LOCKED "eternalsonata.costume.locked"

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataCostumeAbiVersionFn)(void);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// How many costumes `character` has, its default included, so at least 1; or
// a negative error.
typedef int (*EternalSonataGetCostumeCountFn)(int character);

// A costume's id and its display label (UTF-8). "" for an unknown costume.
// The pointers stay valid for the rest of the session.
typedef const char* (*EternalSonataGetCostumeIdFn)(int character, int costume);
typedef const char* (*EternalSonataGetCostumeLabelFn)(int character, int costume);

// The index of the costume with this id, or a negative error.
typedef int (*EternalSonataFindCostumeFn)(int character, const char* id);

// The costume `character` wears now, or a negative error.
typedef int (*EternalSonataGetWornCostumeFn)(int character);

// ---------------------------------------------------------------------------
// Wearing
// ---------------------------------------------------------------------------

// Puts a costume on. Battles and cutscenes started after this show it, and the
// field leader is rebuilt on the next field frame outside a cutscene. Loads the
// model the first time the costume is worn. Saved with the game. Wearing does
// not check the lock (below); only the costume picker does.
typedef int (*EternalSonataWearCostumeFn)(int character, int costume);

// ---------------------------------------------------------------------------
// Locks
// ---------------------------------------------------------------------------
//
// A locked costume is listed but cannot be picked. Locks belong to the game in
// progress: they are saved with it, and loading a save or starting a new game
// puts every costume back to the state that save or game has. The PS3's
// costumes start locked and the game's scripts unlock them; a mod's start
// unlocked unless it says otherwise.

// 1 if unlocked, 0 if locked, or a negative error.
typedef int (*EternalSonataIsCostumeUnlockedFn)(int character, int costume);

// Unlock or lock a costume in the game in progress. Locking the costume worn
// puts the default back on. The default cannot be locked.
typedef int (*EternalSonataUnlockCostumeFn)(int character, int costume);
typedef int (*EternalSonataLockCostumeFn)(int character, int costume);

// Whether a new game starts with the costume locked (nonzero) or not. Call it
// right after registering; it also sets the current lock. The same as
// `locked = true` in an assets.toml [[costume]].
typedef int (*EternalSonataSetCostumeStartsLockedFn)(int character, int costume, int locked);

// ---------------------------------------------------------------------------
// Adding costumes
// ---------------------------------------------------------------------------
//
// `id` names the costume for EternalSonataFindCostume and must be unique per
// character; prefix it with your mod's name ("my_mod/swimsuit"). `label` is
// what menus show. Both are copied. Costumes keep their registration order
// and cannot be removed. Each returns the new costume's index, or a negative
// error.

// From memory: `model` is a complete NOBJ, `size` bytes long, and is copied.
typedef int (*EternalSonataRegisterCostumeFn)(int character, const char* id, const char* label,
                                              const uint8_t* model, uint32_t size);

// From a file (UTF-8 path, absolute or relative to the working directory),
// read the first time the costume is worn. Only checked for existence here.
typedef int (*EternalSonataRegisterCostumeFileFn)(int character, const char* id,
                                                  const char* label, const char* path);

// Portraits the camp menu shows while the costume is worn, by kind.
enum {
  // The status page; 512 by 512 on PS3 data.
  ETERNALSONATA_COSTUME_PORTRAIT_STATUS = 0,
  // The one to three member panel's full body art; 512 by 512 (Allegretto's
  // own is 256 by 512). Only Allegretto, Polka, Beat and Frederic show it:
  // the others' panels are laid out for their own art.
  ETERNALSONATA_COSTUME_PORTRAIT_PANEL = 1,
  // The faces of the four or more member layouts, the member swaps and the
  // item target list, which pick one set or the other per layout. Both 256
  // by 256: a bust filling the image, and a smaller head with room around it.
  ETERNALSONATA_COSTUME_PORTRAIT_FACE = 2,
  ETERNALSONATA_COSTUME_PORTRAIT_SMALL_FACE = 3,
  ETERNALSONATA_COSTUME_PORTRAIT_KIND_COUNT = 4
};

// Version 2. One portrait of a costume: a DDS file or an NTEX chunk, the size
// of the character's own, read the first time the costume is worn (the status
// portrait: shown). Without one the character's own stays. Once per kind and
// costume, and not for the default.
typedef int (*EternalSonataSetCostumePortraitFileFn)(int character, int costume, int kind,
                                                     const char* path);

#ifdef __cplusplus
}  // extern "C"
#endif
