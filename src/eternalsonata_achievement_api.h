// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for achievements: the title's own twenty two, and achievements a
// mod adds of its own. Custom ones show up everywhere the stock ones do: the
// unlock toast, the SDK's achievement overlay, and the status menu's
// Achievements screen, whose third tab exists only when a mod registered at
// least one.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Options API is used (see eternalsonata_options_api.h):
//
//     auto reg = reinterpret_cast<EternalSonataRegisterCustomAchievementFn>(
//         GetProcAddress(GetModuleHandle(nullptr),
//                        "EternalSonataRegisterCustomAchievement"));
//     EternalSonataCustomAchievementData data = {};
//     data.name = "Perfect Pitch";
//     data.description = "Finish a duel without taking damage.";
//     data.gamerscore = 50;
//     const int id = reg ? reg(&data) : -1;
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataAchievementAbiVersion() before using anything
// added after version 1.
//
// Ids are assigned by the host, like custom items: registration returns one out
// of a range reserved for mods, so two mods cannot collide and a mod does not
// have to know what the title already uses. Keep the returned id for the rest
// of the session; it is what everything else here is keyed by, and it is not
// stable across runs (it depends on load order), so never write it to a file.
//
// Unlocks persist. The SDK keeps unlock state in the profile's achievement save
// alongside the title's own, keyed by id, so a mod that registers a different
// number of achievements than it did last run will find the ids shifted under
// it. That is the trade for not having to pick ids: if a mod needs unlocks to
// survive a change to its own catalogue, it should record that in its own
// storage and re-unlock on startup.
//
// Localisation, two routes. A mod hands its own translations in with the
// registration, as an array keyed by language id, and the host keeps the one the
// game booted in. Anyone else translates it through the ordinary
// [language.strings] table, addressed by the registering mod's id and the index
// of the achievement within it (`mod_id` below), which is also how a mod-added
// language reaches it: the ids there are whatever the language mod declared, and
// the boot language's id is what both routes match on.
//
// Threading. Everything here is host state under a lock and is safe from any
// thread, including the ImGui draw thread; nothing runs guest code, so there is
// no queued-write path. Registration is cheapest from `OnModuleLaunched()`, but
// it is legal at any time: the Achievements screen rebuilds its rows every time
// it is opened.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_ACHIEVEMENT_ABI_VERSION 1u

// How many custom achievements can be registered at once, across all mods.
// That is also the Achievements screen's third tab capacity.
#define ETERNALSONATA_CUSTOM_ACHIEVEMENT_CAPACITY 32

// The id range custom achievements are allocated from. Exposed so a mod can
// tell one of its own apart from a stock one without calling anything.
#define ETERNALSONATA_CUSTOM_ACHIEVEMENT_ID_MIN 0x10000
#define ETERNALSONATA_CUSTOM_ACHIEVEMENT_ID_MAX \
  (ETERNALSONATA_CUSTOM_ACHIEVEMENT_ID_MIN + ETERNALSONATA_CUSTOM_ACHIEVEMENT_CAPACITY - 1)

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_ACHIEVEMENT_OK = 0,

  // No runtime is live yet, so there is no catalogue to touch.
  ETERNALSONATA_ACHIEVEMENT_ERR_UNAVAILABLE = -1,
  // No achievement with that id.
  ETERNALSONATA_ACHIEVEMENT_ERR_NO_SUCH_ACHIEVEMENT = -2,
  // Every custom id is taken.
  ETERNALSONATA_ACHIEVEMENT_ERR_FULL = -3,
  // The id is a real achievement, but not one a mod registered, and the call
  // only applies to custom ones.
  ETERNALSONATA_ACHIEVEMENT_ERR_NOT_CUSTOM = -4,
  ETERNALSONATA_ACHIEVEMENT_ERR_INVALID_ARGUMENT = -10
};

// Text limits in EternalSonataAchievement below. Longer strings are accepted at
// registration and truncated only when read back through this struct.
#define ETERNALSONATA_ACHIEVEMENT_NAME_MAX 128
#define ETERNALSONATA_ACHIEVEMENT_DESCRIPTION_MAX 256

// Language ids, the console's own (XLanguage). A mod that added a language of
// its own uses whatever id it declared; EternalSonataGetAchievementLanguage
// answers the one the process booted in, which is the only one that matters
// since it cannot change without a restart.
enum {
  ETERNALSONATA_LANGUAGE_ENGLISH = 1,
  ETERNALSONATA_LANGUAGE_JAPANESE = 2,
  ETERNALSONATA_LANGUAGE_GERMAN = 3,
  ETERNALSONATA_LANGUAGE_FRENCH = 4,
  ETERNALSONATA_LANGUAGE_SPANISH = 5,
  ETERNALSONATA_LANGUAGE_ITALIAN = 6
};

// One language's strings for an achievement. Anything left null falls back to
// the matching field of EternalSonataCustomAchievementData.
typedef struct EternalSonataAchievementTranslation {
  int32_t language;  // an ETERNALSONATA_LANGUAGE_* id
  const char* name;
  const char* description;
  const char* locked_description;
} EternalSonataAchievementTranslation;

// What a mod provides when registering. The host assigns the id.
typedef struct EternalSonataCustomAchievementData {
  // The registering mod's id: its folder name under mods/, the same string
  // mods.toml lists. Optional, but without it nobody else can translate this
  // achievement, because the id it gets is handed out at runtime and moves with
  // load order.
  //
  // The host joins it to `key` below, so an achievement "shutterbug" names
  // `five_photos` is addressed from a translation mod's [language.strings] as
  //     achv_name_shutterbug_five_photos,
  //     achv_desc_shutterbug_five_photos,
  //     achv_desc_locked_shutterbug_five_photos
  // for the language it is translating into. Those are looked up when the
  // achievement is registered and win over `translations` below, so a
  // translation mod can correct or add a language the author never shipped.
  const char* mod_id;

  // A short stable name for this achievement within the mod, in the same spirit
  // as `mod_id`: lower case, no spaces. Optional; leave it null and the host
  // numbers the mod's achievements in registration order instead, giving
  // `shutterbug_0`, `shutterbug_1`. A name is worth giving, since the numbering
  // moves if the mod ever registers conditionally or reorders its calls, and a
  // translation written against the old numbering then lands on the wrong
  // achievement.
  const char* key;

  // Shown as the achievement's title. Required; copies are made, caller retains
  // ownership of every string here.
  const char* name;
  // Shown once it is unlocked, and while it is locked too unless `secret`.
  const char* description;
  // Optional: what to show while it is locked. Defaults to `description`.
  const char* locked_description;

  // Optional path to a PNG for the toast and the overlay. Absolute, or relative
  // to the process working directory. The status menu draws its own icon and
  // ignores this.
  const char* icon_path;

  // What the unlock is worth. The status menu draws it in the row's left
  // column, where it has room for four characters ("100G").
  int32_t gamerscore;

  // 1 to hide the description until it is unlocked, the way the title's own
  // secret achievements behave. 0 to always show it.
  int32_t secret;

  // Optional translations. The host keeps the entry whose `language` matches
  // the language the game booted in and drops the rest, so the three fields
  // above are what a language nobody translated falls back to. Order does not
  // matter; the first entry for a language wins.
  const EternalSonataAchievementTranslation* translations;
  int32_t translation_count;
} EternalSonataCustomAchievementData;

// One achievement, read back.
typedef struct EternalSonataAchievement {
  int32_t id;
  int32_t is_custom;  // 1 when a mod registered it
  int32_t unlocked;
  int32_t gamerscore;
  int32_t secret;  // 1 when the description is hidden until unlocked

  char name[ETERNALSONATA_ACHIEVEMENT_NAME_MAX];
  char description[ETERNALSONATA_ACHIEVEMENT_DESCRIPTION_MAX];

  // Windows FILETIME of the unlock, 0 while it is locked.
  uint64_t unlocked_at;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataAchievement;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataAchievementAbiVersionFn)(void);

// True once the catalogue is readable, i.e. once the runtime is up.
typedef int (*EternalSonataIsAchievementSystemAvailableFn)(void);

// The language the game booted in, as an ETERNALSONATA_LANGUAGE_* id, or 0 if
// it is not known yet. Mods that would rather pick their own strings than hand
// over a translation table can switch on this.
typedef int (*EternalSonataGetAchievementLanguageFn)(void);

// ---------------------------------------------------------------------------
// Registering
// ---------------------------------------------------------------------------

// Registers a new achievement and returns the id it was given (> 0), or a
// negative error. `data->name` must be non-empty.
typedef int (*EternalSonataRegisterCustomAchievementFn)(
    const EternalSonataCustomAchievementData* data);

// Takes a custom achievement back out of the catalogue and frees its id. Its
// unlock state is left in the save, so re-registering into the same id restores
// it. Returns ETERNALSONATA_ACHIEVEMENT_OK or a negative error.
typedef int (*EternalSonataUnregisterCustomAchievementFn)(int id);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// How many achievements the catalogue holds, custom ones included, or a
// negative error.
typedef int (*EternalSonataGetAchievementCountFn)(void);

// Fills `out` with the achievement `id`. Returns ETERNALSONATA_ACHIEVEMENT_OK
// or a negative error.
typedef int (*EternalSonataGetAchievementFn)(int id, EternalSonataAchievement* out);

// Fills `out` with up to `max` achievements in ascending id order, so the
// custom ones come last, and returns how many were written, or a negative
// error. Pass max = 0 to just count.
typedef int (*EternalSonataGetAchievementsFn)(EternalSonataAchievement* out, int max);

// 1, 0, or a negative error.
typedef int (*EternalSonataIsAchievementUnlockedFn)(int id);

// ---------------------------------------------------------------------------
// Unlocking
// ---------------------------------------------------------------------------

// Unlocks `id`, saves the unlock state and, unless `show_toast` is 0, shows the
// usual unlock notification. Works on the title's own achievements as well as
// custom ones. Returns 1 on a first-time unlock, 0 if it was already unlocked,
// or a negative error.
typedef int (*EternalSonataUnlockAchievementFn)(int id, int show_toast);

#ifdef __cplusplus
}  // extern "C"
#endif
