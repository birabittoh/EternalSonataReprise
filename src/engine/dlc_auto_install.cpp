// eternalsonata - ReXGlue Recompiled Project
//
// Asks for the Piano Music Key DLC package when the game looks for it.
//
// The Music menu's Unlock Key action runs a state machine in sub_82209FB0 (state
// byte at +0x15 of the object in dword_8244012C, error code at +0x20). State 2,
// sub_82209A18, opens the device selector; state 4 enumerates marketplace
// content. XamContentCreate is never called, so the load is driven purely by
// that enumeration, which comes back empty when nothing is installed.
//
// ContentManager::ListContent scans
// <content root>/0000000000000000/<title id>/00000002/<name>/ for this game's
// own title id (0x4E4D07E2), while the retail package is filed under 4E4D07E4.
// Installing through ContentManager::InstallContent avoids that mismatch.
//
// The hook on sub_82209A18 runs when no marketplace content is installed: it
// asks the player to confirm, then lets them pick the package and installs it.
// Declining, cancelling or a bad file changes nothing, so the game goes on to
// its own "failed to read" message.

#include <cstdint>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

#include "eternalsonata_hooks_internal.h"
#include "settings.h"
#include "generated/eternalsonata_init.h"

namespace {

// STFS containers start with one of these magics.
bool LooksLikeStfsPackage(const void* data, size_t size) {
  if (size < 4) {
    return false;
  }
  const std::string_view m(static_cast<const char*>(data), 4);
  return m == "LIVE" || m == "CON " || m == "PIRS";
}

// The installed folder is named after the file; a picked Android content URI
// carries it after the last (possibly escaped) separator.
std::string PackageName(std::string picked) {
  for (std::string_view separator : {"/", "\\", "%2F", "%2f", "%3A", "%3a"}) {
    if (const size_t at = picked.rfind(separator); at != std::string::npos) {
      picked.erase(0, at + separator.size());
    }
  }
  return picked;
}

struct PickRequest {
  std::promise<std::string> picked;
};

void SDLCALL OnPicked(void* userdata, const char* const* files, int) {
  std::unique_ptr<PickRequest> request(static_cast<PickRequest*>(userdata));
  if (!files) {
    REXLOG_WARN("DLC: the file dialog failed: {}", SDL_GetError());
  }
  request->picked.set_value(files && files[0] ? files[0] : "");
}

// Blocks the calling guest thread until the player answers; empty if cancelled.
// Goes through the SDK's UI loop: SDL_RunOnMainThread here would reach this
// executable's own SDL copy, whose callback queue nothing ever pumps.
std::string PickPackage() {
  auto* request = new PickRequest;
  auto picked = request->picked.get_future();
  if (!eternalsonata::CallInWindowThread([request] {
        SDL_ShowOpenFileDialog(OnPicked, request, nullptr, nullptr, 0, nullptr, false);
      })) {
    delete request;
    return {};
  }
  REXLOG_INFO("DLC: waiting for the package picker");
  return picked.get();
}

// False once marketplace content is installed.
bool NeedsPackage() {
  auto* kernel_state = rex::system::kernel_state();
  auto* content_manager = kernel_state ? kernel_state->content_manager() : nullptr;
  if (!content_manager) {
    return false;
  }
  // Mirrors what state 4 asks for: the dummy HDD, the common xuid 0 that
  // marketplace content is filed under, and the running title id.
  return content_manager->ListContent(1, 0, rex::system::XContentType::kMarketplaceContent).empty();
}

bool InstallPickedPackage() {
  auto* kernel_state = rex::system::kernel_state();
  auto* content_manager = kernel_state->content_manager();
  const std::string picked = PickPackage();
  if (picked.empty()) {
    return false;
  }
  // SDL reads content URIs as well as paths; the package is copied to the cache
  // since InstallContent needs a real file.
  size_t size = 0;
  std::unique_ptr<void, decltype(&SDL_free)> data(SDL_LoadFile(picked.c_str(), &size), SDL_free);
  if (!data || !LooksLikeStfsPackage(data.get(), size)) {
    REXLOG_WARN("DLC: '{}' is not an STFS package", picked);
    return false;
  }
  const std::string name = PackageName(picked);
  const auto cache = kernel_state->emulator()->user_data_root() / "cache";
  std::error_code ec;
  std::filesystem::create_directories(cache, ec);
  const auto package = cache / name;
  {
    std::ofstream out(package, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data.get()), std::streamsize(size));
    if (!out) {
      REXLOG_WARN("DLC: could not write '{}'", package.string());
      return false;
    }
  }
  const auto result = content_manager->InstallContent(package);
  std::filesystem::remove(package, ec);
  if (XFAILED(result)) {
    REXLOG_WARN("DLC: failed to install '{}' (0x{:08X})", package.string(), uint32_t(result));
    return false;
  }
  REXLOG_INFO("DLC: installed '{}'", package.string());
  return true;
}

}  // namespace

// sub_82209A18 - state 2 of the unlock-key state machine.
REX_EXTERN(__imp__sub_82209A18);

constexpr uint32_t kUnlockObject = 0x8244012C;

bool eternalsonata::ResumeInstallDlc(PPCContext& ctx, uint8_t* base) {
  if (!InstallPickedPackage()) {
    return false;
  }
  const PPCContext saved = ctx;
  ctx.r3.u32 = REX_LOAD_U32(kUnlockObject);
  __imp__sub_82209A18(ctx, base);
  ctx = saved;
  return true;
}

REX_HOOK_RAW(sub_82209A18) {
  if (NeedsPackage()) {
    // The state stays 2 while the dialog is up; it is left from the menu tick.
    eternalsonata::AskInstallDlc(ctx, base);
    // The menu tick adds the return value to its substate.
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82209A18(ctx, base);
}
