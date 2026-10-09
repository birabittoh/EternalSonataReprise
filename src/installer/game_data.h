#pragma once

// Resolves game_data_root before the runtime starts, through the install
// phases of install_pipeline.h, shown on the intro screen. A picked source is
// identified first and installed only when the player presses Extract; the
// game starts only when the player presses Start after every phase is done. A
// prepared directory with nothing left to do skips the screen.

#include <filesystem>
#include <functional>
#include <string>

#include "install_pipeline.h"

namespace rex::ui {
class WindowedAppContext;
}

namespace eternalsonata {

enum class GameDataChoice { kQuit, kDiscImage, kFolder, kExtract, kStart };

// What the prompt is asked to offer.
struct GameDataPrompt {
  // False on Android, where SDL has no folder picker.
  bool can_pick_folder = true;
  // Where already extracted files can be copied to be found, or empty.
  std::string copy_hint;
  // Why the previous selection failed, empty on the first call.
  std::string error;
  // The release picked, once identified.
  std::string release;
  // A supported source is picked: Extract is enabled.
  bool can_extract = false;
  // Every phase is done: Start is enabled.
  bool ready = false;
};

struct GameDataOptions {
  // The config game_data_root is persisted to; relative values resolve against
  // its folder.
  std::filesystem::path config_path;
};

// Sets and persists game_data_root from the cvar or an earlier extraction when
// it needs no more work. False otherwise, with `pending` set to a directory
// that still needs conversion or patching, or left empty when none was found.
bool UsePreparedGameData(const GameDataOptions& options, std::string& pending);

// Asks through the intro screen (a message box where it cannot draw) until
// game data is ready and started, or the player quits. A non empty `pending`
// runs the phases on that directory first. Returns at once: the main loop has
// to run for the screen to get input. `done` runs on the UI thread.
void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::string pending, std::function<void(bool ready)> done);

// Whether game files are being installed; the window must not
// close then.
bool IsExtractingGameData();

}  // namespace eternalsonata
