// eternalsonata - ReXGlue Recompiled Project
//
// Asks for the Piano Unlock Key DLC package when the game looks for it.
//
// ---------------------------------------------------------------------------
// Why this lives here and not in the SDK
// ---------------------------------------------------------------------------
//
// The obvious-looking SDK fix -- prompting the user for a package file from
// inside XamContentCreate's OPEN_EXISTING path -- is wrong twice over. It
// never fires for this title (see the flow below, which never calls
// XamContentCreate at all), and plenty of games probe for optional DLC during
// normal startup with no user intent behind it, so a modal file picker there
// would ambush players of every other title built on the SDK. Keep it
// game-specific.
//
// ---------------------------------------------------------------------------
// The guest-side DLC flow (reverse-engineered from default.xex)
// ---------------------------------------------------------------------------
//
// The music-player menu's "Y: Unlock Key" action drives a 12-state machine in
// sub_82209FB0 (state byte at +0x15 of the object in dword_8244012C, error
// code at +0x20). The bctr jump table is byte_82082328, based at
// loc_8220A008:
//
//   state 1  -> XamUserGetSigninState; 0 (not signed in) = error 6
//   state 2  -> sub_82209A18: XamShowDeviceSelectorUI(user, type=2,
//               size=0x300, overlapped); must return 997 (IO_PENDING),
//               otherwise error 5
//   state 3  -> polls the overlapped; picks up the device id, 0 = cancelled
//   state 4  -> sub_82209AF0: sub_8223C1E0 -> XamContentCreateEnumerator(
//               user, device_id, content_type=2 /* kMarketplaceContent */)
//               followed by XamEnumerate; failure = error 2
//   state 5  -> waits on the async enumerate
//   state 6+ -> sub_82209BA0 and friends draw the result
//   state 11 -> sub_82209E80, the error box: it looks the message up with
//               sub_8223B780("BTX ", <error code>), i.e. the error code at
//               +0x20 doubles as a BTX string id. "Failed to read Unlock Key"
//               is one of those ids.
//
// The load is therefore driven entirely by *enumeration* of marketplace
// content -- XamContentCreateEx is never involved. With nothing installed the
// enumerator legitimately comes back empty and the game reports failure, which
// is exactly the symptom.
//
// ContentManager::ListContent scans
//   <content root>/0000000000000000/<title id>/00000002/<name>/
// and the title id it wants is this game's own (0x4E4D07E2). Note that the
// retail DLC package ships filed under 4E4D07E4 in its own directory tree --
// one hex digit off -- so hand-copying the extracted folder lands it where
// nothing will ever look for it. Installing through the SDK's
// ContentManager::InstallContent avoids the whole class of mistake: it mounts
// the STFS container, extracts it to the correct path, and writes a header
// file in the layout ReadContentHeaderFile actually expects.
//
// ---------------------------------------------------------------------------
// What this hook does
// ---------------------------------------------------------------------------
//
// sub_82209A18 is state 2, the first thing that runs once the player has asked
// for the unlock key. Just before it, if no marketplace content is installed
// for this title, a file dialog asks for the package and installs it. A
// cancelled dialog or a bad file changes nothing, so the game goes on to its
// own "failed to read" message.

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
  request->picked.set_value(files && files[0] ? files[0] : "");
}

void SDLCALL ShowPicker(void* userdata) {
  SDL_ShowOpenFileDialog(OnPicked, userdata, nullptr, nullptr, 0, nullptr, false);
}

// Blocks the calling guest thread until the player answers; empty if cancelled.
std::string PickPackage() {
  auto* request = new PickRequest;
  auto picked = request->picked.get_future();
  if (!SDL_RunOnMainThread(ShowPicker, request, false)) {
    delete request;
    return {};
  }
  return picked.get();
}

void InstallPickedPackage() {
  auto* kernel_state = rex::system::kernel_state();
  auto* content_manager = kernel_state ? kernel_state->content_manager() : nullptr;
  if (!content_manager) {
    return;
  }
  // Mirrors what state 4 asks for: the dummy HDD, the common xuid 0 that
  // marketplace content is filed under, and the running title id.
  if (!content_manager->ListContent(1, 0, rex::system::XContentType::kMarketplaceContent).empty()) {
    return;
  }
  const std::string picked = PickPackage();
  if (picked.empty()) {
    return;
  }
  // SDL reads content URIs as well as paths; the package is copied to the cache
  // since InstallContent needs a real file.
  size_t size = 0;
  std::unique_ptr<void, decltype(&SDL_free)> data(SDL_LoadFile(picked.c_str(), &size), SDL_free);
  if (!data || !LooksLikeStfsPackage(data.get(), size)) {
    REXLOG_WARN("DLC: '{}' is not an STFS package", picked);
    return;
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
      return;
    }
  }
  const auto result = content_manager->InstallContent(package);
  std::filesystem::remove(package, ec);
  if (XFAILED(result)) {
    REXLOG_WARN("DLC: failed to install '{}' (0x{:08X})", package.string(), uint32_t(result));
    return;
  }
  REXLOG_INFO("DLC: installed '{}'", package.string());
}

}  // namespace

// sub_82209A18 - state 2 of the unlock-key state machine.
REX_EXTERN(__imp__sub_82209A18);
REX_HOOK_RAW(sub_82209A18) {
    InstallPickedPackage();
    __imp__sub_82209A18(ctx, base);
}
