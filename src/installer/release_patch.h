#pragma once

// The install step after extraction: rebuilds a USA or JP copy's BattleKeep
// in PAL's slot layout, which the recompiled code indexes by position, and
// adds the files only PAL ships from the release's own data. Everything else
// runs as shipped. Runs in place on an extracted Xbox 360 directory, so a
// folder extracted by an older build or by hand needs only this step.

#include <filesystem>
#include <string>

#include "disc_image.h"

namespace eternalsonata {

// Whether `dir` was patched by this build's recipe.
bool IsReleasePatched(const std::filesystem::path& dir);

// Patches `dir` unless IsReleasePatched. Replaced files are kept with an
// ".orig" suffix and every run starts from those, so a newer recipe repatches
// cleanly. Returns why it failed, for the player, or empty.
std::string PatchRelease(const std::filesystem::path& dir, const ExtractProgress& progress);

}  // namespace eternalsonata
