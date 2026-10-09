#include "install_pipeline.h"

#include "disc_image.h"
#include "ps3_install.h"
#include "release_patch.h"

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

constexpr const char* kTableOfContents = "index.vmtoc";

bool IsContentUri(const std::string& path) {
  return path.starts_with("content://");
}

void Report(const InstallHooks& hooks, Phase phase, PhaseState state) {
  if (hooks.phase)
    hooks.phase(phase, state);
}

// One phase: skipped when nothing is pending, else run and marked.
template <typename Needed, typename Run>
std::string RunPhase(const InstallHooks& hooks, Phase phase, Needed needed, Run run) {
  if (!needed()) {
    Report(hooks, phase, PhaseState::kSkipped);
    return {};
  }
  Report(hooks, phase, PhaseState::kActive);
  std::string error = run();
  if (error.empty())
    Report(hooks, phase, PhaseState::kDone);
  return error;
}

// Empty when the folder is something to install from, else why not.
std::string CheckFolder(const fs::path& dir) {
  if (IsGameDirectory(dir) || !FindPs3Archives(dir).empty())
    return {};
  std::error_code ec;
  if (fs::exists(dir / "PS3_GAME", ec) || fs::exists(dir / "PARAM.SFO", ec))
    return "This PS3 disc folder has no USRDIR/archives.";
  return "This folder does not hold the extracted game files (no index.vmtoc).";
}

// A file picked inside an extracted folder means the folder.
fs::path Resolve(const std::string& picked) {
  fs::path path(picked);
  std::error_code ec;
  if (!IsContentUri(picked) && fs::is_regular_file(path, ec) &&
      (SameFileName(path.filename().string(), kTableOfContents) ||
       SameFileName(path.extension().string(), ".xex")))
    return path.parent_path();
  return path;
}

std::string RunLaterPhases(const fs::path& dir, const InstallHooks& hooks) {
  std::string error = RunPhase(
      hooks, Phase::kConvert, [&] { return NeedsConversion(dir); },
      [&] { return ConvertPs3(dir, hooks.progress); });
  if (!error.empty())
    return error;
  return RunPhase(
      hooks, Phase::kPatch, [&] { return NeedsPatching(dir); },
      [&] { return PatchRelease(dir, hooks.progress); });
}

}  // namespace

const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::kExtract: return "Extraction";
    case Phase::kConvert: return "Conversion";
    case Phase::kPatch: return "Patching";
  }
  return "";
}

bool IsGameDirectory(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && (fs::is_regular_file(dir / kTableOfContents, ec) || IsPs3Directory(dir));
}

bool NeedsConversion(const fs::path& dir) {
  return IsPs3Directory(dir) && !IsPs3Converted(dir);
}

bool NeedsPatching(const fs::path& dir) {
  // Converted PS3 data is PAL's already, as only the PAL release converts.
  return !IsPs3Directory(dir) && !IsReleasePatched(dir);
}

bool IsExtractedSource(const std::string& picked) {
  if (IsContentUri(picked))
    return false;
  std::error_code ec;
  const fs::path path = Resolve(picked);
  return fs::is_directory(path, ec) && IsGameDirectory(path);
}

std::string InstallDirectory(const fs::path& dir, const InstallHooks& hooks) {
  Report(hooks, Phase::kExtract, PhaseState::kSkipped);
  return RunLaterPhases(dir, hooks);
}

std::string Install(const std::string& picked, const fs::path& assets, const InstallHooks& hooks,
                    fs::path& dir) {
  std::error_code ec;
  const fs::path path = Resolve(picked);
  const bool folder = !IsContentUri(picked) && fs::is_directory(path, ec);
  if (folder) {
    if (std::string error = CheckFolder(path); !error.empty())
      return error;
    if (IsGameDirectory(path)) {
      dir = path;
      return InstallDirectory(dir, hooks);
    }
  }

  Report(hooks, Phase::kExtract, PhaseState::kActive);
  std::string error;
  if (folder) {
    error = UnpackPs3(FindPs3Archives(path), assets, hooks.progress);
  } else {
    error = ExtractDiscImage(picked, assets, kTableOfContents, hooks.progress);
    if (error.empty() && !IsGameDirectory(assets))
      error = "The extracted files are incomplete.";
  }
  if (!error.empty())
    return error;
  Report(hooks, Phase::kExtract, PhaseState::kDone);
  dir = assets;
  return RunLaterPhases(dir, hooks);
}

}  // namespace eternalsonata
