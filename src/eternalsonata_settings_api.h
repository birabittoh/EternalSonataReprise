// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's own Options settings: Battle Camera, Attack
// Button, the three volumes, Subtitles, Voice language, Audio Output and the
// three player-to-controller assignments.
//
// This is deliberately NOT the row-registration API in
// eternalsonata_options_api.h. That one lets a mod add a row of its own; this
// one reads and writes the settings the game already has, so a mod can change
// Voice or mute the music without owning a menu row. The two are independent
// and a mod may use either or both.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable:
//
//     auto get = reinterpret_cast<EternalSonataGetSettingFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataGetSetting"));
//     if (get) { ... }
//
// Always null-check, and check EternalSonataSettingsAbiVersion() before using
// anything added after version 1.
//
// Threading: reads and most writes are plain guest-memory accesses and can be
// made from any thread. The volumes have to go through the game's own mixer
// routine, which is guest code, so setting one from a thread other than the
// guest main thread defers it to that thread's next frame and answers
// ETERNALSONATA_SETTING_QUEUED.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_SETTINGS_ABI_VERSION 1u

// The settings, and what their values mean. Every one of them is persisted in
// the save, so a load replaces whatever a mod wrote.
enum {
  // 0 = OFF, 1 = ON. Default 1.
  ETERNALSONATA_SETTING_BATTLE_CAMERA = 0,
  // 0 = A, 1 = B. Default 0.
  ETERNALSONATA_SETTING_ATTACK_BUTTON = 1,
  // 0..100. Default 100. Applied through the game's mixer, see above.
  ETERNALSONATA_SETTING_VOLUME_MUSIC = 2,
  ETERNALSONATA_SETTING_VOLUME_SFX = 3,
  ETERNALSONATA_SETTING_VOLUME_VOICE = 4,
  // 0 = OFF, 1 = ON. Default 1.
  ETERNALSONATA_SETTING_SUBTITLES = 5,
  // 0 = Japanese, 1 = English. Default 1.
  ETERNALSONATA_SETTING_VOICE_LANGUAGE = 6,
  // 0 = Stereo, 1 = Mono, 2 = 5.1ch Surround. Default 0.
  //
  // The setting is real and persisted, and the three names above are its own
  // strings in the executable's text blob, but the retail Options screen draws
  // no row for it: the 360 configures audio output at the system level. It is
  // exposed because it round-trips through the save like the rest, not because
  // anything is known to read it.
  ETERNALSONATA_SETTING_AUDIO_OUTPUT = 7,
  // 0..3, the controller port that drives this player. The player-controls
  // screen only offers ports with a pad connected, and the game reassigns a
  // player whose port goes away, so a value written here does not necessarily
  // survive the next visit to that screen.
  ETERNALSONATA_SETTING_CONTROLLER_P1 = 8,
  ETERNALSONATA_SETTING_CONTROLLER_P2 = 9,
  ETERNALSONATA_SETTING_CONTROLLER_P3 = 10,

  ETERNALSONATA_SETTING_COUNT = 11
};

enum {
  ETERNALSONATA_SETTING_OK = 0,
  // The write was handed to the guest main thread and will land on its next
  // frame. Only the volumes can answer this.
  ETERNALSONATA_SETTING_QUEUED = 1,
  // No runtime yet, or guest memory is not up.
  ETERNALSONATA_SETTING_ERR_UNAVAILABLE = -1,
  // Not one of ETERNALSONATA_SETTING_*.
  ETERNALSONATA_SETTING_ERR_INVALID_SETTING = -2,
  // Outside the range EternalSonataGetSettingRange reports.
  ETERNALSONATA_SETTING_ERR_INVALID_VALUE = -3
};

// Event published on the mod registry bus whenever a setting changes, whoever
// changed it: the player in the Options screen, a save being loaded, a new
// game resetting everything, or another mod. Payload u64 is the setting,
// payload f64 is its new value.
//
// This is how a mod that forces a setting keeps it forced. Four different
// routines reset the whole block to defaults (new game and both title-screen
// paths among them), so a value written once will not stay written.
#define ETERNALSONATA_SETTING_EVENT_CHANGED "eternalsonata.setting.changed"

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataSettingsAbiVersionFn)(void);

// Returns the setting's current value, or a negative error. Values are never
// negative, so the two never collide.
typedef int (*EternalSonataGetSettingFn)(int setting);

// Writes a setting. Returns ETERNALSONATA_SETTING_OK,
// ETERNALSONATA_SETTING_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataSetSettingFn)(int setting, int value);

// Fills `min` and `max` (both inclusive, either may be null) with the range
// `setting` accepts. Returns ETERNALSONATA_SETTING_OK or a negative error.
typedef int (*EternalSonataGetSettingRangeFn)(int setting, int* min, int* max);

#ifdef __cplusplus
}  // extern "C"
#endif
