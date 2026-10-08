#pragma once

// The install step after extraction: converts a USA or JP copy's containers
// into PAL's layout, which the recompiled code indexes by position, and adds
// the files only PAL ships. Runs in place on an extracted Xbox 360 directory,
// so a folder extracted by an older build or by hand needs only this step.

#include <filesystem>
#include <string>

#include "disc_image.h"

namespace eternalsonata {

// Whether `dir` was patched by this build's bundle.
bool IsReleasePatched(const std::filesystem::path& dir);

// Patches `dir` unless IsReleasePatched. Replaced files are kept with an
// ".orig" suffix and every run starts from those, so a newer bundle repatches
// cleanly. Returns why it failed, for the player, or empty.
std::string PatchRelease(const std::filesystem::path& dir, const ExtractProgress& progress);

}  // namespace eternalsonata
