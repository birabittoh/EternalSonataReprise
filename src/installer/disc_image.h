#pragma once

// Extraction of an Xbox 360 disc image (XDVDFS) into a directory.

#include <cstdint>
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

// Bytes extraction would write, read from the directory alone; 0 when the
// image cannot be read.
uint64_t MeasureDiscImage(const std::string& image);
uint64_t MeasureIsoFolder(const std::string& image, std::string_view folder);

// Whether `image` is an ISO 9660 image, as a decrypted PS3 dump is.
bool IsIsoImage(const std::string& image);

// Reads `path` (slash separated, case insensitive) from an ISO 9660 `image`.
std::string ReadIsoImageFile(const std::string& image, std::string_view path,
                             std::vector<uint8_t>& out);

// A file read in place by offset, so a large one is never copied out.
struct ImageFile {
  std::string name;
  uint64_t size;
  std::function<bool(uint64_t offset, void* dst, uint64_t length)> read;
};

// Calls `use` with the files of `folder` in an ISO 9660 `image` whose name ends
// in `suffix`; they are readable only during the call. Returns what `use`
// returns, or why the image could not be read.
std::string WithIsoFiles(const std::string& image, std::string_view folder, std::string_view suffix,
                         const std::function<std::string(const std::vector<ImageFile>&)>& use);

// Copies the folder `folder` of an ISO 9660 `image` into `out_dir`, which is
// replaced. Returns why it failed, for the player, or empty.
std::string ExtractIsoFolder(const std::string& image, std::string_view folder,
                             const std::filesystem::path& out_dir, const ExtractProgress& progress);

}  // namespace eternalsonata
