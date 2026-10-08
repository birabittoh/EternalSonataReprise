// eternalsonata - ReXGlue Recompiled Project
//
// The guest image (default.xex) is linked into the executable, so a game
// directory only needs the data files. It is served from a VFS device of its
// own and the module is loaded from there.

#pragma once

#include <string>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace eternalsonata {

// Mounts the embedded image and returns the guest path to load it from.
std::string MountGuestImage(rex::filesystem::VirtualFileSystem* file_system);

}  // namespace eternalsonata
