#pragma once

// Which release a dump is, from the SHA-256 of one file in it: default.xex on
// the Xbox 360, PS3_GAME/USRDIR/EBOOT.BIN on the PS3. Hashing a whole image
// would read 7.8 GB.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace eternalsonata {

struct Release {
  const char* name;
  const char* file;
  const char* file_sha256;
  bool supported;
};

// The release whose identifying file has this hash, or null.
const Release* FindRelease(std::string_view file_sha256);

// Lowercase hex.
std::string Sha256Hex(std::span<const uint8_t> data);

}  // namespace eternalsonata
