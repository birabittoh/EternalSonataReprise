// eternalsonata - ReXGlue Recompiled Project

#include "target.h"

#include <rex/logging.h>
#include <rex/system/mod_version.h>

#include "game_library.h"
#include "install_pipeline.h"

namespace eternalsonata {
namespace {

// Field character models moved out of AppKeep.bmd into these PS3 only files,
// so they are in every PS3 copy, unpacked or converted.
constexpr const char* kPs3Probe = "pcalg_v1.p3obj";

bool g_ps3 = false;
std::filesystem::path g_root;

}  // namespace

void DetectTarget(const std::filesystem::path& game_data_root) {
  g_root = game_data_root;
  std::error_code ec;
  g_ps3 = std::filesystem::is_regular_file(game_data_root / kPs3Probe, ec);
  REXLOG_INFO("Game data target: {} ({})", g_ps3 ? "PS3" : "Xbox 360",
              TargetReleaseId().empty() ? "unknown release" : TargetReleaseId());
}

bool IsPs3Target() {
  return g_ps3;
}

const std::filesystem::path& GameDataRoot() {
  return g_root;
}

const std::string& TargetRelease() {
  static const std::string release = [] {
    std::string name = ReadVersionRelease(g_root);
    if (name.empty())
      name = IdentifySource(g_root.string()).release;
    const size_t open = name.rfind('(');
    const size_t close = name.rfind(')');
    return open != std::string::npos && close > open ? name.substr(open + 1, close - open - 1)
                                                      : name;
  }();
  return release;
}

const std::string& TargetReleaseId() {
  static const std::string id = [] {
    std::string value = ReadVersionId(g_root);
    return value.empty() ? IdentifySource(g_root.string()).id : value;
  }();
  return id;
}

std::vector<std::string> GameTargets() {
  if (g_root.empty())
    return {};
  std::vector<std::string> targets{g_ps3 ? "ps3" : "360"};
  if (!TargetReleaseId().empty())
    targets.push_back(TargetReleaseId());
  return targets;
}

bool ModRunsOnTarget(const std::filesystem::path& mod_root) {
  return rex::system::ModTargetsMatch(rex::system::ParseModInfo(mod_root).targets, GameTargets());
}

}  // namespace eternalsonata
