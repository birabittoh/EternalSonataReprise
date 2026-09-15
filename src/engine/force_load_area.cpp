#include "force_load_area.h"

#include "guest_main_thread.h"

#include <cctype>
#include <rex/hook.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>

namespace {

// sub_820F9EC8 is the field resume routine. Its first act is to check
// byte_8243C368: if set, it takes a full map-reset branch that tears down the
// current field, loads a new area (from dword_8243C230), respawns the leader,
// and re-runs the transition sequence. That's the path we use for area warps:
// instead of calling the loader directly, we queue the target area id, set
// dword_8243C230 and byte_8243C368, and let the game run its own reset.
//
// This reuses the engine's ordering and state machine, avoiding the issues
// that came up when trying to force respawns directly (player unable to move,
// many attempted re-arm sequences all failed).
constexpr uint32_t kMapResetFlag = 0x8243C368u;
constexpr uint32_t kMapResetDword = 0x8243C230u;
constexpr uint32_t kMapManager = 0x8244B4B0u;

// Real transition flags: same value as a genuine area transition carries in r5
// (verified from session logs: real transitions had r5 = 0xA100 / 0xA001, while
// speculative preloads had r5 = 0x10).
constexpr uint32_t kRealTransitionFlags = 0xA100u;

// Area encoding is just the ASCII representation of the area id's 3-letter
// prefix + numeric suffix, packed as a big-endian dword. The reset sequence
// calls the loaders itself based on this value.

// Pending warp data, written by Request and read by the resume hook
std::mutex g_warp_mutex;
uint32_t g_warp_encoded_dword = 0;
bool g_warp_pending = false;

}  // namespace

namespace eternalsonata {

void ForceLoadArea::Bind(rex::Runtime* /*runtime*/) {
  // Nothing to register: requests are posted straight to the guest main
  // thread. Kept so the app's OnPostSetup wiring stays uniform with the other
  // debug tools.
}

void ForceLoadArea::Request(std::string area_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_area_id_ = std::move(area_id);
    has_pending_ = true;
  }
  PostToGuestMainThread([this] { Tick(); });
}

void ForceLoadArea::Tick() {
  std::string area_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_pending_) {
      return;
    }
    has_pending_ = false;
    area_id = std::move(pending_area_id_);
  }
  if (area_id.empty()) {
    return;
  }

  // Normalize: strip ".e" if present, uppercase, extract 3-letter prefix + numeric suffix
  std::string normalized = area_id;
  if (normalized.size() >= 2 && normalized.compare(normalized.size() - 2, 2, ".e") == 0) {
    normalized.erase(normalized.size() - 2);
  }
  for (auto& c : normalized) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }

  if (normalized.size() < 4) {
    return;
  }

  // Extract: first 3 chars are area letters, last 1-2 chars are numeric suffix
  // e.g. "HNO01" → H, N, O, 01; "FMT14" → F, M, T, 14
  uint8_t letter1 = static_cast<uint8_t>(normalized[0]);  // H, F, etc
  uint8_t letter2 = static_cast<uint8_t>(normalized[1]);  // N, M, etc
  uint8_t letter3 = static_cast<uint8_t>(normalized[2]);  // O, T, etc
  uint32_t number = 0;
  try {
    number = std::stoul(normalized.substr(3));
  } catch (...) {
    return;
  }

  if (number > 255) {
    return;
  }

  // Encode as dword: byte 3 = letter1, byte 2 = letter2, byte 1 = letter3, byte 0 = number
  // The game reads: HIWORD gets letter1|letter2, BYTE2 gets letter2, LOBYTE gets number
  uint32_t encoded_dword = (letter1 << 24) | (letter2 << 16) | (letter3 << 8) | number;

  // Queue the warp to be applied on next field resume (via hook)
  {
    std::lock_guard<std::mutex> lock(g_warp_mutex);
    g_warp_encoded_dword = encoded_dword;
    g_warp_pending = true;
  }
}

ForceLoadArea& GetForceLoadArea() {
  static ForceLoadArea instance;
  return instance;
}

// Expose warp data for the field_player_model_override hook to apply
std::mutex& GetWarpMutex() { return g_warp_mutex; }
uint32_t& GetWarpEncodedDword() { return g_warp_encoded_dword; }
bool& GetWarpPending() { return g_warp_pending; }

}  // namespace eternalsonata
