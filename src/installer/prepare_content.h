#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace eternalsonata {

// Runs before the SDK's desktop entry point, without SDL, a GPU or a window.
// Returns -1 for a normal game launch, 0 on success, 2 for invalid arguments,
// and 1 for an installation failure. Arguments and output are UTF-8.
int PrepareContentCommand(int argc, char** argv, std::ostream& out, std::ostream& err);

// Owns only destination (and a newly created sibling transaction directory).
// The source is always read-only, including already extracted game folders.
std::string PrepareContent(const std::filesystem::path& source,
                           const std::filesystem::path& destination, std::ostream& out);

}  // namespace eternalsonata
