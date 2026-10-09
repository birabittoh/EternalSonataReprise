#pragma once

// Extraction of an Xbox 360 disc image (XDVDFS) into a directory.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace eternalsonata {

// `fraction` outside [0,1] means unknown. When empty, an SDL progress window
// shows it.
using ExtractProgress =
    std::function<void(const std::string& title, float fraction, const std::string& detail)>;

// Moves a directory out of the way instead of deleting what a player put
// there; an empty one is removed.
bool MoveAside(const std::filesystem::path& dir);

// Case insensitive, as disc file names are.
bool SameFileName(std::string_view a, std::string_view b);

// Extracts `image` (a path, or an Android content URI) into `out_dir`, which
// is replaced; a non empty one is moved aside, not deleted. Refuses an image
// whose root lacks `required_entry`. Goes through a sibling directory, so an
// interrupted run never leaves `out_dir` half written. Returns why it failed,
// for the player, or empty.
std::string ExtractDiscImage(const std::string& image, const std::filesystem::path& out_dir,
                             std::string_view required_entry, const ExtractProgress& progress);

// Reads the file `name` from the root of `image` into `out`. Returns why it
// could not, for the player, or empty.
std::string ReadDiscImageFile(const std::string& image, std::string_view name,
                              std::vector<uint8_t>& out);

}  // namespace eternalsonata
