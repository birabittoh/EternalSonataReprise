#pragma once

// Resolves game_data_root before the runtime starts, in two steps: extraction
// of an Xbox 360 disc image into the writable base dir, then patching
// (release_patch.h). A folder picked or found already extracted only goes
// through the second.

#include <filesystem>
#include <functional>
#include <string>

#include "disc_image.h"

namespace rex::ui {
class WindowedAppContext;
}

namespace eternalsonata {

enum class GameDataChoice { kQuit, kDiscImage, kFolder };

// What the prompt is asked to offer.
struct GameDataPrompt {
  // False on Android, where SDL has no folder picker.
  bool can_pick_folder = true;
  // Where already extracted files can be copied to be found, or empty.
  std::string copy_hint;
  // Why the previous selection failed, empty on the first call.
  std::string error;
};

struct GameDataOptions {
  // The config game_data_root is persisted to; relative values resolve against
  // its folder.
  std::filesystem::path config_path;
  ExtractProgress progress;
};

// Whether `dir` holds game data this build can run.
bool IsGameDirectory(const std::filesystem::path& dir);

// Sets and persists game_data_root from the cvar or an earlier extraction,
// patching it first if it has not been. False when neither holds game data,
// or with `error` set when patching failed.
bool UsePreparedGameData(const GameDataOptions& options, std::string& error);

// Asks through the intro screen (a message box where it cannot draw) until
// game data is ready or the player quits, showing `error` first if not empty.
// Returns at once: the main loop has to run for the screen to get input.
// `done` runs on the UI thread.
void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::string error, std::function<void(bool ready)> done);

// Whether game files are being extracted or patched; the window must not
// close then.
bool IsExtractingGameData();

}  // namespace eternalsonata
