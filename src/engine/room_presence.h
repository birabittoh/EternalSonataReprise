// eternalsonata: Discord Rich Presence.
//
// The details row names the current area. While a field is loaded its id
// comes from the field loader hooks (eternalsonata_presence.cpp); otherwise
// from the menu/event id at byte_8244B500. Ids are translated through the
// table generated from the cfdata BTX files.
//
// The state row is "Fighting...", "Watching a cutscene..." or "Exploring..."
// followed by the party level, and is empty otherwise, where the details row
// ("Loading..." / "In Main Menu") already says it all.
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

struct AreaDescription {
  std::string id;
  std::string name;
};

class RoomPresence {
 public:
  RoomPresence() = default;

  // Starts Discord RPC and registers the per-frame Tick(). Call from
  // OnPostSetup.
  void Bind(rex::system::KernelState* kernel_state, rex::Runtime* runtime);

  // Pushes the presence to Discord when it changed. No-op until bound.
  void Tick();

  // Called from the field loader hooks with the area id ("ktm01.e").
  // Thread-safe.
  void NotifyAreaLoad(const char* area_id);

  // Called when the field map is torn down (sub_820FD998 with a null name).
  void NotifyFieldTeardown();

  // Read live from the battle FSM (FsmStateIsInBattle), so it is stateless
  // and safe from any thread. Exported to mods as EternalSonataIsBattleActive.
  bool IsBattleActive();

  // Canonical id and display name shared by presence and overworld events.
  AreaDescription CurrentArea();
  AreaDescription DescribeArea(std::string area_id) const;

  // True when object is the live player-controlled field object.
  bool IsFieldLeader(uint32_t object) const;
  bool IsFieldActive();

 private:
  rex::system::KernelState* kernel_state_ = nullptr;

  std::mutex area_mutex_;
  std::string field_area_id_;
  // Guarded by area_mutex_.
  bool field_active_ = false;

  std::string last_details_;
  std::string last_state_;
  bool has_read_area_once_ = false;
};

// Process-wide instance shared between the app hooks.
RoomPresence& GetRoomPresence();

}  // namespace eternalsonata
