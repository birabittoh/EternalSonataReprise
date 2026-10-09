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

// Converts one sound bank embedded in a container, given its ordinal in the
// container and its bytes.
using AudioConverter = std::function<Bytes(size_t index, const Bytes& bank)>;

using Token = std::array<uint8_t, 8>;
// Receives a sidecar WAV for pcm/<token as hex>.wav.
using PcmWriter = std::function<void(const Token& token, const Bytes& wav)>;

struct Output {
  std::string path;
  Bytes data;
};

// Mirrors convert_file: the 360 path and bytes, or nothing to serve the PS3
// file as is. `audio` converts the banks inside containers, null to copy them.
std::optional<Output> ConvertFile(const std::string& rel, const Bytes& d, Report& report,
                                  const AudioConverter* audio);

extern const char* const kBattleKeep;
// Converted PS3 BattleKeep with null slots for the 360's dropped effects.
Bytes AppendEmptyBattleKeep(const Bytes& d, Report& report);

// Sound bank (CSF) and bank directory (CSL) detection.
bool IsCsf(const Bytes& d, size_t o, size_t end);
bool IsCsl(const Bytes& d, size_t o, size_t end);
std::vector<size_t> CslBanks(const Bytes& d, size_t o);

// Audio (ps3_audio.cpp).
Token MakeToken(const std::string& key);
std::string TokenHex(const Token& token);
Bytes ConvertCsf(const std::string& rel, const Bytes& ps3, const PcmWriter& write_pcm,
                 Report& report);
// Music: the files to add for one .cps.
std::vector<Output> ConvertCps(const std::string& rel, const Bytes& d, const PcmWriter& write_pcm,
                               Report& report);

// The encoded sidecars' WAV format tags: ATRAC3's registered one, and a
// private one for PS-ADPCM.
constexpr uint16_t kWaveAtrac3 = 0x0270;
constexpr uint16_t kWavePsxAdpcm = 0x5053;
// PS-ADPCM, channels interleaved per 16 byte frame of 28 samples, to
// interleaved PCM.
std::vector<int16_t> DecodePsxAdpcm(const uint8_t* data, size_t size, uint32_t channels);

}  // namespace eternalsonata::ps3
