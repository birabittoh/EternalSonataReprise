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

#include "install_pipeline.h"
#include "intro_screen.h"

// The storage probes come from the SDK's GameDataSelector
// (src/system/game_data_selector.cpp), which this replaces.

namespace eternalsonata {
namespace {

namespace fs = std::filesystem;

#if REX_PLATFORM_ANDROID
constexpr bool kCanPickFolder = false;
#else
constexpr bool kCanPickFolder = true;
#endif

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

InstallHooks MakeHooks() {
  InstallHooks hooks;
  // Without the intro the SDK's progress window shows it.
  if (IntroScreenAvailable()) {
    hooks.progress = ReportIntroProgress;
    hooks.phase = SetIntroPhase;
  }
  return hooks;
}

// Runs the install phases on `picked` and makes the result game_data_root.
// Empty when it worked, else why not.
std::string Prepare(const GameDataOptions& options, const std::string& picked) {
  fs::path dir;
  BeginIntroWork();
  g_extracting = true;
  std::string error = Install(picked, WritableBaseDir() / "assets", MakeHooks(), dir);
  g_extracting = false;
  EndIntroWork();
  if (error.empty())
    Use(options, dir);
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
  message += prompt.can_pick_folder ? ", or a folder with the extracted files or a PS3 disc dump." : ".";
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

void Ask(std::shared_ptr<Flow> flow);

void OnPicked(std::shared_ptr<Flow> flow, std::string picked) {
  if (flow->finished)
    return;
  if (!picked.empty()) {
    REXLOG_INFO("Selected {}", picked);
    flow->prompt.ready = false;
    try {
      flow->prompt.error = Prepare(flow->options, picked);
    } catch (const std::exception& e) {
      g_extracting = false;
      EndIntroWork();
      flow->prompt.error = std::string("Preparing the game files failed: ") + e.what();
    }
    if (flow->prompt.error.empty()) {
      // The player starts the game from the intro once every phase is done.
      if (!IntroScreenAvailable()) {
        Finish(flow, true);
        return;
      }
      flow->prompt.ready = true;
    } else {
      REXLOG_ERROR("{}: {}", picked, flow->prompt.error);
    }
  }
  Ask(flow);
}

void OnChoice(std::shared_ptr<Flow> flow, GameDataChoice choice) {
  if (flow->finished)
    return;
  flow->prompt.error.clear();
  if (choice == GameDataChoice::kQuit || choice == GameDataChoice::kStart) {
    Finish(flow, choice == GameDataChoice::kStart);
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

bool UsePreparedGameData(const GameDataOptions& options, std::string& pending) {
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
    if (NeedsConversion(dir) || NeedsPatching(dir)) {
      pending = dir.string();
      return false;
    }
    Use(options, dir);
    return true;
  }
  return false;
}

void AskForGameData(const GameDataOptions& options, rex::ui::WindowedAppContext& context,
                    std::string pending, std::function<void(bool ready)> done) {
  auto flow = std::make_shared<Flow>();
  flow->options = options;
  flow->context = &context;
  flow->done = std::move(done);
  flow->prompt.can_pick_folder = kCanPickFolder;
  flow->prompt.copy_hint = CopyHint();
  if (pending.empty()) {
    Ask(flow);
    return;
  }
  // Found but not through every phase: the intro shows the rest running.
  if (IntroScreenAvailable()) {
    ShowIntroScreen(flow->prompt, [flow](GameDataChoice choice) { OnChoice(flow, choice); });
    SetIntroBusy(true);
  }
  context.CallInUIThreadDeferred([flow, pending]() { OnPicked(flow, pending); });
}

bool IsExtractingGameData() {
  return g_extracting;
}

}  // namespace eternalsonata
