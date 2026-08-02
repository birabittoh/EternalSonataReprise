#include "room_presence.h"

#include <cctype>
#include <cstring>
#include <string>

#include <rex/discord_rpc.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>

#include "area_names.generated.h"

namespace eternalsonata {

namespace {

// Discord Application ID (discord.com/developers/applications, "Eternal
// Sonata: Reprise").
constexpr char kDiscordClientId[] = "1420820611953066076";

// Guest virtual address of the "menu/event cfdata id" string
// (byte_8244B500): the id the game's generic cfdata loader (sub_820FCC80)
// copies in each time it builds "cfdata\XXyy.e" and loads the file. Only the
// menu/event path writes it. Vanilla image only -- unlike the Nocturne
// player-stats struct, this is static data, so no heap guard is needed, but
// it hasn't been re-derived for a TU build.
constexpr uint32_t kAreaIdGuestAddress = 0x8244B500;
// Longest area id ("e3120_120") is well under this.
constexpr uint32_t kAreaIdMaxLength = 32;

// Guest virtual address of the map-region buffer (map manager 0x824D0480 +
// 0x2CBB0): the single writer-verified "which field map is loaded" copy.
// Only switches on a real map transition, funneled through the game's own
// dispatcher, which strcmp's against this buffer and no-ops if unchanged.
// Lowercased filename with its .bop extension ("ktm90.bop"); empty/all-zero
// means no field map is currently loaded (title/menu screens). This is what
// gates which area-id source Tick() trusts.
constexpr uint32_t kMapRegionGuestAddress = 0x824FD030;
constexpr uint32_t kMapRegionMaxLength = 128;

// Guest virtual address of the scene-mode byte (byte 3 of dword_824C74C8,
// the map/field manager's mode register). Polling it produces a noticeable
// lag on state transitions and can get stuck showing a stale mode after
// battle ends; unresolved.
//   4 = battle active, 3 = field/overworld, 5 = menu/event screen,
//   2 = loading/init, 0 = nothing loaded (boot).
constexpr uint32_t kSceneModeGuestAddress = 0x824C74CB;

// Last scene-mode byte seen by Tick().
uint8_t g_last_scene_mode = 0xFF;

std::string ReadGuestCString(rex::memory::Memory* memory, uint32_t guest_address,
                             size_t max_len) {
  const char* host_address = memory->TranslateVirtual<const char*>(guest_address);
  size_t len = strnlen(host_address, max_len);
  return std::string(host_address, len);
}

uint8_t ReadGuestU8(rex::memory::Memory* memory, uint32_t guest_address) {
  return *memory->TranslateVirtual<const uint8_t*>(guest_address);
}

// Fold an area id to its canonical table key: strip a trailing ".e" extension
// (the map loaders pass "cfdata\<id>.e" filenames) and lowercase the rest.
// The loaders are not consistent about case -- the same area arrives as both
// "kts01.e" and "KTM04.e" -- while AreaNameTable() is keyed lowercase.
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

  rex::discord_rpc::Presence initial;
  initial.details_ = "Playing Eternal Sonata";
  initial.large_image_key_ = "icon";
  initial.large_image_text_ = "Eternal Sonata: Reprise";
  rex::discord_rpc::Start(kDiscordClientId, initial);

  runtime->mod_registry()->RegisterTick([this] { Tick(); });
}

void RoomPresence::Tick() {
  if (!kernel_state_ || !kernel_state_->memory()) {
    return;
  }
  auto* memory = kernel_state_->memory();

  // Which area-id source is authoritative depends on whether a field map is
  // loaded. 0x824FD030 is the single writer-verified "which map is loaded"
  // copy: non-empty (e.g. "ktm90.bop") means the player is inside a field map -- including in-field cutscenes, which keep the map
  // loaded -- and the area id captured by the map-loader hooks is
  // authoritative (the loaders never write byte_8244B500). Empty means the
  // map was torn down; then byte_8244B500 tells us whether we are on a
  // menu/event screen (sub_820FCC80 wrote it, "E%04d.e" and friends). If
  // neither buffer is set, the game is in the middle of a transition/loading
  // (previous map down, next not up yet), so keep showing the last known
  // field area rather than "Title Screen".
  std::string map_region = ReadGuestCString(memory, kMapRegionGuestAddress, kMapRegionMaxLength);

  // Scene-mode byte, logged on change so state-row behavior stays checkable in
  // logs/eternalsonata_*.log (the Discord overlay only shows the resulting
  // label; the raw value is useful when the mapping needs tuning).
  uint8_t scene_mode = ReadGuestU8(memory, kSceneModeGuestAddress);
  if (scene_mode != g_last_scene_mode) {
    g_last_scene_mode = scene_mode;
    REXLOG_INFO("[presence] mode={} map='{}'", scene_mode, map_region);
  }

  std::string area_id;
  if (!map_region.empty()) {
    std::lock_guard<std::mutex> lock(area_mutex_);
    area_id = field_area_id_;
  } else {
    std::string menu_id = ReadGuestCString(memory, kAreaIdGuestAddress, kAreaIdMaxLength);
    if (!menu_id.empty()) {
      area_id = std::move(menu_id);
    } else {
      std::lock_guard<std::mutex> lock(area_mutex_);
      area_id = field_area_id_;
    }
  }
  area_id = NormalizeAreaId(std::move(area_id));

  // Second row (state): the scene-mode byte is the only signal that can tell
  // "In Battle" from "Overworld", because the map-region buffer stays loaded
  // through a battle. Runtime-verified modes: 4 = battle, 3 = field, 5 =
  // menu/event, 2 = loading, 0 = nothing loaded. Field play keeps the map
  // loaded (including in-field cutscenes), so gate "Overworld" on that rather
  // than on mode 3 -- the byte only dips to 3 during transitions.
  std::string state;
  if (scene_mode == 4) {
    state = "In Battle";
  } else if (!map_region.empty()) {
    state = "Overworld";
  } else if (scene_mode == 5) {
    state = "In Main Menu";
  } else {
    state = "Starting...";
  }

  // Battle entry does not change the area id (same map stays loaded), so the
  // state row needs its own change detection; the details row keeps its.
  bool area_changed = !has_read_area_once_ || area_id != last_area_id_;
  bool state_changed = !has_read_area_once_ || state != last_state_;
  if (!area_changed && !state_changed) {
    return;
  }
  has_read_area_once_ = true;

  if (state_changed) {
    last_state_ = state;
    rex::discord_rpc::SetState(state);
  }
  if (!area_changed) {
    return;
  }
  last_area_id_ = area_id;

  if (area_id.empty()) {
    // Nothing loaded yet: boot logos, before the title event (E%04d.e)
    // populates byte_8244B500. The actual title screen and its menus are
    // labeled by the event-file branch below.
    rex::discord_rpc::SetDetails("Starting...");
    return;
  }

  // Event/scene file (the "E%04d.e" family): these carry no banner of their
  // own and are only ever chosen from the menu/event branch above (the field
  // loaders only pass field ids to the capture), so this effectively maps the
  // title screen and its menus to a stable label. No table entry ever matches
  // this pattern.
  if (area_id.size() >= 2 && area_id[0] == 'e' &&
      std::isdigit(static_cast<unsigned char>(area_id[1]))) {
    rex::discord_rpc::SetDetails("Main Menu");
    return;
  }

  const auto& table = AreaNameTable();
  auto it = table.find(area_id);
  if (it != table.end()) {
    rex::discord_rpc::SetDetails(it->second);
  } else {
    // Known-but-unnamed id: an event/support file (cutscene E%04d.e, zzz02,
    // *60, ...) that carries no banner of its own. Show the raw id.
    rex::discord_rpc::SetDetails(area_id);
  }
}

void RoomPresence::NotifyAreaLoad(const char* area_id) {
  std::lock_guard<std::mutex> lock(area_mutex_);
  std::string id = area_id ? area_id : "";
  if (id == field_area_id_) {
    return;
  }
  field_area_id_ = std::move(id);
}

RoomPresence& GetRoomPresence() {
  static RoomPresence instance;
  return instance;
}

}  // namespace eternalsonata
