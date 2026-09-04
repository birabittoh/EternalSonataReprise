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

namespace eternalsonata {

// Collects patches, builds the cache, and remounts the game partition with it.
// Must run before the guest starts (OnPostSetup is the right place): the
// remount replaces a live device.
void BindAssetSystem(rex::Runtime* runtime);

}  // namespace eternalsonata
