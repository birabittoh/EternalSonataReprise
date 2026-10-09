#pragma once

// The PS3 release's install steps: unpacking USRDIR/archives/*.files, then
// converting the unpacked tree in place, as release_patch.h does for a USA or
// JP copy. Converted files replace the shipped ones, which are kept with an
// ".orig" suffix, and every run starts from those. scripts/unpack_ps3.py and
// scripts/ps3_convert.py are the reference.

#include <filesystem>
#include <string>

#include "disc_image.h"

namespace eternalsonata {

// Whether `dir` holds the PS3 release's files, converted or not.
bool IsPs3Directory(const std::filesystem::path& dir);

// Whether `dir` was converted by this build.
bool IsPs3Converted(const std::filesystem::path& dir);

// Converts `dir` in place. Returns why it failed, for the player, or empty.
std::string ConvertPs3(const std::filesystem::path& dir, const ExtractProgress& progress);

// The archives folder of a PS3 disc folder, given the disc root, PS3_GAME,
// USRDIR or archives itself; empty when there is none.
std::filesystem::path FindPs3Archives(const std::filesystem::path& picked);

// Unpacks every *.files in `archives` into `out_dir`, through a sibling
// directory like ExtractDiscImage. PS3 paths are all lowercase; every host
// lookup folds case.
std::string UnpackPs3(const std::filesystem::path& archives, const std::filesystem::path& out_dir,
                      const ExtractProgress& progress);

}  // namespace eternalsonata
