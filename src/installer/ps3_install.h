#pragma once

// The PS3 release's install steps: unpacking USRDIR/archives/*.files, then
// converting the unpacked tree in place, as release_patch.h does for a USA or
// JP copy. Each converted file replaces the shipped one as it is
// written, so the tree never needs more room than the unpacked files. scripts/unpack_ps3.py and
// scripts/ps3_convert.py are the reference.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "disc_image.h"

namespace eternalsonata {

// Whether `dir` holds the PS3 release's files, converted or not.
bool IsPs3Directory(const std::filesystem::path& dir);

// The disc's region, from its title id.
enum class Ps3Region { kPal, kUsa, kJapan };

// Region of the disc `dir` was unpacked from; PAL when unknown, which is
// every tree unpacked before the id was recorded.
Ps3Region ReadPs3Region(const std::filesystem::path& dir);

// Whether `dir` was converted by this build.
bool IsPs3Converted(const std::filesystem::path& dir);

// Converts `dir` in place. Returns why it failed, for the player, or empty.
std::string ConvertPs3(const std::filesystem::path& dir, const ExtractProgress& progress);

// TITLE_ID from a disc's PARAM.SFO, or empty.
std::string ReadTitleId(const std::filesystem::path& sfo);
std::string TitleIdFromSfo(const std::vector<uint8_t>& sfo);

// The archives folder of a PS3 disc folder, given the disc root, PS3_GAME,
// USRDIR or archives itself; empty when there is none.
std::filesystem::path FindPs3Archives(const std::filesystem::path& picked);

// Total size of the *.files in `archives`, which is what UnpackPs3 writes.
uint64_t Ps3ArchiveBytes(const std::filesystem::path& archives);

// Unpacks every *.files in `archives` into `out_dir`, through a sibling
// directory like ExtractDiscImage. PS3 paths are all lowercase; every host
// lookup folds case.
std::string UnpackPs3(const std::filesystem::path& archives, const std::filesystem::path& out_dir,
                      const ExtractProgress& progress);

// UnpackPs3 for the *.files inside an ISO 9660 `image`, read in place.
std::string UnpackPs3Image(const std::string& image, const std::filesystem::path& out_dir,
                           const ExtractProgress& progress);

}  // namespace eternalsonata
