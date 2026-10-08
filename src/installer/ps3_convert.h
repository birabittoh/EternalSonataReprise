#pragma once

// Conversion of the PS3 release's files into what the 360 executable loads.
// A port of scripts/ps3_convert.py and scripts/ps3_audio.py, which stay the
// reference: every non audio output must equal theirs byte for byte.
// docs/ps3-assets.md has the formats.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eternalsonata::ps3 {

using Bytes = std::vector<uint8_t>;

struct Report {
  std::map<std::string, size_t> counts;
  std::vector<std::string> warnings;

  void Warn(std::string message) { warnings.push_back(std::move(message)); }
};

// A bank inside a CSL directory: the directory's offset, the bank's position
// in it and every bank of the directory.
struct BankMember {
  size_t directory = 0;
  size_t position = 0;
  const std::vector<Bytes>* group = nullptr;
};

// Converts one sound bank embedded in a container: its ordinal in the
// container, its bytes and its directory, if any.
using AudioConverter =
    std::function<Bytes(size_t index, const Bytes& bank, const BankMember* member)>;

using Token = std::array<uint8_t, 8>;
// Receives a sidecar WAV for pcm/<token as hex>.wav.
using PcmWriter = std::function<void(const Token& token, const Bytes& wav)>;

struct Output {
  std::string path;
  Bytes data;
};

// Mirrors convert_file: the 360 path and bytes, or nothing to keep the 360
// file. `audio` converts the banks inside containers, null to copy them.
std::optional<Output> ConvertFile(const std::string& rel, const Bytes& d, Report& report,
                                  const AudioConverter* audio);

extern const char* const kBattleKeep;
// Converted PS3 BattleKeep with the 360's dropped effects at its end.
Bytes AppendDroppedBattleKeep(const Bytes& ps3, const Bytes& x360, Report& report);

// Container scanning shared with the bank pairing.
bool IsCsf(const Bytes& d, size_t o, size_t end);
bool IsCsl(const Bytes& d, size_t o, size_t end);
std::vector<size_t> CslBanks(const Bytes& d, size_t o);

// Audio (ps3_audio.cpp).
Token MakeToken(const std::string& key);
std::string TokenHex(const Token& token);
// `x360` is the decoded 360 bank of the same name, or null.
Bytes ConvertCsf(const std::string& rel, const Bytes& ps3, const Bytes* x360,
                 const PcmWriter& write_pcm, Report& report);
const Bytes* BestTwin(const Bytes& bank, const std::vector<Bytes>& candidates, size_t index);
const std::vector<Bytes>* BestGroup(const std::vector<Bytes>& group,
                                    const std::vector<std::vector<Bytes>>& candidates);

// One 360 .cxs that a new track's tag is written into, by file name.
using CxsDonors = std::vector<std::pair<std::string, Bytes>>;
// Music: what to add, empty when the 360 track serves. `has_base` tells
// whether the 360 directory has a path (lowercase).
std::vector<Output> ConvertCps(const std::string& rel, const Bytes& d,
                               const std::function<bool(const std::string&)>& has_base,
                               const CxsDonors& donors, const PcmWriter& write_pcm,
                               Report& report);

}  // namespace eternalsonata::ps3
