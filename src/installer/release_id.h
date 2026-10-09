#pragma once

// Which release a dump is, from the SHA-256 of one file in it: default.xex on
// the Xbox 360, PS3_GAME/USRDIR/EBOOT.BIN on the PS3. Hashing a whole image
// would read 7.8 GB.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace eternalsonata {

// Release::languages: the text a release ships, one bit per res/lang file.
enum : uint32_t {
  kTextJa = 1u << 0,
  kTextEn = 1u << 1,
  kTextFr = 1u << 2,
  kTextIt = 1u << 3,
  kTextDe = 1u << 4,
  kTextEs = 1u << 5,
  kTextAll = (1u << 6) - 1,
};

// The bit for a res/lang code, or 0.
uint32_t TextLanguageBit(std::string_view code);

struct Release {
  const char* name;
  const char* file;
  const char* file_sha256;
  bool supported;
  uint32_t languages;
};

// The release whose identifying file has this hash, or null.
const Release* FindRelease(std::string_view file_sha256);

// Lowercase hex.
std::string Sha256Hex(std::span<const uint8_t> data);

}  // namespace eternalsonata
