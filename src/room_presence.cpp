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

namespace eternalsonata {

namespace {

// Discord Application ID (discord.com/developers/applications, "Eternal
// Sonata: Reprise").
#if REX_PLATFORM_WIN32 || REX_PLATFORM_GNU_LINUX
constexpr char kDiscordClientId[] = "1420820611953066076";
#endif

// Guest virtual address of the "menu/event cfdata id" string
// (byte_8244B500): the id the game's generic cfdata loader (sub_820FCC80)
// copies in each time it builds "cfdata\XXyy.e" and loads the file. Only the
// menu/event path writes it. Vanilla image only -- unlike the Nocturne
// player-stats struct, this is static data, so no heap guard is needed, but
// it hasn't been re-derived for a TU build.
constexpr uint32_t kAreaIdGuestAddress = 0x8244B500;
// Longest area id ("e3120_120") is well under this.
constexpr uint32_t kAreaIdMaxLength = 32;

// The map-region buffer at 0x824FD030 (map manager 0x824D0480 + 0x2CBB0) is
// the game's own "which field map is loaded" copy, and looks like the obvious
// way to tell "a field is loaded" -- but it is not usable here and is
// deliberately not read. Session-log-verified: it stayed empty for the whole
// of Tenuto Village while the loader hooks were concurrently reporting real
// area loads there, and only became non-empty ("ktm90.bop") out on the field.
// Its sole writer, sub_8219FF58, returns early unless the SYSN singleton
// (dword_8244B9A0) is present, which is the likely reason. field_active_,
// driven by the loader hooks, is used instead.

// The game's scene mode is a request/apply pair of adjacent globals, not a
// single register:
//
//   dword_824C74C4  requested mode  \  written together, as a unit, by every
//   dword_824C74CC  request pending /  real transition
//   dword_824C74C8  applied mode
//
// Every transition writes the new mode into C4 and sets CC to 1
// (sub_820FDB80 battle entry, sub_820FDFC0 field load, sub_820E7B38
// event/menu start, and the battle-end writers). Confirmed by IDA: *nothing*
// in default.xex writes C8 through a direct reference except the
// initialize-to-zero in sub_822E0540 -- every other reference to C8 reads it,
// mostly as `C4 = C8` (re-request the current mode after a failure). C8 is
// applied indirectly, later, by whatever consumes the pending flag.
//
// Mode values (runtime-verified): 4 = battle active, 3 = field/overworld,
// 5 = menu/event screen, 2 = loading/init, 0 = no request pending.
//
// C4 cannot be polled, which is why the transitions below are hooks. It is
// written as a plain `stw` of a small int (0x820FDF94: `li r10, 4` / `stw r10,
// C4`), but is cleared again as soon as the request is consumed -- faster than
// one rendered frame, and Tick() runs on the host swap. Session-log-verified
// over two full runs: every sampled tick read 0, including across transitions
// that demonstrably happened.
//
// C8 does not mean what its documented "4 = battle active" reading suggests.
// Session-log-verified across three battles in two runs: for a battle's whole
// duration it reads 3 (field), and it turns 4 only as the battle *ends* --
// consistently ~50s after the entry hook, immediately before the post-battle
// screen.
//
// None of this is read any more. Battle state used to be a latch: set by a
// hook on the battle-entry function sub_820FDB80, cleared by Tick() when the
// applied mode reached 4. That was correct for exactly one battle per session.
// Whichever half of it failed to re-arm, the shape of the bug was inherent:
// two unrelated edges, on two different threads, had to keep a bool in step
// with a state neither of them owned, and nothing resynced it if they ever
// disagreed. It is now derived from the battle FSM instead, which cannot drift
// because it is the thing being asked about; see FsmStateIsInBattle in
// battle_layout.h for why the state field is readable between battles too.

// Guest virtual address of the party level (low byte of the big-endian
// dword_8243F3EC, same convention as kSceneModeAppliedGuestAddress above).
// New game and chapter init write 1 (sub_820FDFC0 and its duplicates).
// Identified via sub_82228410, a party-rank UI draw routine with a
// fallthrough switch (case 1, 2, 3, 4, 5) on dword_8243F3EC that lights up
// one more rank icon per case -- the classic "N-star rating" pattern for a
// Party Level display. Live-verified 2026-08-04: reads 2 while the in-game
// X-button status screen shows "Party Level 2", no offset needed. (The
// earlier byte_8243FBFC guess was wrong -- its own logic only ever handles
// 3 states, but Party Level goes 1-6.)
constexpr uint32_t kPartyLevelGuestAddress = 0x8243F3EF;

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
// The loaders are not consistent about case while AreaNameTable() is keyed lowercase.
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

  // The SDK only builds the Discord RPC implementation for Windows and GNU
  // Linux (linux/CMakeLists gated on those two); on other platforms the
  // header still declares the API but nothing links, so skip it.
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
  // No Discord RPC implementation on this platform; nothing to keep in sync
  // (rex::discord_rpc is header-only declared, never linked).
  return;
#endif

  if (!kernel_state_ || !kernel_state_->memory()) {
    return;
  }
  auto* memory = kernel_state_->memory();

  const bool battle_active = IsBattleActive();
  bool field_active;
  {
    std::lock_guard<std::mutex> lock(area_mutex_);
    field_active = field_active_;
  }

  // Which area-id source is authoritative depends on whether a field map is
  // loaded. While one is, the id captured by the map-loader hooks wins (the
  // loaders never write byte_8244B500), including through in-field cutscenes,
  // which keep the field loaded. Otherwise byte_8244B500 tells us whether we
  // are on a menu/event screen (sub_820FCC80 wrote it, "E%04d.e" and
  // friends). If neither is set the game is mid-transition, so keep showing
  // the last known field area rather than falling back to "Title Screen".
  std::string area_id;
  std::string menu_id_debug;
  if (field_active) {
    std::lock_guard<std::mutex> lock(area_mutex_);
    area_id = field_area_id_;
  } else {
    std::string menu_id = ReadGuestCString(memory, kAreaIdGuestAddress, kAreaIdMaxLength);
    menu_id_debug = menu_id;
    if (!menu_id.empty()) {
      area_id = std::move(menu_id);
    } else {
      std::lock_guard<std::mutex> lock(area_mutex_);
      area_id = field_area_id_;
    }
  }
  area_id = NormalizeAreaId(std::move(area_id));

  // Event/scene file (the "E%04d.e" family): these carry no banner of their
  // own and are only ever reached from the menu/event branch above (the field
  // loaders only pass field ids to the capture), so this effectively
  // identifies the title screen and its menus. No table entry ever matches
  // this pattern.
  const bool is_event_area = area_id.size() >= 2 && area_id[0] == 'e' &&
                             std::isdigit(static_cast<unsigned char>(area_id[1]));

  // Second row (state). The battle flag wins: a battle keeps the field loaded
  // under it, so field_active stays true throughout and cannot distinguish the
  // two on its own. Otherwise a loaded field is "Exploring..." (including
  // in-field cutscenes, which the game runs at mode 5 without tearing the
  // field down -- another reason not to key this off the mode).
  //
  // Outside a field there is no second-row information the first row does not
  // already carry ("Loading..." / "In Main Menu"), so leave it empty rather
  // than restating it. rex::discord_rpc omits empty fields from the activity
  // payload entirely, which is what actually clears the row in the overlay.
  std::string state;
  if (battle_active) {
    state = "Fighting...";
  } else if (field_active) {
    state = "Exploring...";
  }
  if (!state.empty()) {
    // Append the party level (dword_8243F3EC, see kPartyLevelGuestAddress)
    // so the state row reads e.g. "Exploring... Party Lv. 2". Tick()
    // re-reads it every frame, so a level change shows up through the same
    // state_changed detection as the base text.
    const uint8_t party_level = ReadGuestU8(memory, kPartyLevelGuestAddress);
    state += " Party Lv. " + std::to_string(party_level);
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
    // Nothing loaded yet: the boot logos, before the title event (E%04d.e)
    // populates byte_8244B500. The actual title screen and its menus are
    // labeled by the event-file branch below.
    rex::discord_rpc::SetDetails("Loading...");
    return;
  }

  if (is_event_area) {
    rex::discord_rpc::SetDetails("In Main Menu");
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
  field_active_ = true;
  std::string id = area_id ? area_id : "";
  if (id == field_area_id_) {
    return;
  }
  field_area_id_ = std::move(id);
}

void RoomPresence::NotifyFieldTeardown() {
  std::lock_guard<std::mutex> lock(area_mutex_);
  // field_area_id_ is deliberately kept: the details row falls back to the
  // last known field area during a transition, rather than flashing
  // "Starting..." between the teardown and the next area load.
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

RoomPresence& GetRoomPresence() {
  static RoomPresence instance;
  return instance;
}

}  // namespace eternalsonata

// Exported so mod DLLs (loaded into this same process) can query battle
// state via GetProcAddress(GetModuleHandle(nullptr), ...) instead of
// guessing at guest memory. See room_presence.h -- battle state is not
// reliably derivable from guest memory alone (the scene-mode register only
// exposes the *end* of a battle, not the whole span), which is exactly the
// mistake the party_overlay mod made before this was added. It is declared
// alongside the rest of the battle surface in src/eternalsonata_battle_api.h.
//
// Returns int rather than bool so the C ABI a mod resolves it through is
// unambiguous: a bool return only defines the low byte of the return register,
// so a caller that declared it as int could read whatever was left in the rest.
extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsBattleActive() {
  return eternalsonata::GetRoomPresence().IsBattleActive() ? 1 : 0;
}
