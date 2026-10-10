// eternalsonata - ReXGlue Recompiled Project
//
// Granular asset replacement: collects text patches from every enabled mod
// (declaratively from mods/<name>/assets/, or at runtime through
// src/eternalsonata_asset_api.h), splices them into one patched image per
// container, and serves the result to the guest.
//
// How it is served (milestone 1): the patched containers and a matching
// index.vmtoc are materialised into <user_data>/cache/patched_assets/<hash>/,
// which is then pushed as the highest-priority overlay root of the game data
// partition, ahead of the mods' own game/ folders. That composes with the SDK's
// existing whole-file overlay for free and leaves the patched files on disk
// where they can be diffed. The cache key covers the mod list and every patch's
// bytes, so a rebuild only happens when something actually changed.

#pragma once

#include <rex/runtime.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eternalsonata {

// Publishes the `[[language]]` blocks in every enabled mod's assets.toml into
// the settings language registry (see settings.h's RegisterModLanguage), so a
// pure-translation mod needs no C++ at all: a toml block plus text files under
// assets/<container>/text/<CODE>/ is a whole new language.
//
// Separate from BindAssetSystem, and called before it, because the registry has
// to be complete before the boot language is latched and before the guest is
// pointed at a donor BTX slot. Mods that publish through the mod-registry
// events instead have already run by this point (OnCreateDialogs), so the
// first-wins rule spans both routes.
void ScanModLanguages(rex::Runtime* runtime);

// The same from mods.toml, for the start screen, which opens before any
// runtime exists. ScanModLanguages then skips the mods this already read.
void ScanModLanguagesEarly();

// The `font` files that enabled mods name in their `[[language]]` blocks, read
// straight from mods.toml because the overlay atlas is built before any
// runtime exists. They are merged into the overlay font behind the stock ones,
// so they only supply the glyphs those lack. The atlas is baked once, so
// `glyphs` is every character the mod's assets.toml contains.
struct ModLanguageFont {
  std::filesystem::path path;
  std::string glyphs;
};
std::vector<ModLanguageFont> ModLanguageFonts();

// Collects patches, builds the cache, and remounts the game partition with it.
// Must run before the guest starts (OnPostSetup is the right place): the
// remount replaces a live device.
void BindAssetSystem(rex::Runtime* runtime);

// Applies the text patches addressed to the `default.xex` container, which are
// the only ones that do not go through the cache directory.
//
// The game's own UI chrome ("Player Controls", "Next", "ON"/"OFF" and ~2850
// other strings per language) is not in any .e file: it lives in 23 ordinary
// BTX blobs baked into the executable image. They parse with the same code the
// container blobs use, so a mod addresses them exactly like any other text
// patch. But they sit at fixed addresses with unrelated data on both sides, so
// the rebuild is preserve-size only and is written straight into guest memory
// over read-only pages.
//
// Call after the image is loaded and before the guest runs (OnPreLaunchModule).
void ApplyXexTextPatches(rex::Runtime* runtime);

// Whether "title_jpn.bmd", the Japanese title screen, was installed.
bool JapaneseTitleServed();

// Whether the `fourcc` BTX block ("ITA " and so on) has text to show: the
// release in game_data_root ships it, or a mod patches .e text into it. The USA
// release carries all seven blocks but leaves the European ones empty. When
// `count_mods` is false only the shipped text counts. Valid once
// BindAssetSystem has run; true before that, and when the probe cannot be read.
bool BtxLanguageAvailable(const char* fourcc, bool count_mods = true);

}  // namespace eternalsonata
