#pragma once

// The install as three independent phases, each deciding for itself whether
// the directory needs it:
//   extraction  a dumped disc (Xbox 360 image, PS3 folder) into game files
//   conversion  PS3 game files into the Xbox 360 layout the executable loads
//   patching    a USA or JP Xbox 360 layout into the PAL one
// A folder extracted earlier, or by an older build, only goes through the
// phases it still needs.

#include <filesystem>
#include <functional>
#include <string>

#include "disc_image.h"

namespace eternalsonata {

enum class Phase { kExtract, kConvert, kPatch };
inline constexpr int kInstallPhaseCount = 3;

enum class PhaseState { kPending, kActive, kDone, kSkipped };

const char* PhaseName(Phase phase);

using PhaseReport = std::function<void(Phase, PhaseState)>;

struct InstallHooks {
  // Per work item; unknown fraction is outside [0,1].
  ExtractProgress progress;
  PhaseReport phase;
};

// Whether `dir` holds game files, converted or not.
bool IsGameDirectory(const std::filesystem::path& dir);

// Whether the later phases still have work on `dir`.
bool NeedsConversion(const std::filesystem::path& dir);
bool NeedsPatching(const std::filesystem::path& dir);

// Whether `picked` (a path or an Android content URI) is a game directory,
// or a file inside one, that needs no extraction.
bool IsExtractedSource(const std::string& picked);

struct SourceInfo {
  // The release, for the player; empty when not known.
  std::string release;
  // Why `picked` cannot be installed, or empty.
  std::string error;
  // The text it ships (release_id.h bits), 0 when not known.
  uint32_t languages = 0;
};

// Identifies `picked` (as Install takes it) without extracting anything.
SourceInfo IdentifySource(const std::string& picked);

// Runs the phases on `picked` (a disc image, a PS3 disc folder, or a game
// directory), extracting into `assets` when it is a disc. Returns why it
// failed, for the player, or empty with `dir` set to the ready game
// directory.
std::string Install(const std::string& picked, const std::filesystem::path& assets,
                    const InstallHooks& hooks, std::filesystem::path& dir);

// The phases 2 and 3 alone, for a directory already extracted.
std::string InstallDirectory(const std::filesystem::path& dir, const InstallHooks& hooks);

}  // namespace eternalsonata
