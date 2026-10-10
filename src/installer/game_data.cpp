#include "game_data.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

#include "install_pipeline.h"
#include "intro_screen.h"
#include "ui_text.h"

// The storage probes come from the SDK's GameDataSelector
// (src/system/game_data_selector.cpp), which this replaces.

REXCVAR_DEFINE_BOOL(show_start_screen, false, "Eternal Sonata",
                    "Show the start screen even when game data is already prepared");

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

// Symlinks and junctions, which are never followed or deleted through.
bool IsLink(const fs::path& path) {
  std::error_code ec;
  if (fs::is_symlink(path, ec))
    return true;
#if REX_PLATFORM_WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return false;
#endif
}

// Any game directory, the installer's or one the player pointed at. A link
// cannot be emptied without going through it.
bool IsDeletable(const fs::path& dir) {
  std::error_code ec;
  return !IsLink(dir) && fs::is_directory(dir, ec) && IsGameDirectory(dir);
}

struct DeleteJob {
  std::atomic<uint64_t> done{0};
  std::atomic<bool> finished{false};
  std::string error;
};

// Regular files and folders under `dir`, links skipped, not entered.
uint64_t TreeSize(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!IsLink(it->path()) && it->is_regular_file(entry_ec))
      total += it->file_size(entry_ec);
  }
  return total;
}

void DeleteTree(const fs::path& dir, DeleteJob& job) {
  std::error_code ec;
  std::vector<fs::path> folders{dir};
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    const fs::path path = it->path();
    if (IsLink(path)) {
      it.disable_recursion_pending();
      continue;
    }
    std::error_code entry_ec;
    if (it->is_directory(entry_ec)) {
      folders.push_back(path);
    } else {
      const uint64_t size = it->file_size(entry_ec);
      if (!fs::remove(path, entry_ec) && entry_ec) {
        job.error = path.string() + ": " + entry_ec.message();
        break;
      }
      job.done += size;
    }
  }
  if (job.error.empty() && ec)
    job.error = dir.string() + ": " + ec.message();
  // Deepest first; one holding a skipped link stays.
  for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
    std::error_code remove_ec;
    fs::remove(*folder, remove_ec);
  }
  job.finished = true;
}

std::string FormatSize(uint64_t bytes) {
  char text[32];
  if (bytes >= (1ull << 30))
    std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / double(1ull << 30));
  else
    std::snprintf(text, sizeof(text), "%.0f MB", double(bytes) / double(1ull << 20));
  return text;
}

std::string DeleteQuestion(const std::string& release, const fs::path& dir, uint64_t bytes) {
  std::string message = Tr(IntroText::kConfirmDelete, release);
  if (const size_t at = message.find("{}"); at != std::string::npos)
    message.replace(at, 2, FormatSize(bytes));
  return message + "\n" + dir.string();
}

// Kept alive by every callback still pending, so a dialog answering after
// the player quit lands nowhere.
struct Flow {
  GameDataOptions options;
  rex::ui::WindowedAppContext* context = nullptr;
  std::function<void(bool)> done;
  GameDataPrompt prompt;
  // The identified source Extract installs.
  std::string picked;
  // The prepared directory on offer, when the screen was asked for on demand.
  fs::path prepared;
  bool finished = false;
};

std::atomic<bool> g_extracting{false};
// A finished directory is on offer; set by UsePreparedGameData.
bool g_review = false;

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
  // A content link has no folder to resolve a dump's file to.
  static const SDL_DialogFileFilter kFilters[] = {
#if REX_PLATFORM_ANDROID
      {"Eternal Sonata (disc image)", "iso"},
#else
      {"Eternal Sonata (disc image, EBOOT.BIN, default.xex)", "iso;bin;xex"},
#endif
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
  message += Tr(IntroText::kNeedFiles);
  if (!prompt.can_pick_folder)
    message += std::string(" ") + Tr(IntroText::kSelectIsoOnly);
  if (!prompt.copy_hint.empty())
    message += "\n\n" + Tr(IntroText::kCopyHint, prompt.copy_hint);
  if (prompt.can_extract)
    message = Tr(IntroText::kFoundRelease, prompt.release) + "\n\n" + message;
  SDL_MessageBoxButtonData buttons[4];
  int count = 0;
  if (prompt.can_extract)
    buttons[count++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, int(GameDataChoice::kExtract),
                        Tr(IntroText::kExtract)};
  buttons[count++] = {prompt.can_extract ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
                      int(GameDataChoice::kDiscImage), Tr(IntroText::kSelectFile)};
  if (prompt.can_pick_folder)
    buttons[count++] = {0, int(GameDataChoice::kFolder), Tr(IntroText::kSelectFolder)};
  buttons[count++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, int(GameDataChoice::kQuit), Tr(IntroText::kQuit)};
  const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, Tr(IntroText::kTitle),
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

// Runs the phases on the identified source.
void Extract(std::shared_ptr<Flow> flow) {
  if (flow->finished)
    return;
  flow->prompt.can_extract = false;
  try {
    flow->prompt.error = Prepare(flow->options, flow->picked);
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
    REXLOG_ERROR("{}: {}", flow->picked, flow->prompt.error);
    // Extract again after fixing what went wrong, such as disk space.
    flow->prompt.can_extract = true;
  }
  Ask(flow);
}

// Identifies the pick; extracting waits for the player.
void OnPicked(std::shared_ptr<Flow> flow, std::string picked) {
  if (flow->finished)
    return;
  if (!picked.empty()) {
    REXLOG_INFO("Selected {}", picked);
    const SourceInfo info = IdentifySource(picked);
    flow->prompt.ready = false;
    flow->prompt.release = info.release;
    flow->prompt.languages = info.languages;
    flow->prompt.error = info.error;
    flow->prompt.can_extract = info.error.empty();
    flow->picked = info.error.empty() ? picked : std::string();
    if (!info.release.empty())
      REXLOG_INFO("Identified {}", info.release);
    if (!info.error.empty())
      REXLOG_ERROR("{}: {}", picked, info.error);
  }
  Ask(flow);
}

void AskToDelete(std::shared_ptr<Flow> flow) {
  const fs::path dir = flow->prepared;
  if (!flow->prompt.can_delete || dir.empty() || !IsDeletable(dir)) {
    Ask(flow);
    return;
  }
  flow->prompt.confirm = DeleteQuestion(flow->prompt.release, dir, TreeSize(dir));
  Ask(flow);
}

void DeletePrepared(std::shared_ptr<Flow> flow) {
  const fs::path dir = flow->prepared;
  const uint64_t total = TreeSize(dir);
  REXLOG_INFO("Deleting {}", dir.string());
  BeginIntroWork();
  for (Phase phase : {Phase::kExtract, Phase::kConvert, Phase::kPatch})
    SetIntroPhase(phase, PhaseState::kSkipped);
  DeleteJob job;
  g_extracting = true;
  std::thread worker(DeleteTree, dir, std::ref(job));
  while (!job.finished) {
    const uint64_t done = job.done;
    ReportIntroProgress("Deleting game files...", total ? float(double(done) / double(total)) : -1.0f,
                        FormatSize(done) + " / " + FormatSize(total));
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  worker.join();
  g_extracting = false;
  EndIntroWork();

  g_review = false;
  flow->prepared.clear();
  flow->picked.clear();
  flow->prompt.release.clear();
  flow->prompt.languages = 0;
  flow->prompt.ready = false;
  flow->prompt.can_extract = false;
  flow->prompt.can_delete = false;
  flow->prompt.error = job.error;
  REXCVAR_SET(game_data_root, std::string());
  if (!flow->options.config_path.empty())
    rex::cvar::SaveConfigSubset(flow->options.config_path, {"game_data_root"});
  Ask(flow);
}

void OnChoice(std::shared_ptr<Flow> flow, GameDataChoice choice) {
  if (flow->finished)
    return;
  flow->prompt.error.clear();
  if (choice == GameDataChoice::kQuit || choice == GameDataChoice::kStart) {
    if (choice == GameDataChoice::kStart)
      ConfirmIntroLanguage();
    Finish(flow, choice == GameDataChoice::kStart);
    return;
  }
  if (choice == GameDataChoice::kDelete) {
    AskToDelete(flow);
    return;
  }
  if (choice == GameDataChoice::kCancel) {
    flow->prompt.confirm.clear();
    Ask(flow);
    return;
  }
  if (choice == GameDataChoice::kConfirmDelete) {
    flow->prompt.confirm.clear();
    if (flow->prompt.can_delete && IsDeletable(flow->prepared))
      DeletePrepared(flow);
    else
      Ask(flow);
    return;
  }
  if (choice == GameDataChoice::kExtract) {
    if (flow->prompt.can_extract)
      Extract(flow);
    else
      Ask(flow);
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
    if (REXCVAR_GET(show_start_screen)) {
      pending = dir.string();
      g_review = true;
      return false;
    }
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
  if (g_review) {
    // Finished data: Start works at once, and another source can still be picked.
    const SourceInfo info = IdentifySource(pending);
    flow->prompt.release = info.release;
    flow->prompt.languages = info.languages;
    flow->prompt.ready = true;
    flow->prepared = pending;
    flow->prompt.can_delete = IsDeletable(flow->prepared);
    Ask(flow);
    return;
  }
  // Found but not through every phase: the intro shows the rest running.
  if (IntroScreenAvailable()) {
    ShowIntroScreen(flow->prompt, [flow](GameDataChoice choice) { OnChoice(flow, choice); });
    SetIntroBusy(true);
  }
  flow->picked = pending;
  const SourceInfo info = IdentifySource(pending);
  flow->prompt.release = info.release;
  flow->prompt.languages = info.languages;
  context.CallInUIThreadDeferred([flow]() { Extract(flow); });
}

bool IsExtractingGameData() {
  return g_extracting;
}

}  // namespace eternalsonata
