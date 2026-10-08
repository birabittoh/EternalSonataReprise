#include "game_data.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/windowed_app_context.h>

#include "disc_image.h"
#include "intro_screen.h"
#include "release_patch.h"

// The storage probes come from the SDK's GameDataSelector
// (src/system/game_data_selector.cpp), which this replaces.

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

constexpr const char* kTableOfContents = "index.vmtoc";

#if REX_PLATFORM_ANDROID
constexpr bool kCanPickFolder = false;
#else
constexpr bool kCanPickFolder = true;
#endif

bool IsContentUri(std::string_view path) {
  return path.starts_with("content://");
}

// Same folder the SDK extracted into, so earlier installs are still found.
fs::path WritableBaseDir() {
#if REX_PLATFORM_ANDROID
  if (char* pref = SDL_GetPrefPath("rexglue", "eternalsonata")) {
    fs::path p(pref);
    SDL_free(pref);
    return p;
  }
  return "/data/local/tmp";
#else
  return rex::filesystem::GetExecutableFolder();
#endif
}

std::vector<fs::path> PreparedCandidates() {
  std::vector<fs::path> dirs;
  auto push = [&dirs](fs::path p) {
    if (!p.empty() && std::find(dirs.begin(), dirs.end(), p) == dirs.end())
      dirs.push_back(std::move(p));
  };
  push(WritableBaseDir() / "assets");
#if REX_PLATFORM_ANDROID
  if (const char* internal_path = SDL_GetAndroidInternalStoragePath())
    push(fs::path(internal_path) / "assets");
  if (const char* external_path = SDL_GetAndroidExternalStoragePath())
    push(fs::path(external_path) / "assets");
#endif
  return dirs;
}

// The one candidate a player can copy files into, `/sdcard` shortened.
std::string CopyHint() {
#if REX_PLATFORM_ANDROID
  const char* external_path = SDL_GetAndroidExternalStoragePath();
  if (!external_path)
    return {};
  std::string p = (fs::path(external_path) / "assets").string();
  constexpr std::string_view kEmulated = "/storage/emulated/0";
  if (p.starts_with(kEmulated))
    p = "/sdcard" + p.substr(kEmulated.size());
  return p;
#else
  return {};
#endif
}

std::string CheckFolder(const fs::path& dir) {
  if (IsGameDirectory(dir))
    return {};
  std::error_code ec;
  if (fs::exists(dir / "PS3_GAME", ec) || fs::exists(dir / "PARAM.SFO", ec))
    return "A PS3 disc has to be converted first (scripts/ps3_convert.py).";
  return "This folder does not hold the extracted game files (no index.vmtoc).";
}

// Kept alive by every callback still pending, so a dialog answering after
// the player quit lands nowhere.
struct Flow {
  GameDataOptions options;
  rex::ui::WindowedAppContext* context = nullptr;
  std::function<void(bool)> done;
  GameDataPrompt prompt;
  bool finished = false;
};

std::atomic<bool> g_extracting{false};

fs::path ConfigDir(const GameDataOptions& options) {
  return options.config_path.parent_path();
}

// Stored relative to the config when inside it, so the folder can move whole.
void Use(const GameDataOptions& options, const fs::path& dir) {
  REXCVAR_SET(game_data_root, rex::filesystem::RelativeIfInside(dir, ConfigDir(options)).string());
  if (!options.config_path.empty())
    rex::cvar::SaveConfigSubset(options.config_path, {"game_data_root"});
  REXLOG_INFO("Game data set to {}", dir.string());
}

// The second install step, separate from extraction so a folder extracted
// earlier needs only this one.
std::string PatchGameData(const GameDataOptions& options, const fs::path& dir) {
  if (IsReleasePatched(dir))
    return {};
  g_extracting = true;
  std::string error = PatchRelease(dir, options.progress);
  g_extracting = false;
  return error;
}

struct DialogRequest {
  std::shared_ptr<Flow> flow;
  std::function<void(std::shared_ptr<Flow>, std::string)> then;
};

// May run on the dialog's own thread (Windows), so it only posts the answer.
void SDLCALL DialogCallback(void* userdata, const char* const* files, int) {
  std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
  if (!files)
    REXLOG_ERROR("The file dialog failed: {}", SDL_GetError());
  std::string picked = files && files[0] ? files[0] : "";
  auto flow = request->flow;
  auto then = std::move(request->then);
  flow->context->CallInUIThreadDeferred(
      [flow, then, picked]() { then(flow, picked); });
}

void ShowDialog(std::shared_ptr<Flow> flow, bool folder,
                std::function<void(std::shared_ptr<Flow>, std::string)> then) {
  static const SDL_DialogFileFilter kFilters[] = {
      {"Xbox 360 disc image", "iso"},
      {"All files", "*"},
  };
  auto* request = new DialogRequest{std::move(flow), std::move(then)};
  if (folder)
    SDL_ShowOpenFolderDialog(DialogCallback, request, nullptr, nullptr, false);
  else
    SDL_ShowOpenFileDialog(DialogCallback, request, nullptr, kFilters, 2, nullptr, false);
}

// Where no intro screen can draw.
GameDataChoice MessageBoxPrompt(const GameDataPrompt& prompt) {
  std::string message = prompt.error.empty() ? "" : prompt.error + "\n\n";
  message += "Eternal Sonata needs the original game files. Select an Xbox 360 disc image to extract";
  message += prompt.can_pick_folder ? ", or a folder with the extracted files." : ".";
  if (!prompt.copy_hint.empty())
    message += "\n\nAlready extracted? Copy them to\n" + prompt.copy_hint;
  SDL_MessageBoxButtonData buttons[3];
  int count = 0;
  buttons[count++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, int(GameDataChoice::kDiscImage),
                      "Select Disc Image..."};
  if (prompt.can_pick_folder)
    buttons[count++] = {0, int(GameDataChoice::kFolder), "Select Folder..."};
  buttons[count++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, int(GameDataChoice::kQuit), "Quit"};
  const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, "Game Files Required",
                                  message.c_str(), count, buttons, nullptr};
  int id = int(GameDataChoice::kQuit);
  if (!SDL_ShowMessageBox(&box, &id))
    return GameDataChoice::kQuit;
  return GameDataChoice(id);
}

void Finish(const std::shared_ptr<Flow>& flow, bool ready) {
  if (flow->finished)
    return;
  flow->finished = true;
  HideIntroScreen();
  flow->done(ready);
}

// Empty when the pick is now game_data_root, else why not.
std::string Prepare(const GameDataOptions& options, const std::string& picked) {
  fs::path path(picked);
  std::error_code ec;
  const bool local = !IsContentUri(picked);
  // Picking a file inside an extracted folder means the folder.
  if (local && fs::is_regular_file(path, ec) &&
      (SameFileName(path.filename().string(), kTableOfContents) ||
       SameFileName(path.extension().string(), ".xex")))
    path = path.parent_path();

  if (local && fs::is_directory(path, ec)) {
    std::string error = CheckFolder(path);
    if (error.empty()) {
      HideIntroScreen();
      error = PatchGameData(options, path);
    }
    if (error.empty())
      Use(options, path);
    return error;
  }

  // The loading screen draws the extraction; the intro would draw over it.
  HideIntroScreen();
  const fs::path out_dir = WritableBaseDir() / "assets";
  g_extracting = true;
  std::string error = ExtractDiscImage(picked, out_dir, kTableOfContents, options.progress);
  g_extracting = false;
  if (error.empty() && !IsGameDirectory(out_dir))
    error = "The extracted files are incomplete.";
  if (error.empty())
    error = PatchGameData(options, out_dir);
  if (error.empty())
    Use(options, out_dir);
  return error;
}

void Ask(std::shared_ptr<Flow> flow);

void OnPicked(std::shared_ptr<Flow> flow, std::string picked) {
  if (flow->finished)
    return;
  if (!picked.empty()) {
    REXLOG_INFO("Selected {}", picked);
    try {
      flow->prompt.error = Prepare(flow->options, picked);
    } catch (const std::exception& e) {
      g_extracting = false;
      flow->prompt.error = std::string("Preparing the game files failed: ") + e.what();
    }
    if (flow->prompt.error.empty()) {
      Finish(flow, true);
      return;
    }
    REXLOG_ERROR("{}: {}", picked, flow->prompt.error);
  }
  Ask(flow);
}

void OnChoice(std::shared_ptr<Flow> flow, GameDataChoice choice) {
  if (flow->finished)
    return;
  flow->prompt.error.clear();
  if (choice == GameDataChoice::kQuit) {
    Finish(flow, false);
    return;
  }
  SetIntroBusy(true);
  ShowDialog(flow, kCanPickFolder && choice == GameDataChoice::kFolder, OnPicked);
}

void Ask(std::shared_ptr<Flow> flow) {
  if (IntroScreenAvailable()) {
    ShowIntroScreen(flow->prompt, [flow](GameDataChoice choice) { OnChoice(flow, choice); });
    return;
  }
  flow->context->CallInUIThreadDeferred(
      [flow]() { OnChoice(flow, MessageBoxPrompt(flow->prompt)); });
}

}  // namespace

bool IsGameDirectory(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / kTableOfContents, ec);
}

bool UsePreparedGameData(const GameDataOptions& options, std::string& error) {
  std::vector<fs::path> dirs;
  const std::string value = REXCVAR_GET(game_data_root);
  if (!value.empty())
    dirs.push_back(rex::filesystem::ResolveRelativeTo(fs::path(value), ConfigDir(options)));
  for (fs::path& dir : PreparedCandidates())
    dirs.push_back(std::move(dir));
  for (const fs::path& dir : dirs) {
    if (!IsGameDirectory(dir))
      continue;
    REXLOG_INFO("Found game data at {}", dir.string());
    try {
      error = PatchGameData(options, dir);
    } catch (const std::exception& e) {
      g_extracting = false;
      error = std::string("Patching the game files failed: ") + e.what();
    }
    if (!error.empty()) {
      REXLOG_ERROR("{}: {}", dir.string(), error);
      return false;
    }
    Use(options, dir);
    return true;
  }
  return false;
}

void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::string error, std::function<void(bool ready)> done) {
  auto flow = std::make_shared<Flow>();
  flow->prompt.error = std::move(error);
  flow->options = options;
  flow->context = &context;
  flow->done = std::move(done);
  flow->prompt.can_pick_folder = kCanPickFolder;
  flow->prompt.copy_hint = CopyHint();
  Ask(flow);
}

bool IsExtractingGameData() {
  return g_extracting;
}

}  // namespace eternalsonata
