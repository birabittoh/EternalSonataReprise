// eternalsonata - ReXGlue Recompiled Project
//
// Which release's data game_data_root holds. The PS3 release is a second
// target with its own engine behaviour (docs/ps3-assets.md), so everything
// PS3 specific is gated on IsPs3Target() and 360 mode stays untouched.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace eternalsonata {

// Latches the target from the resolved game directory. Call once, before the
// runtime is constructed.
void DetectTarget(const std::filesystem::path& game_data_root);

bool IsPs3Target();

// The directory DetectTarget latched.
const std::filesystem::path& GameDataRoot();

// The console and region that directory was installed from, as "Xbox 360,
// Europe", or empty when unknown. Hashes default.xex on first call.
const std::string& TargetRelease();

// That directory's library id ("360-pal", "ps3-jp"), or empty when unknown.
const std::string& TargetReleaseId();

// What a mod.toml `target` may name to match this run: the console ("360",
// "ps3") and the release id. RuntimeConfig::game_targets, so the SDK leaves
// out mods made for another version. Empty before DetectTarget.
std::vector<std::string> GameTargets();

// For readers of mods.toml that run without the runtime's filtered list.
bool ModRunsOnTarget(const std::filesystem::path& mod_root);

}  // namespace eternalsonata
