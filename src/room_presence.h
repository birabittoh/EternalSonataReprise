// eternalsonata - Discord Rich Presence: show the area the player is
// currently in, translated from the game's own current-area id.
//
// Two guest/host sources feed the current-area id, chosen in this order:
//   * if a field map is loaded (the host-side capture below has fired and has
//     not been torn down since), that capture is authoritative. Field play and
//     in-field cutscenes both keep the map loaded; the loaders never write
//     byte_8244B500.
//   * otherwise byte_8244B500 (written by the generic cfdata loader
//     sub_820FCC80, i.e. the menu/event path: "E%04d.e" etc.), read straight
//     out of live guest memory. Non-empty means title/menu/event screens.
//   * if neither is set, the game is mid-transition (previous map torn down,
//     next not loaded yet); fall back to the host-side capture so the last
//     known field area stays up instead of "Title Screen".
// The host-side capture itself comes from the field-area loaders
// sub_820FAFB0 / sub_820FB420 (the map loaders that receive "cfdata\XXyy.e"
// filenames; see eternalsonata_hooks.cpp "Field area tracking"). Those
// loaders also fire for speculative gate preloads as the player approaches a
// transition, so the hook filters on the loaders' r5 flags word and forwards
// only real transitions -- see eternalsonata_hooks.cpp for details.
//
// Ids are normalized (lowercase, ".e" stripped) and translated through the
// static table generated from the cfdata BTX files. Event/scene ids ("e%04d")
// map to "In Main Menu", and nothing loaded yet to "Loading...". Changes are
// pushed to rex::discord_rpc's SetDetails so the SDK's own worker thread does
// the actual Discord IPC.
//
// The presence's second row (state) qualifies the first: a battle is
// "Fighting...", a loaded field is "Exploring...", and outside a field it is
// left empty, because the first row already says everything there is to say
// ("Loading..." / "In Main Menu") and repeating it just prints the same text
// twice. While a state row is shown it is suffixed with the current party
// level read from byte_8243FBFC (" Group Lv. N"). Battle is tracked
// separately because the field stays loaded underneath one, so "is a field
// loaded" cannot tell the two apart. See room_presence.cpp for why the game's
// scene-mode register drives only the *end* of a battle and nothing else.
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

  // Called from the guest-thread map-dispatcher hook (sub_820FD998 with a null
  // name, see eternalsonata_hooks.cpp) when the game tears the field map down.
  // Clears the "a field is loaded" flag that NotifyAreaLoad sets, so the state
  // row can leave "Exploring..." when the player returns to a menu screen.
  void NotifyFieldTeardown();

  // Called from the guest-thread battle-entry hook (sub_820FDB80, see
  // eternalsonata_hooks.cpp), whose last act is an unconditional request for
  // scene mode 4. Marks the battle immediately; Tick() ends it again when the
  // applied scene mode reaches 4, which is the end-of-battle edge.
  void NotifyBattleStart();

  // Ends the battle. Called from NotifyAreaLoad as a backstop: a real area
  // transition cannot happen mid-battle, so walking into the next area clears
  // a battle whose end Tick() somehow missed.
  void NotifyBattleEnd();

 private:
  rex::system::KernelState* kernel_state_ = nullptr;

  // Last field-area id captured by NotifyAreaLoad, guarded by area_mutex_.
  std::mutex area_mutex_;
  std::string field_area_id_;
  // Whether a field map is currently loaded: set by NotifyAreaLoad, cleared by
  // NotifyFieldTeardown. Used in place of the game's own map-region buffer,
  // which is not reliably populated -- see room_presence.cpp.
  bool field_active_ = false;
  // Whether a battle is running: set by NotifyBattleStart, cleared by
  // NotifyBattleEnd. Not derivable from the scene mode -- see room_presence.cpp
  // for why the mode register reads "field" for a battle's whole duration.
  bool battle_active_ = false;

  std::string last_area_id_;
  std::string last_state_;
  bool has_read_area_once_ = false;
};

// Process-wide instance shared between the app hooks.
RoomPresence& GetRoomPresence();

}  // namespace eternalsonata
