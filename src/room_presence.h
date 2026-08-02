// eternalsonata - Discord Rich Presence: show the area the player is
// currently in, translated from the game's own current-area id.
//
// Three guest/host sources feed the current-area id, chosen in this order:
//   * 0x824FD030 (the map-region buffer) non-empty (e.g. "ktm90.bop") means
//     a field map is loaded, so the host-side capture is authoritative.
//     Field play and in-field cutscenes both keep the map loaded; the
//     loaders never write byte_8244B500.
//   * otherwise byte_8244B500 (written by the generic cfdata loader
//     sub_820FCC80, i.e. the menu/event path: "E%04d.e" etc.), read straight
//     out of live guest memory. Non-empty means title/menu/event screens.
//   * if neither is set, the game is mid-transition (previous map torn down,
//     next not loaded yet); fall back to the host-side capture so the last
//     known field area stays up instead of "Title Screen".
// The host-side capture itself comes from the field-area loaders
// sub_820FAFB0 / sub_820FB420 (the map loaders that receive "cfdata\XXyy.e"
// filenames; see eternalsonata_hooks.cpp "Field area tracking"). Those
// loaders also fire for speculative gate preloads, so NotifyAreaLoad rejects
// any id whose family (first two characters) doesn't match the currently
// loaded map region -- see its definition for details.
//
// Ids are normalized (lowercase, ".e" stripped) and translated through the
// static table generated from the cfdata BTX files. Event/scene ids
// ("e%04d") map to "Main Menu". Changes are pushed to rex::discord_rpc's
// SetDetails so the SDK's own worker thread does the actual Discord IPC.
//
// The presence's second row (state) mirrors the game's scene-mode byte
// (0x824C74CB): 4 = "In Battle", field map loaded = "Overworld", 5 = "In
// Main Menu", else "Starting...". The mode byte is needed because the
// map-region buffer stays loaded through a battle, so it cannot tell battle
// from field on its own. Polling this byte is unreliable -- it lags state
// transitions and can get stuck on a stale mode after battle ends; see
// Tick() in room_presence.cpp for the current (broken) implementation.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace rex {
class Runtime;
namespace system {
class KernelState;
}  // namespace system
}  // namespace rex

namespace eternalsonata {

class RoomPresence {
 public:
  RoomPresence() = default;

  // Starts Discord RPC (rex::discord_rpc::Start) and registers a per-guest-
  // frame tick that keeps the presence's details line in sync with the
  // player's current area. Call once KernelState and the runtime are both
  // live (OnPostSetup).
  void Bind(rex::system::KernelState* kernel_state, rex::Runtime* runtime);

  // Re-reads the current area id and, if it changed, updates the Discord
  // presence. Registered as a guest-frame tick by Bind(); safe to call before
  // Bind() (no-op until bound).
  void Tick();

  // Called from the guest-thread field-loader hook (sub_820FAFB0, see
  // eternalsonata_hooks.cpp) with the area id ("ktm01.e"-style filename, or
  // empty) each time the game loads a field area. Records it for the next
  // Tick(). Thread-safe: the hook runs on a guest thread while Tick may run
  // on another.
  void NotifyAreaLoad(const char* area_id);

 private:
  rex::system::KernelState* kernel_state_ = nullptr;

  // Last field-area id captured by NotifyAreaLoad, guarded by area_mutex_.
  std::mutex area_mutex_;
  std::string field_area_id_;

  std::string last_area_id_;
  std::string last_state_;
  bool has_read_area_once_ = false;
};

// Process-wide instance shared between the app hooks.
RoomPresence& GetRoomPresence();

}  // namespace eternalsonata
