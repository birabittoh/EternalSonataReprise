#include "install_pipeline.h"

#include <fstream>
#include <iterator>
#include <vector>

#include <rex/logging.h>

#include "disc_image.h"
#include "ps3_install.h"
#include "release_id.h"
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

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

fs::path ChildNamed(const fs::path& dir, std::string_view name) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (SameFileName(entry.path().filename().string(), name))
      return entry.path();
  }
  return {};
}

// Known releases by their hash; a supported one installs.
SourceInfo FromHash(const std::vector<uint8_t>& file, const std::string& unknown) {
  const std::string hash = Sha256Hex(file);
  const Release* release = FindRelease(hash);
  if (!release) {
    REXLOG_WARN("Unknown dump: {}", hash);
    return {{}, unknown};
  }
  SourceInfo info{std::string("Eternal Sonata, ") + release->name, {}};
  if (!release->supported)
    info.error = info.release + " is not supported yet.";
  return info;
}

const char* RegionName(Ps3Region region) {
  switch (region) {
    case Ps3Region::kPal: return "Europe";
    case Ps3Region::kUsa: return "North America";
    case Ps3Region::kJapan: return "Japan";
  }
  return "";
}

// A PS3 dump's region from its title id: BLES Europe, BLUS America, BLJS Japan.
SourceInfo FromTitleId(const std::string& title_id) {
  if (title_id.size() < 3)
    return {};
  const Ps3Region region = title_id[2] == 'U'   ? Ps3Region::kUsa
                           : title_id[2] == 'J' ? Ps3Region::kJapan
                                                : Ps3Region::kPal;
  return {std::string("Eternal Sonata, PS3, ") + RegionName(region) + " (" + title_id + ")", {}};
}

SourceInfo IdentifyPs3Folder(const fs::path& archives) {
  const fs::path usrdir = archives.parent_path();
  const fs::path eboot = ChildNamed(usrdir, "EBOOT.BIN");
  std::vector<uint8_t> file;
  if (!eboot.empty() && ReadFile(eboot, file)) {
    SourceInfo info = FromHash(file, {});
    if (!info.release.empty())
      return info;
  }
  // An unknown EBOOT.BIN: named from the disc's title id when it has one.
  SourceInfo info = FromTitleId(ReadTitleId(ChildNamed(usrdir.parent_path(), "PARAM.SFO")));
  if (info.release.empty())
    return {{}, "This PS3 disc folder is not a known dump of Eternal Sonata."};
  info.error = info.release + " is not a known dump, or not supported yet.";
  return info;
}

// Prepared folders are taken as they are, named when their files say which
// release they came from.
SourceInfo IdentifyGameDirectory(const fs::path& dir) {
  if (IsPs3Directory(dir)) {
    const Ps3Region region = ReadPs3Region(dir);
    return {std::string("Eternal Sonata, PS3, ") + RegionName(region), {}};
  }
  std::vector<uint8_t> file;
  fs::path xex = ChildNamed(dir, "default.xex.orig");
  if (xex.empty())
    xex = ChildNamed(dir, "default.xex");
  if (xex.empty() || !ReadFile(xex, file))
    return {};
  SourceInfo info = FromHash(file, {});
  info.error.clear();
  return info;
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
  return !IsReleasePatched(dir);
}

bool IsExtractedSource(const std::string& picked) {
  if (IsContentUri(picked))
    return false;
  std::error_code ec;
  const fs::path path = Resolve(picked);
  return fs::is_directory(path, ec) && IsGameDirectory(path);
}

SourceInfo IdentifySource(const std::string& picked) {
  std::error_code ec;
  const fs::path path = Resolve(picked);
  if (!IsContentUri(picked) && fs::is_directory(path, ec)) {
    if (std::string error = CheckFolder(path); !error.empty())
      return {{}, error};
    if (IsGameDirectory(path))
      return IdentifyGameDirectory(path);
    return IdentifyPs3Folder(FindPs3Archives(path));
  }
  std::vector<uint8_t> xex;
  if (std::string error = ReadDiscImageFile(picked, "default.xex", xex); !error.empty())
    return {{}, error};
  return FromHash(xex, "This Eternal Sonata disc image is not a known dump.");
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
