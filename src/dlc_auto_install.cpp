// eternalsonata - ReXGlue Recompiled Project
//
// Auto-installs the "Piano Music Key" DLC from a project-local `dlc` folder.
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
// sub_82209A18 is state 2 -- the first thing that runs once the player has
// actually asked for the unlock key. Just before it, if no marketplace content
// is installed for this title, scan `dlc/` next to the executable for STFS
// packages and install them. No dialogs, no prompting, and nothing happens at
// all on a normal boot or when the DLC is already installed.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

#include "generated/eternalsonata_init.h"

namespace {

// Where the user drops packages, relative to the working directory -- the same
// place `assets`, `mods` and `logs` are resolved from.
constexpr const char* kDlcDirName = "dlc";

// STFS containers are identified by their 4-byte magic. "LIVE" is signed
// marketplace content (what the Piano Music Key package is), "PIRS" is
// unsigned Microsoft-distributed content, "CON " is console-signed.
bool LooksLikeStfsPackage(const std::filesystem::path& path) {
  FILE* file = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
    return false;
  }
#else
  file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return false;
  }
#endif
  char magic[4] = {};
  const size_t read = std::fread(magic, 1, sizeof(magic), file);
  std::fclose(file);
  if (read != sizeof(magic)) {
    return false;
  }
  return std::memcmp(magic, "LIVE", 4) == 0 || std::memcmp(magic, "CON ", 4) == 0 ||
         std::memcmp(magic, "PIRS", 4) == 0;
}

void InstallPackagesFromDlcDir() {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state) {
    return;
  }
  auto* content_manager = kernel_state->content_manager();
  if (!content_manager) {
    return;
  }

  // Already installed? Then the enumerator will find it and there is nothing
  // to do. This mirrors what state 4 is about to ask for: device id 1 (the
  // dummy HDD), the common xuid 0 that marketplace content is filed under, and
  // the running title id (ListContent's default).
  if (!content_manager
           ->ListContent(1, 0, rex::system::XContentType::kMarketplaceContent)
           .empty()) {
    return;
  }

  std::error_code ec;
  const auto dlc_dir = std::filesystem::current_path(ec) / kDlcDirName;
  if (ec || !std::filesystem::is_directory(dlc_dir, ec)) {
    REXLOG_INFO("DLC auto-install: no '{}' directory, nothing to install", kDlcDirName);
    return;
  }

  // Recursive so the retail package can be dropped in with its original
  // directory tree intact (.../4E4D07E4/00000002/<40-hex-char name>) rather
  // than requiring the user to dig the container file out of it.
  int installed = 0;
  std::filesystem::recursive_directory_iterator it(
      dlc_dir, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    REXLOG_WARN("DLC auto-install: could not walk '{}': {}", dlc_dir.string(), ec.message());
    return;
  }
  for (const auto& entry : it) {
    if (!entry.is_regular_file(ec) || ec) {
      continue;
    }
    if (!LooksLikeStfsPackage(entry.path())) {
      continue;
    }
    const auto result = content_manager->InstallContent(entry.path());
    if (XFAILED(result)) {
      REXLOG_WARN("DLC auto-install: failed to install '{}' (0x{:08X})", entry.path().string(),
                  uint32_t(result));
      continue;
    }
    REXLOG_INFO("DLC auto-install: installed '{}'", entry.path().string());
    ++installed;
  }

  if (!installed) {
    REXLOG_WARN("DLC auto-install: no STFS content packages found under '{}'", dlc_dir.string());
  }
}

}  // namespace

// sub_82209A18 - state 2 of the unlock-key state machine. Runs once per
// player-initiated unlock attempt, so retrying after dropping a package into
// `dlc/` works without a restart; the ListContent check above keeps the
// repeat cost to a directory scan.
REX_EXTERN(__imp__sub_82209A18);
REX_HOOK_RAW(sub_82209A18) {
    InstallPackagesFromDlcDir();
    __imp__sub_82209A18(ctx, base);
}
