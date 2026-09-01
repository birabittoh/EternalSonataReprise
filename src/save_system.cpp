// eternalsonata - Save state: observing the game's save flow, for mods.
//
// Everything here was derived from the retail xex. The short version:
//
//   * The pause menu's Save entry is gated on byte_8243C360, "the party is
//     standing at a save point". Nothing in default.xex writes it; the field
//     scripts do. sub_821E51B0 latches it into byte_8243FBFB when the menu
//     opens, and sub_82236678 (the menu build) is the only consumer: with the
//     flag clear it clamps the menu's selectable-entry count to nine and greys
//     the entry past that, which is Save, the menu's last one.
//     EternalSonataCanSave reports that gate and
//     EternalSonataSetSaveAlwaysAllowed forces it.
//
//   * sub_82241190(ctrl, slot) is the save. It gathers every live global the
//     save holds into the save controller's buffers, registers them with
//     sub_8223DB40, and hands the lot to sub_8223DF80, which spawns a content
//     worker thread running sub_8223DC58. It returns 0 if the job could not be
//     started at all. `slot` is 0..9 and is the NN of the "savecontentNN"
//     container the write lands in.
//
//   * sub_8223DC58 is that worker. It writes every registered buffer through
//     XamContentCreateEx and returns 1 on success, 0 on failure (also left in
//     dword_824409E8 as 1 or 2). It is generic - loads and other content jobs
//     run through it too - so a completion is only attributed to a save when
//     this file knows a save is outstanding.
//
// So the three events are: sub_82241190 entry (started), its own zero return
// (failed, nothing was written), and the worker's result (completed/failed).
//
// Threading. The two hooks run on different guest threads - the save starts on
// whichever thread drove the menu and finishes on the spawned worker - so the
// small amount of state here is under a mutex, and the events are published
// from those threads rather than marshalled onto the frame tick. That is
// documented in eternalsonata_save_api.h; subscribers have to be thread-safe.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xam/content_manager.h>

#include "eternalsonata_save_api.h"
#include "save_system.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// "The party is standing at a save point." Written by the field scripts, read
// by sub_821E51B0 when the pause menu opens.
constexpr uint32_t kAtSavePointAddr = 0x8243C360u;
// The pause menu's own copy of it, latched on open. Read here as well as the
// source flag because a mod (save_anywhere) may be forcing only one of them,
// and either one being set means the menu offers Save.
constexpr uint32_t kMenuSaveEnabledAddr = 0x8243FBFBu;

// The game's own save-slot count; sub_82241190 and sub_822404E8 both refuse a
// slot above 9.
constexpr int kSlotCount = ETERNALSONATA_SAVE_SLOT_COUNT;

// Container naming. The game writes one content package per slot, named after
// the slot number, which is how a container on disk maps back to a slot.
constexpr std::string_view kContainerPrefix = "savecontent";

// XContentType::kSavedGame is 1, and the content manager files a type under a
// directory named after that value in hex.
constexpr std::string_view kSavedGameTypeDir = "00000001";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::mutex g_mutex;
rex::Runtime* g_runtime = nullptr;

// Set by EternalSonataSetSaveAlwaysAllowed. Read from the pause-menu-build
// hook on a guest thread, hence the atomic.
std::atomic<bool> g_force_save_allowed{false};

// Guarded by g_mutex.
bool g_save_running = false;
int g_save_slot = -1;       // slot of the save in flight, or of the last one
int g_last_slot = -1;       // slot of the last save this run
int g_state = ETERNALSONATA_SAVE_STATE_NONE;

rex::memory::Memory* Mem() {
  return g_runtime ? g_runtime->memory() : nullptr;
}

uint8_t ReadGuestByte(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  const auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : 0;
}

bool GuestReadable(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address) != rex::memory::PageAccess::kNoAccess;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback
// list of our own, so a mod subscribes by name with nothing linked. Called
// with g_mutex NOT held: a subscriber may call back into this file (e.g.
// EternalSonataGetSaveState from its handler), and Publish runs subscribers
// outside the registry's own lock too.
void PublishSaveEvent(const char* event_name, int slot, double reason) {
  rex::Runtime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    runtime = g_runtime;
  }
  if (!runtime) {
    return;
  }
  auto* registry = runtime->mod_registry();
  if (!registry) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = static_cast<uint64_t>(slot);
  payload.f64 = reason;
  registry->Publish(event_name, payload);
}

void OnSaveStarted(int slot) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_save_running = true;
    g_save_slot = slot;
    g_last_slot = slot;
    g_state = ETERNALSONATA_SAVE_STATE_RUNNING;
  }
  PublishSaveEvent(ETERNALSONATA_SAVE_EVENT_STARTED, slot, 0.0);
}

// `reason` is 0 for a success, or one of ETERNALSONATA_SAVE_FAIL_*. Returns
// without publishing anything if no save was outstanding, which is what makes
// the shared content worker's completions safe to hook: a load finishing while
// no save is in flight is not our event.
void OnSaveFinished(bool succeeded, double reason) {
  int slot = -1;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_save_running) {
      return;
    }
    g_save_running = false;
    slot = g_save_slot;
    g_state = succeeded ? ETERNALSONATA_SAVE_STATE_COMPLETED : ETERNALSONATA_SAVE_STATE_FAILED;
  }
  PublishSaveEvent(
      succeeded ? ETERNALSONATA_SAVE_EVENT_COMPLETED : ETERNALSONATA_SAVE_EVENT_FAILED, slot,
      reason);
}

// ---------------------------------------------------------------------------
// Save containers on disk
// ---------------------------------------------------------------------------

rex::system::xam::ContentManager* ContentManager() {
  auto* kernel_state = rex::system::kernel_state();
  return kernel_state ? kernel_state->content_manager() : nullptr;
}

uint64_t CurrentXuid() {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state) {
    return 0;
  }
  auto* profile = kernel_state->user_profile();
  return profile ? profile->xuid() : 0;
}

// Where this profile's save containers live: the directory holding one
// savecontentNN/ per slot.
//
// The layout is <user data root>/<profile>/<title id>/00000001/<container>/,
// but what the <profile> level is named - the profile's display name, its xuid
// in hex - is the content manager's own business and is not exposed, so that
// one level is searched rather than reconstructed. The current profile's name
// and xuid are tried first so a machine with several profiles still resolves
// to the right one; the scan is only the fallback.
//
// Note that ContentManager::ResolveGameUserContentPath is a different tree,
// <root>/<title id>/profile/<user name>, and holds no save containers.
std::filesystem::path SaveContainerRoot() {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state || !g_runtime) {
    return {};
  }
  std::filesystem::path root = g_runtime->user_data_root();
  if (root.empty()) {
    return {};
  }
  std::error_code ec;
  root = std::filesystem::absolute(root, ec);
  if (ec) {
    return {};
  }

  char title[16];
  std::snprintf(title, sizeof(title), "%08X", kernel_state->title_id());
  const std::filesystem::path tail =
      std::filesystem::path(title) / std::filesystem::path(kSavedGameTypeDir);

  auto* profile = kernel_state->user_profile();
  if (profile) {
    const std::string name = profile->name();
    if (!name.empty() && std::filesystem::is_directory(root / name / tail, ec)) {
      return root / name / tail;
    }
    char xuid[24];
    std::snprintf(xuid, sizeof(xuid), "%016llX",
                  static_cast<unsigned long long>(profile->xuid()));
    if (std::filesystem::is_directory(root / xuid / tail, ec)) {
      return root / xuid / tail;
    }
  }

  // No profile level at all, in case a host files saves straight under the
  // root.
  if (std::filesystem::is_directory(root / tail, ec)) {
    return root / tail;
  }

  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory(ec)) {
      continue;
    }
    std::filesystem::path candidate = entry.path() / tail;
    if (std::filesystem::is_directory(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

// "savecontent07" -> 7. -1 for anything that is not one of the game's own
// containers, so a stray directory is still listed but carries no slot.
int32_t SlotFromContainerName(std::string_view file_name) {
  if (file_name.size() != kContainerPrefix.size() + 2 ||
      file_name.compare(0, kContainerPrefix.size(), kContainerPrefix) != 0) {
    return -1;
  }
  const char tens = file_name[kContainerPrefix.size()];
  const char ones = file_name[kContainerPrefix.size() + 1];
  if (tens < '0' || tens > '9' || ones < '0' || ones > '9') {
    return -1;
  }
  const int32_t slot = (tens - '0') * 10 + (ones - '0');
  return slot < kSlotCount ? slot : -1;
}

// Total bytes and newest write time of every file in a container directory.
// Both are best effort: a container that cannot be walked reports zeros rather
// than failing the whole listing.
void MeasureContainer(const std::filesystem::path& dir, uint64_t* out_size,
                      uint64_t* out_modified) {
  *out_size = 0;
  *out_modified = 0;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const uintmax_t size = entry.file_size(ec);
    if (!ec) {
      *out_size += static_cast<uint64_t>(size);
    }
    const auto write_time = entry.last_write_time(ec);
    if (ec) {
      continue;
    }
    // file_time_type's epoch is unspecified, so the timestamp only becomes a
    // Unix one after a cast to the system clock.
    const auto system_time = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(write_time));
    const auto seconds = static_cast<uint64_t>(system_time.time_since_epoch().count());
    if (seconds > *out_modified) {
      *out_modified = seconds;
    }
  }
}

void CopyTruncated(char* dest, size_t dest_size, std::string_view source) {
  if (!dest || dest_size == 0) {
    return;
  }
  const size_t count = std::min(source.size(), dest_size - 1);
  std::memcpy(dest, source.data(), count);
  dest[count] = '\0';
}

// Every save container the current profile has, ordered by slot (unknown-slot
// containers last, in the order the content manager returned them).
std::vector<EternalSonataSaveSlot> CollectSaves() {
  std::vector<EternalSonataSaveSlot> saves;
  auto* content = ContentManager();
  if (!content) {
    return saves;
  }

  const uint64_t xuid = CurrentXuid();
  // The game selects the HDD (device 1) at boot, which is what its containers
  // are filed under; falling back to 0 covers a host that files them under
  // "any device" instead.
  auto listed = content->ListContent(1, xuid, rex::system::XContentType::kSavedGame);
  if (listed.empty()) {
    listed = content->ListContent(0, xuid, rex::system::XContentType::kSavedGame);
  }

  const std::filesystem::path dir = SaveContainerRoot();
  for (const auto& data : listed) {
    EternalSonataSaveSlot slot{};
    slot.struct_size = static_cast<uint32_t>(sizeof(EternalSonataSaveSlot));
    const std::string file_name = data.file_name();
    slot.slot = SlotFromContainerName(file_name);
    CopyTruncated(slot.file_name, sizeof(slot.file_name), file_name);
    CopyTruncated(slot.display_name, sizeof(slot.display_name),
                  rex::string::to_utf8(data.display_name()));
    if (!dir.empty()) {
      MeasureContainer(dir / file_name, &slot.size_bytes, &slot.modified_time);
    }
    saves.push_back(slot);
  }

  std::stable_sort(saves.begin(), saves.end(),
                   [](const EternalSonataSaveSlot& a, const EternalSonataSaveSlot& b) {
                     const int32_t lhs = a.slot < 0 ? kSlotCount : a.slot;
                     const int32_t rhs = b.slot < 0 ? kSlotCount : b.slot;
                     return lhs < rhs;
                   });
  return saves;
}

bool Bound() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_runtime != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindSaveSystem(rex::Runtime* runtime) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_runtime = runtime;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Guest hooks
// ---------------------------------------------------------------------------

// sub_82241190(ctrl, slot) -> bool: gather the live game state and start the
// write. A zero return means the job never got off the ground, so nothing will
// reach the content worker and the failure has to be reported here.
REX_EXTERN(__imp__sub_82241190);
REX_HOOK_RAW(sub_82241190) {
  // The original clobbers r4, so take the slot before running it.
  const u32 slot = ctx.r4.u32;

  eternalsonata::OnSaveStarted(static_cast<int>(slot));
  __imp__sub_82241190(ctx, base);

  if (!(ctx.r3.u32 & 0xFF)) {
    eternalsonata::OnSaveFinished(false, ETERNALSONATA_SAVE_FAIL_NOT_STARTED);
  }
}

// sub_82236678(root, page) -> void: the pause menu build, and the only thing
// that reads the save gate:
//
//     if (!byte_8243FBFB) {
//       sub_822004E0(root, 0, 9);                 // nine selectable entries
//       sub_82179160(&gfx, obj[132], -6710887);   // and the last one greyed
//     }
//
// The flags are rewritten by the field scripts on their own schedule, so this
// is the one place a forced gate can be set without racing them.
REX_EXTERN(__imp__sub_82236678);
REX_HOOK_RAW(sub_82236678) {
  if (eternalsonata::g_force_save_allowed.load(std::memory_order_relaxed)) {
    // The menu's own copy, which is what the build below reads, and the flag
    // it was latched from, so anything else that consults the save-point state
    // during this menu agrees with it.
    REX_STORE_U8(eternalsonata::kMenuSaveEnabledAddr, 1);
    REX_STORE_U8(eternalsonata::kAtSavePointAddr, 1);
  }
  __imp__sub_82236678(ctx, base);
}

// sub_8223DC58(request) -> bool: the content worker body, on its own guest
// thread. Generic across content jobs; OnSaveFinished ignores it unless a save
// is actually outstanding.
REX_EXTERN(__imp__sub_8223DC58);
REX_HOOK_RAW(sub_8223DC58) {
  __imp__sub_8223DC58(ctx, base);

  const bool succeeded = (ctx.r3.u32 & 0xFF) != 0;
  eternalsonata::OnSaveFinished(succeeded, succeeded ? 0.0 : ETERNALSONATA_SAVE_FAIL_WRITE);
}

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_save_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataSaveAbiVersion(void) {
  return ETERNALSONATA_SAVE_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCanSave(void) {
  using namespace eternalsonata;
  if (!Bound() || !GuestReadable(kAtSavePointAddr)) {
    return ETERNALSONATA_SAVE_ERR_UNAVAILABLE;
  }
  // With the gate forced the menu offers Save wherever the player is, whatever
  // the guest flags happen to read as between menus.
  if (g_force_save_allowed.load(std::memory_order_relaxed)) {
    return 1;
  }
  return (ReadGuestByte(kAtSavePointAddr) || ReadGuestByte(kMenuSaveEnabledAddr)) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetSaveAlwaysAllowed(int enabled) {
  using namespace eternalsonata;
  if (!Bound()) {
    return ETERNALSONATA_SAVE_ERR_UNAVAILABLE;
  }
  g_force_save_allowed.store(enabled != 0, std::memory_order_relaxed);
  return ETERNALSONATA_SAVE_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsSaveAlwaysAllowed(void) {
  using namespace eternalsonata;
  return g_force_save_allowed.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsSaveInProgress(void) {
  using namespace eternalsonata;
  if (!Bound()) {
    return ETERNALSONATA_SAVE_ERR_UNAVAILABLE;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_save_running ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetSaveState(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetLastSaveSlot(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_last_slot;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetSaveSlotCount(void) {
  return ETERNALSONATA_SAVE_SLOT_COUNT;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataListSaves(EternalSonataSaveSlot* out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_SAVE_ERR_INVALID_ARGUMENT;
  }
  if (!Bound() || !ContentManager()) {
    return ETERNALSONATA_SAVE_ERR_UNAVAILABLE;
  }

  std::vector<EternalSonataSaveSlot> saves;
  try {
    saves = CollectSaves();
  } catch (const std::exception&) {
    return ETERNALSONATA_SAVE_ERR_IO;
  }

  // Sizing call: how many there are, without writing anything.
  if (!out || max == 0) {
    return static_cast<int>(saves.size());
  }

  const int count = std::min(static_cast<int>(saves.size()), max);
  for (int i = 0; i < count; ++i) {
    out[i] = saves[static_cast<size_t>(i)];
  }
  return count;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetSaveDirectory(char* out, int max) {
  using namespace eternalsonata;
  if (!out || max <= 0) {
    return ETERNALSONATA_SAVE_ERR_INVALID_ARGUMENT;
  }
  if (!Bound() || !ContentManager()) {
    return ETERNALSONATA_SAVE_ERR_UNAVAILABLE;
  }

  const std::filesystem::path dir = SaveContainerRoot();
  if (dir.empty()) {
    return ETERNALSONATA_SAVE_ERR_IO;
  }
  const std::string utf8 = rex::path_to_utf8(dir);
  if (static_cast<int>(utf8.size()) + 1 > max) {
    return ETERNALSONATA_SAVE_ERR_INVALID_ARGUMENT;
  }
  std::memcpy(out, utf8.data(), utf8.size());
  out[utf8.size()] = '\0';
  return static_cast<int>(utf8.size());
}
