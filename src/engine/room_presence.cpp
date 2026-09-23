#include "room_presence.h"

#include <cctype>
#include <cstring>
#include <string>

#include <rex/discord_rpc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "area_names.generated.h"
#include "battle_layout.h"
#include "cutscene_system.h"

namespace eternalsonata {

namespace {

// Discord Application ID (discord.com/developers/applications, "Eternal
// Sonata: Reprise").
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
constexpr char kDiscordClientId[] = "1420820611953066076";
#endif

// Area id written by the generic cfdata loader (sub_820FCC80). Only the
// menu/event path writes it; field loads go through the hooked loaders.
constexpr uint32_t kAreaIdGuestAddress = 0x8244B500;
constexpr uint32_t kAreaIdMaxLength = 32;

// The map-region buffer at 0x824FD030 would be the obvious "field loaded"
// flag, but it stays empty in some fields (e.g. Tenuto Village), so the
// loader hooks drive field_active_ instead.

// Party level: low byte of the big-endian dword_8243F3EC, as used by the
// party-rank UI draw sub_82228410. Live-verified against the status screen.
constexpr uint32_t kPartyLevelGuestAddress = 0x8243F3EF;
constexpr uint32_t kMapManagerGuestAddress = 0x8244B4B0u;
constexpr uint32_t kFieldLeaderOffset = 1520u;

std::string ReadGuestCString(rex::memory::Memory* memory, uint32_t guest_address,
                             size_t max_len) {
  const char* host_address = memory->TranslateVirtual<const char*>(guest_address);
  size_t len = strnlen(host_address, max_len);
  return std::string(host_address, len);
}

uint8_t ReadGuestU8(rex::memory::Memory* memory, uint32_t guest_address) {
  return *memory->TranslateVirtual<const uint8_t*>(guest_address);
}

// Canonical table key: no ".e" suffix, lowercase, as AreaNameTable() is keyed.
std::string NormalizeAreaId(std::string id) {
  if (id.size() > 3 && id.compare(id.size() - 2, 2, ".e") == 0) {
    id.resize(id.size() - 2);
  }
  for (char& c : id) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return id;
}

}  // namespace

void RoomPresence::Bind(rex::system::KernelState* kernel_state, rex::Runtime* runtime) {
  kernel_state_ = kernel_state;

  // Discord RPC is only built for Windows and GNU Linux.
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
  rex::discord_rpc::Presence initial;
  initial.details_ = "Playing Eternal Sonata";
  initial.large_image_key_ = "icon";
  initial.large_image_text_ = "Eternal Sonata: Reprise";
  rex::discord_rpc::Start(kDiscordClientId, initial);
#endif

  runtime->mod_registry()->RegisterTick([this] { Tick(); });
}

void RoomPresence::Tick() {
#if !REX_PLATFORM_WIN32 && !REX_PLATFORM_GNU_LINUX
  return;
#endif

  if (!kernel_state_ || !kernel_state_->memory()) {
    return;
  }
  auto* memory = kernel_state_->memory();

  const bool battle_active = IsBattleActive();
  const bool cutscene_active = IsCutsceneActive();
  bool field_active;
  std::string field_area_id;
  {
    std::lock_guard<std::mutex> lock(area_mutex_);
    field_active = field_active_;
    field_area_id = field_area_id_;
  }

  // A loaded field, or a cutscene (some tear the field down and load their
  // E%04d file through the menu/event path), uses the last area captured by
  // the loader hooks. Otherwise the menu/event id wins, and if that is empty
  // too the game is mid transition, so the last known field area stays up.
  std::string area_id;
  if (field_active || (cutscene_active && !field_area_id.empty())) {
    area_id = std::move(field_area_id);
  } else {
    area_id = ReadGuestCString(memory, kAreaIdGuestAddress, kAreaIdMaxLength);
    if (area_id.empty()) {
      area_id = std::move(field_area_id);
    }
  }
  area_id = NormalizeAreaId(std::move(area_id));

  // An "E%04d" id only comes from the menu/event branch: the title screen and
  // its menus, or a cutscene played before any field was loaded.
  const bool is_event_area = area_id.size() >= 2 && area_id[0] == 'e' &&
                             std::isdigit(static_cast<unsigned char>(area_id[1]));

  std::string details;
  if (area_id.empty()) {
    // Boot logos, before the title event loads.
    details = "Loading...";
  } else if (is_event_area) {
    details = cutscene_active ? "Watching a cutscene" : "In Main Menu";
  } else {
    // Support files without a banner of their own show the raw id.
    const auto& table = AreaNameTable();
    const auto it = table.find(area_id);
    details = it != table.end() ? it->second : area_id;
  }

  // Battles keep the field loaded, so they are checked first. Outside a field
  // and a cutscene the row is left empty, which makes rex::discord_rpc drop
  // it from the overlay.
  std::string state;
  if (battle_active) {
    state = "Fighting...";
  } else if (cutscene_active) {
    state = "Watching a cutscene...";
  } else if (field_active) {
    state = "Exploring...";
  }
  if (!state.empty()) {
    const uint8_t party_level = ReadGuestU8(memory, kPartyLevelGuestAddress);
    state += " Party Lv. " + std::to_string(party_level);
  }

  if (!has_read_area_once_ || details != last_details_) {
    last_details_ = details;
    rex::discord_rpc::SetDetails(details);
  }
  if (!has_read_area_once_ || state != last_state_) {
    last_state_ = state;
    rex::discord_rpc::SetState(state);
  }
  has_read_area_once_ = true;
}

void RoomPresence::NotifyAreaLoad(const char* area_id) {
  std::lock_guard<std::mutex> lock(area_mutex_);
  field_active_ = true;
  std::string id = area_id ? area_id : "";
  if (id == field_area_id_) {
    return;
  }
  field_area_id_ = std::move(id);
}

void RoomPresence::NotifyFieldTeardown() {
  std::lock_guard<std::mutex> lock(area_mutex_);
  // field_area_id_ is kept so transitions keep showing the last area.
  field_active_ = false;
}

bool RoomPresence::IsBattleActive() {
  if (!kernel_state_ || !kernel_state_->memory()) {
    return false;
  }
  const auto* host = kernel_state_->memory()->TranslateVirtual<const uint8_t*>(
      battle::kManager + battle::kFsmStateOffset);
  if (!host) {
    return false;
  }
  return battle::FsmStateIsInBattle(rex::memory::load_and_swap<uint32_t>(host));
}

AreaDescription RoomPresence::DescribeArea(std::string area_id) const {
  AreaDescription result;
  result.id = NormalizeAreaId(std::move(area_id));
  const auto& table = AreaNameTable();
  const auto it = table.find(result.id);
  result.name = it != table.end() ? it->second : result.id;
  return result;
}

AreaDescription RoomPresence::CurrentArea() {
  std::lock_guard<std::mutex> lock(area_mutex_);
  return DescribeArea(field_area_id_);
}

bool RoomPresence::IsFieldLeader(uint32_t object) const {
  if (!kernel_state_ || !kernel_state_->memory()) {
    return false;
  }
  const auto* leader = kernel_state_->memory()->TranslateVirtual<const uint32_t*>(
      kMapManagerGuestAddress + kFieldLeaderOffset);
  return leader && rex::memory::load_and_swap<uint32_t>(leader) == object;
}

bool RoomPresence::IsFieldActive() {
  std::lock_guard<std::mutex> lock(area_mutex_);
  return field_active_;
}

RoomPresence& GetRoomPresence() {
  static RoomPresence instance;
  return instance;
}

}  // namespace eternalsonata

// Exported for mods; declared in eternalsonata_battle_api.h. Returns int
// because a bool return only defines the low byte of the return register.
extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsBattleActive() {
  return eternalsonata::GetRoomPresence().IsBattleActive() ? 1 : 0;
}
