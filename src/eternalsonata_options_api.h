// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI letting mods add their own rows to the game's native Options
// screen (the one reachable from both the main menu and the in-game status
// screen - see eternalsonata_options.cpp, they are the same screen).
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// EternalSonataIsBattleActive is used (see room_presence.cpp):
//
//     auto reg = reinterpret_cast<EternalSonataRegisterOptionRowFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataRegisterOptionRow"));
//     if (reg) { ... }
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataOptionsAbiVersion() before using anything
// added after version 1.
//
// Call from the mod's OnModuleLaunched(). Registration is only guaranteed to
// take effect if it happens before the player first opens Options; rows
// registered later appear the next time the screen is built.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_OPTIONS_ABI_VERSION 2u

// Options is two pages, paged between with LB/RB. Page 1 is the Options screen
// proper: the game's own Subtitles and Voice rows plus this project's Text
// row. Page 2 is the button-configuration screen, which the recompilation uses
// for graphics settings (Resolution, Frame Rate). A row registered by a mod
// starts on page 2, because page 1 is reserved for the game's own settings;
// move it with EternalSonataSetOptionRowPage.
enum {
  ETERNALSONATA_PAGE_GAME = 0,
  ETERNALSONATA_PAGE_GRAPHICS = 1
};

// Language slots, matching the five Options display lists the game ships
// (see kOptionsListByLang in eternalsonata_options.cpp).
enum {
  ETERNALSONATA_LANG_EN = 0,
  ETERNALSONATA_LANG_DE = 1,
  ETERNALSONATA_LANG_FR = 2,
  ETERNALSONATA_LANG_ES = 3,
  ETERNALSONATA_LANG_IT = 4,
  ETERNALSONATA_LANG_COUNT = 5
};

// Returns the currently selected value, as a 0-based index into the `values`
// array the row was registered with. Called when the screen is built and
// whenever the highlight needs repositioning, so it must be cheap and must not
// block. `user` is the pointer passed to EternalSonataRegisterOptionRow.
typedef int (*EternalSonataOptionGetFn)(void* user);

// Applies a newly chosen value. Called on the guest thread that is running the
// menu, from inside the cursor-update hook - do not block, and do not re-enter
// the options API from here.
typedef void (*EternalSonataOptionSetFn)(int index, void* user);

// Registers one row. Returns the new row's index (>= 0), or -1 if the row was
// rejected: null/empty label, value_count outside 1..ETERNALSONATA_MAX_ROW_VALUES,
// null callbacks, or the screen is already full (see below).
//
// `label` and the strings in `values` are copied; the caller keeps ownership.
// Text is single-byte (CP1252/Latin-1), NOT UTF-8 - the game's font draws one
// glyph per byte, so a UTF-8 accented character renders as two garbled glyphs.
// Write "\xE9" rather than "é".
//
// `label` is used for every language. Call EternalSonataSetOptionRowLabel for
// each language you actually translate; untranslated languages keep `label`.
// Values are not localised (FPS figures, resolution names and the like are
// conventionally left untranslated), matching how the built-in rows behave.
//
// Keep values short. They are drawn side by side on one line, sharing about
// 630px. Columns are spaced evenly when that fits (200px each for two or three
// values, ~157px for four, ~126px for five) and follow the values' own lengths
// when it does not. Either way the budget is finite - that is why the built-in
// Text row draws "EN" / "DE" / ... rather than language names. A label has
// about 13 characters before it reaches the value column.
//
// Row capacity is per page, and it is the game's, not ours: a selectable
// group's item array is pre-allocated with 10 slots, of which page 1's stock
// Subtitles and Voice rows take 2 and page 2's three button rows take 3. That
// leaves ETERNALSONATA_MAX_OPTION_ROWS rows on page 1 and one fewer on page 2,
// built-in rows included - registration past that is rejected rather than
// silently corrupting the menu.
#define ETERNALSONATA_MAX_OPTION_ROWS 8
#define ETERNALSONATA_MAX_ROW_VALUES 9

typedef int (*EternalSonataRegisterOptionRowFn)(const char* label,
                                               const char* const* values,
                                               int value_count,
                                               EternalSonataOptionGetFn get,
                                               EternalSonataOptionSetFn set,
                                               void* user);

// Overrides `row`'s label for one language. Returns false for an unknown row
// index, an out-of-range language, or a null label. Safe to call after
// registration and after the screen has been built - labels are rewritten on
// every Options entry, so a change takes effect the next time it opens.
typedef int (*EternalSonataSetOptionRowLabelFn)(int row, int language,
                                                const char* label);

// Moves `row` to another page (ETERNALSONATA_PAGE_*). Returns false for an
// unknown row, an unknown page, or a destination page that is already full.
// Safe to call any time; the move shows up the next time each page is built.
// Added in ABI version 2 - null-check the symbol.
typedef int (*EternalSonataSetOptionRowPageFn)(int row, int page);

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataOptionsAbiVersionFn)(void);

#ifdef __cplusplus
}  // extern "C"
#endif
