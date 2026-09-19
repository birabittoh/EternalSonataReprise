// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's cutscenes ("events" in the scripts: the E%04d.e
// files a map script starts while the field stays loaded): whether one is
// running, whether the game would let the player skip it, and skipping it.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the overworld API is used:
//
//     auto skip = reinterpret_cast<EternalSonataSkipCutsceneFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataSkipCutscene"));
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataCutsceneAbiVersion() before using anything
// added after version 1.
//
// The game's own skip is the pause menu: Start during a skippable cutscene,
// then A. EternalSonataSkipCutscene performs the same steps the game does on
// that A press, so it ends up exactly where the player would: the script's own
// skip handler runs and lands the party at the post-cutscene state. A cutscene
// the game does not let the player skip cannot be skipped here either, since
// there is no handler to run.
//
// Events. Two edges are published on the shared mod registry bus
// (rex::system::ModRegistry, reached via runtime->mod_registry()):
//
//     ETERNALSONATA_CUTSCENE_EVENT_STARTED  "eternalsonata.cutscene.started"
//     ETERNALSONATA_CUTSCENE_EVENT_ENDED    "eternalsonata.cutscene.ended"
//
// Both carry an EternalSonataCutsceneEvent in `bytes`, valid only during the
// callback. They run on the guest main thread, so a subscriber must be
// thread-safe and must not touch ImGui. Calling EternalSonataSkipCutscene from
// a `started` handler is fine and is how an auto-skip mod is written.
//
// Camera. The script animates its own camera object for the whole scene.
// EternalSonataSetCutsceneCameraControl(1) parks that camera at the transform
// last given to EternalSonataSetCutsceneCamera (initially wherever it was)
// every frame, over the script's motion. Control ends with the cutscene, or
// on (0). The transform type is the overworld API's EternalSonataFieldCamera
// (position and euler rotation in radians), so copy that header too.

#pragma once

#include <stdint.h>

#include "eternalsonata_overworld_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETERNALSONATA_CUTSCENE_ABI_VERSION 1u

enum {
  ETERNALSONATA_CUTSCENE_OK = 0,
  ETERNALSONATA_CUTSCENE_QUEUED = 1,
  ETERNALSONATA_CUTSCENE_ERR_UNAVAILABLE = -1,
  ETERNALSONATA_CUTSCENE_ERR_INVALID_ARGUMENT = -2,
  ETERNALSONATA_CUTSCENE_ERR_NOT_ACTIVE = -3,
  ETERNALSONATA_CUTSCENE_ERR_NOT_SKIPPABLE = -4,
  ETERNALSONATA_CUTSCENE_ERR_ALREADY_SKIPPING = -5
};

#define ETERNALSONATA_CUTSCENE_EVENT_STARTED "eternalsonata.cutscene.started"
#define ETERNALSONATA_CUTSCENE_EVENT_ENDED "eternalsonata.cutscene.ended"

// Skippability, as the game's scripts declare it. NONE means Start+A does
// nothing. PAUSE is the usual case: pause, then A skips. ANY_BUTTON is the
// mode a few scenes use where A/B/Start skips directly, without the pause.
enum {
  ETERNALSONATA_CUTSCENE_SKIP_NONE = 0,
  ETERNALSONATA_CUTSCENE_SKIP_PAUSE = 1,
  ETERNALSONATA_CUTSCENE_SKIP_ANY_BUTTON = 2
};

typedef struct EternalSonataCutsceneInfo {
  // 1 while a cutscene is running.
  int32_t active;
  // One of ETERNALSONATA_CUTSCENE_SKIP_*.
  int32_t skip_mode;
  // 1 once a skip has been requested (by the player or by this API) and the
  // script's skip handler is still running.
  int32_t skipping;
  // 1 while the game is paused. Skipping works either way.
  int32_t paused;
  // The event's guest script handle. Distinguishes consecutive cutscenes;
  // otherwise opaque.
  uint32_t handle;
  // Id of the event file the game last loaded, lowercase and without ".e"
  // (e.g. "e1010"). Best effort: the loader does not tag which event a map
  // script starts, so this is the most recent E%04d.e load.
  char script_id[32];
  // Canonical id of the field area the cutscene plays in, e.g. "bel01".
  char area_id[32];
  // 1 while a mod holds the camera through EternalSonataSetCutsceneCameraControl.
  int32_t camera_controlled;
} EternalSonataCutsceneInfo;

// Payload of both events. `skipped` is meaningful on `ended` only.
typedef struct EternalSonataCutsceneEvent {
  uint32_t handle;
  int32_t skip_mode;
  int32_t skipped;
  int32_t reserved;
  char script_id[32];
  char area_id[32];
} EternalSonataCutsceneEvent;

typedef uint32_t (*EternalSonataCutsceneAbiVersionFn)(void);

// 1 while a cutscene is running. Plain guest-memory read, safe from any thread.
typedef int (*EternalSonataIsCutsceneActiveFn)(void);

// Fills `out`. Returns OK even when no cutscene runs (active is then 0).
typedef int (*EternalSonataGetCutsceneInfoFn)(EternalSonataCutsceneInfo* out);

// Skips the running cutscene on the next guest main thread frame, the way the
// game's own pause menu does it. Returns QUEUED when accepted, NOT_ACTIVE with
// no cutscene, NOT_SKIPPABLE when the script did not declare a skip handler,
// and ALREADY_SKIPPING while one is in progress.
typedef int (*EternalSonataSkipCutsceneFn)(void);

// Reads the active cutscene camera's transform. OK, or NOT_ACTIVE with no
// cutscene, or UNAVAILABLE while the camera is not readable.
typedef int (*EternalSonataGetCutsceneCameraFn)(EternalSonataFieldCamera* out);

// Holds the cutscene camera at the transform last set (initially wherever it
// was when control began). QUEUED when accepted; NOT_ACTIVE with no cutscene.
// Released automatically when the cutscene ends.
typedef int (*EternalSonataSetCutsceneCameraControlFn)(int enabled);

// Moves the controlled camera. QUEUED, or UNAVAILABLE without control.
typedef int (*EternalSonataSetCutsceneCameraFn)(const EternalSonataFieldCamera* camera);

#ifdef __cplusplus
}
#endif
