// eternalsonata - Battle state: reading it, and forcing a win.
//
// The public C ABI is eternalsonata_battle_api.h and the guest layout every
// address here comes from is battle_layout.h. The short version:
//
//   * Everything hangs off the battle manager singleton dword_824D0440. The
//     two sides are separate record arrays with their own strides, and the
//     live unit counts are two bytes at byte_824D0720 / byte_824D0721.
//   * A party record carries the character id (1..10) and a raw {max, cur} HP
//     pair. An enemy record carries neither: what it exposes is a cached
//     current/max HP *ratio*, which is the field every "is this enemy still
//     up" check in the game reads, including the battle-over predicate.
//   * Whose turn it is comes from the actor descriptor at manager+533120, a
//     {kind, slot} pair, and each unit has its own small FSM whose state says
//     whether an action is mid-resolution.
//
// What was NOT found: the raw current/max HP dwords inside an enemy record.
// Three separate rounds of decompiling every function that touches
// unk_82539240 (damage application, AI target selection, the reward/transition
// handler) turned up plenty of readers of enemy state but never a write site
// for a plain "HP -= damage" pair, so forcing a win does not zero a raw HP
// field the way EternalSonataSetCharacterStats does for the party
// (party_system.cpp, kStatHp/kStatHpMax on the 48-byte stride UI stats table;
// that table has no enemy equivalent, being populated by sub_821E7898 from the
// character stat table keyed by a 1..10 character id, which does not exist for
// enemies). Writing the cached ratio instead is sound because it is the same
// field the game's own battle-over predicate reads; battle_layout.h lists the
// confirmed readers.
//
// ---------------------------------------------------------------------------
// Why forcing a win waits for a party member's turn (2026-08-19)
//
// Zeroing the enemies during an enemy's turn used to softlock the battle;
// during the player's turn it always worked. The cause is that the game's own
// battle-over predicate, sub_821B7450, is asymmetric in whose turn it is: it
// only scans the enemies' HP when a *party member* is the acting unit. While
// an enemy holds the turn it asks only whether the party has been wiped.
//
// So zeroing every enemy mid enemy turn wedges it: the acting enemy can no
// longer finish, the actor never flips back to a party member, and the branch
// that would notice the enemies are dead is never evaluated. A logged repro
// confirmed it exactly, with the actor pinned at kind 1 and every enemy at
// hp 0.000 for 600 straight frames while the FSM sat in states 12 and 13.
//
// The request is therefore queued and re-checked once per guest frame, and the
// HP write only happens on a frame where a party member holds the turn and no
// action is mid-resolution, which is precisely the state the player's turn was
// already satisfying. That condition is also published as can_win_now so a UI
// can explain the wait instead of looking hung.
// ---------------------------------------------------------------------------
//
// Threading: the exported entry points are called from mods, i.e. usually from
// the ImGui draw thread, where there is no guest ThreadState and a guest call
// crashes the game (see guest_main_thread.h). Every reader here is a plain
// guest-memory load and answers immediately; the one mutation is queued onto
// the guest main thread.

#include "generated/eternalsonata_init.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>

#include "battle_layout.h"
#include "battle_system.h"
#include "eternalsonata_battle_api.h"
#include "guest_main_thread.h"
#include "room_presence.h"

namespace eternalsonata {
namespace {

// The battle intro's finisher, i.e. what the game's own skip button reaches.
// See battle_layout.h "Battle intro" for the phase machine around it.
REX_IMPORT(__imp__sub_821BB140, g_finish_battle_intro, u32(u32));

// The game's BTX text lookup: (block, index) -> guest char*. Enemy names come
// out of it exactly as sub_821ABE88 fetches them.
REX_IMPORT(__imp__sub_8223B780, g_lookup_text, u32(u32, u32));

// Set in OnPostSetup. Null until then, which is what makes every entry point
// answer "unavailable" during boot rather than dereferencing nothing.
rex::Runtime* g_runtime = nullptr;

// Give up rather than retry forever if the turn never comes back round, e.g.
// because the player is wedged in something else. At 60fps this is ~15s, which
// comfortably covers a long enemy action and its animation.
constexpr int kMaxDeferredFrames = 900;

rex::memory::Memory* Mem() { return g_runtime ? g_runtime->memory() : nullptr; }

template <typename T>
T ReadGuest(uint32_t address, T fallback = T{}) {
  auto* memory = Mem();
  if (!memory) {
    return fallback;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? rex::memory::load_and_swap<T>(host) : fallback;
}

uint8_t ReadGuestByte(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : uint8_t{0};
}

void WriteGuest32(uint32_t address, uint32_t value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    rex::memory::store_and_swap<uint32_t>(host, value);
  }
}

// A battle is in progress and its memory is readable. Deliberately routed
// through RoomPresence rather than calling FsmStateIsInBattle here: it is the
// same read either way, and going through the one accessor keeps the overlay,
// the Discord state row and the party-edit gate from ever disagreeing about
// whether a battle is running.
bool Available() { return g_runtime != nullptr && GetRoomPresence().IsBattleActive(); }

int PartyCount() { return static_cast<int>(ReadGuestByte(battle::kPartyCountAddr)); }
int EnemyCount() { return static_cast<int>(ReadGuestByte(battle::kEnemyCountAddr)); }

// The acting unit's {kind, slot}, as ETERNALSONATA_BATTLE_ACTOR_* and a slot.
// sub_821980D0's null path reports kind 2 (nobody acting), so the missing
// links in the chain report the same thing rather than a made-up party turn.
struct Actor {
  int kind = ETERNALSONATA_BATTLE_ACTOR_NONE;
  int slot = -1;
};

Actor CurrentActor() {
  Actor actor;
  const uint32_t holder =
      ReadGuest<uint32_t>(battle::kManager + battle::kActorHolderOffset);
  if (holder == 0) {
    return actor;
  }
  const uint32_t descriptor = ReadGuest<uint32_t>(holder + battle::kActorDescriptorOffset);
  if (descriptor == 0) {
    return actor;
  }
  const uint32_t kind = ReadGuest<uint32_t>(descriptor);
  if (kind != battle::kActorKindParty && kind != battle::kActorKindEnemy) {
    return actor;
  }
  actor.kind = static_cast<int>(kind);
  actor.slot = static_cast<int>(ReadGuestByte(descriptor + 4));
  return actor;
}

// The per-unit FSM state for one side's slot, or the game's own "no such unit"
// sentinel. sub_821AA6D8 resolves these as a pointer table indexed by slot.
uint32_t UnitState(int kind, int slot) {
  if (slot < 0) {
    return battle::kUnitStateNone;
  }
  const uint32_t base = kind == ETERNALSONATA_BATTLE_ACTOR_PARTY ? battle::kUnitFsmPartyBase
                                                                 : battle::kUnitFsmEnemyBase;
  const uint32_t unit =
      ReadGuest<uint32_t>(battle::kManager + base + static_cast<uint32_t>(slot) * 4u);
  if (unit == 0) {
    return battle::kUnitStateNone;
  }
  return ReadGuest<uint32_t>(unit + battle::kUnitFsmStateOffset);
}

// True when sub_821B7450 would actually reach its "are all enemies dead" scan:
// a party member has to hold the turn, and no action may be mid-resolution.
bool CanWinNow() {
  const Actor actor = CurrentActor();
  if (actor.kind != ETERNALSONATA_BATTLE_ACTOR_PARTY) {
    return false;
  }
  return !battle::UnitIsResolvingAction(UnitState(actor.kind, actor.slot));
}

int32_t Clamp(int32_t value, int32_t lo, int32_t hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

// A plain dword write with none of KillAllEnemies' turn-order hazard: nothing
// reads party HP the way the battle-over predicate reads enemy HP, so there
// is nothing to wedge.
bool SetPartyHp(int slot, int32_t hp) {
  const int live = PartyCount();
  if (slot < 0 || slot >= live) {
    return false;
  }
  const uint32_t record = battle::PartyRecord(static_cast<uint32_t>(slot));
  const uint32_t hp_max = ReadGuest<uint32_t>(record + battle::kPartyHpMaxOffset);
  WriteGuest32(record + battle::kPartyHpCurOffset,
               static_cast<uint32_t>(Clamp(hp, 0, static_cast<int32_t>(hp_max))));
  return true;
}

// Keeps the raw counter and the cached ratio in the same invariant the game
// itself maintains (see FillUnit above).
bool SetEnemyHp(int slot, int32_t hp) {
  const int live = EnemyCount();
  if (slot < 0 || slot >= live) {
    return false;
  }
  const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));
  const uint32_t part_index = ReadGuest<uint32_t>(record + battle::kEnemyPartIndexOffset);
  if (part_index >= battle::kEnemyPartCount) {
    return false;
  }
  const uint32_t part = battle::EnemyPart(static_cast<uint32_t>(slot), part_index);
  const uint32_t hp_max = ReadGuest<uint32_t>(part + battle::kEnemyHpMaxOffset);
  const int32_t clamped = Clamp(hp, 0, static_cast<int32_t>(hp_max));
  WriteGuest32(part + battle::kEnemyHpCurOffset, static_cast<uint32_t>(clamped));

  const float ratio = hp_max > 0 ? static_cast<float>(clamped) / static_cast<float>(hp_max) : 0.0f;
  uint32_t bits = 0;
  std::memcpy(&bits, &ratio, sizeof(bits));
  WriteGuest32(record + battle::kEnemyHpRatioOffset, bits);
  return true;
}

void KillAllEnemies() {
  const int live = EnemyCount();
  for (int i = 0; i < live; ++i) {
    SetEnemyHp(i, 0);
  }
}

void WinBattleOnGuestThread(int frames_waited) {
  // The battle may have ended on its own while the request was pending.
  if (!Available()) {
    return;
  }
  if (!CanWinNow()) {
    if (frames_waited < kMaxDeferredFrames) {
      PostToGuestMainThread([frames_waited] { WinBattleOnGuestThread(frames_waited + 1); });
    }
    return;
  }
  KillAllEnemies();
}

uint32_t FsmState() { return ReadGuest<uint32_t>(battle::kManager + battle::kFsmStateOffset); }

// The current actor's action-timer object, or 0 between turns / before one
// has ever been set up. See battle_layout.h "Turn timers".
uint32_t ActionTimerObject() {
  return ReadGuest<uint32_t>(battle::kManager + battle::kActionTimerObjectPtrOffset);
}

void FillTimers(EternalSonataBattleState* out) {
  out->command_timer_active = 0;
  out->command_timer_ticks = -1;
  out->turn_end_mode = 0;
  out->turn_end_ticks = -1;

  const uint32_t timer_obj = ActionTimerObject();
  if (timer_obj == 0) {
    return;
  }
  const uint32_t mode = ReadGuest<uint32_t>(timer_obj + battle::kCommandTimerModeOffset);
  out->command_timer_active = (mode == battle::kCommandTimerModeArmed) ? 1 : 0;
  out->command_timer_ticks =
      static_cast<int32_t>(ReadGuest<uint32_t>(timer_obj + battle::kCommandTimerRemainingOffset));
  out->turn_end_mode =
      static_cast<int32_t>(ReadGuest<uint32_t>(timer_obj + battle::kTurnEndModeOffset));
  out->turn_end_ticks =
      static_cast<int32_t>(ReadGuest<uint32_t>(timer_obj + battle::kTurnEndCounterOffset));
}

// --- Enemy names ---------------------------------------------------------
//
// Resolving one means calling the guest's text lookup, which needs a guest
// ThreadState the ImGui thread does not have (see guest_main_thread.h). So a
// name cannot be produced on demand for a caller that is drawing.
//
// Instead the first request for an unseen name id queues the lookup and
// answers "" for that frame; from the next frame on the cached string is
// returned immediately. A UI polling every frame therefore shows the name one
// frame late and never blocks, and repeat requests cost a map probe.
//
// Keyed by name id rather than by slot, so two of the same monster share one
// entry and a name survives the enemy dying or the record being reused.
std::mutex g_name_mutex;
std::unordered_map<uint32_t, std::string> g_enemy_names;
std::unordered_set<uint32_t> g_enemy_names_pending;

// Reads a NUL-terminated guest string. The game's text is single-byte
// (CP1252/Latin-1), not UTF-8, exactly as EternalSonataSetCharacterName
// documents for the party side; the bytes are passed through unchanged.
std::string ReadGuestString(uint32_t address, size_t max_length = 128) {
  auto* memory = Mem();
  if (!memory || address == 0) {
    return std::string();
  }
  const auto* host = memory->TranslateVirtual<const char*>(address);
  if (!host) {
    return std::string();
  }
  size_t length = 0;
  while (length < max_length && host[length] != '\0') {
    ++length;
  }
  return std::string(host, length);
}

void ResolveEnemyNameOnGuestThread(uint32_t name_id) {
  // Name ids are 1-based; the lookup takes index - 1.
  const std::string name = ReadGuestString(
      g_lookup_text(battle::kEnemyNameBtxBlock, name_id - 1u));
  std::lock_guard<std::mutex> lock(g_name_mutex);
  g_enemy_names_pending.erase(name_id);
  // Cache even an empty result: an id the text block has nothing for would
  // otherwise re-queue a guest call every single frame, forever.
  g_enemy_names.emplace(name_id, name);
}

// Never null. Returns "" while the lookup is still queued.
const char* EnemyNameFor(uint32_t name_id) {
  if (name_id == 0) {
    return "";
  }
  std::lock_guard<std::mutex> lock(g_name_mutex);
  const auto it = g_enemy_names.find(name_id);
  if (it != g_enemy_names.end()) {
    // unordered_map is node-based, so this stays valid as the map grows.
    return it->second.c_str();
  }
  if (g_enemy_names_pending.insert(name_id).second) {
    PostToGuestMainThread([name_id] { ResolveEnemyNameOnGuestThread(name_id); });
  }
  return "";
}

// Ends the intro on the spot, from whichever phase it is in.
//
// An earlier version waited for the intro object to reach its final phase,
// on the reasoning that that is the only phase the game's own skip button is
// live in. That made the skip useless: the phases are not a one-shot sequence
// but one pass per speaker, so waiting for the last phase meant sitting
// through the enemies' lines and camera work in full and only cutting the
// party's half. Reported 2026-08-19.
//
// Skipping from any phase is safe because sub_821BB140 does not just stop
// whatever the current phase started. Its first act is sub_821A9508, which
// walks every live unit on BOTH sides and calls the unit's own FSM vtable
// slot 4 to put it back to idle (state 26 -> 27, 35 -> 36, anything else
// -> 8). So the enemy-side animations a mid-intro skip interrupts are torn
// down by the same routine that tears down the party-side ones, and the rest
// of sub_821BB140 (camera reset, unit repositioning, sub_821BA360) does not
// care which phase it was called from.
//
// The one thing it does not do is stop an in-flight voice line, which will
// play out over the start of the battle. That is not a regression: the game's
// own skip button does not stop one either.
void SkipIntroOnGuestThread() {
  // The intro may have ended on its own while the request was queued.
  if (!Available() || FsmState() != battle::kFsmStateIntro) {
    return;
  }
  // The call site passes the object in r3 even though the routine works off
  // globals; match it rather than invent a different calling convention.
  g_finish_battle_intro(battle::IntroObject());
}

// Fills one side's unit. Returns false if the slot is past the live count.
bool FillUnit(int kind, int slot, EternalSonataBattleUnit* out) {
  const int live = kind == ETERNALSONATA_BATTLE_ACTOR_PARTY ? PartyCount() : EnemyCount();
  if (slot < 0 || slot >= live) {
    return false;
  }

  std::memset(out, 0, sizeof(*out));
  out->slot = slot;
  out->level = -1;
  out->hp = -1;
  out->hp_max = -1;
  out->hp_ratio = -1.0f;

  const uint32_t state = UnitState(kind, slot);
  out->unit_state = static_cast<int32_t>(state);
  out->resolving = battle::UnitIsResolvingAction(state) ? 1 : 0;

  const Actor actor = CurrentActor();
  out->acting = (actor.kind == kind && actor.slot == slot) ? 1 : 0;

  if (kind == ETERNALSONATA_BATTLE_ACTOR_PARTY) {
    const uint32_t record = battle::PartyRecord(static_cast<uint32_t>(slot));
    out->character =
        static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kPartyCharacterIdOffset));
    out->level = static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kPartyLevelOffset));
    out->hp = static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kPartyHpCurOffset));
    out->hp_max = static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kPartyHpMaxOffset));
    out->alive = out->hp > 0 ? 1 : 0;
    // The party side has no cached ratio in guest memory, but that is the
    // game's storage detail, not something a caller should have to know: it
    // computes the same quotient on demand itself (sub_821A9BE0), so do that
    // here rather than hand back a sentinel every consumer has to branch on.
    if (out->hp_max > 0) {
      out->hp_ratio = static_cast<float>(out->hp) / static_cast<float>(out->hp_max);
    }
  } else {
    const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));
    const uint32_t bits = ReadGuest<uint32_t>(record + battle::kEnemyHpRatioOffset);
    float ratio = 0.0f;
    std::memcpy(&ratio, &bits, sizeof(ratio));
    out->hp_ratio = ratio;
    out->alive = ratio > 0.0f ? 1 : 0;
    out->flags = static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kEnemyFlagsOffset));

    // The stats live in whichever part record is live, so a bad index would
    // read outside the record entirely; leave the fields at "unknown" rather
    // than report whatever is there.
    const uint32_t part_index = ReadGuest<uint32_t>(record + battle::kEnemyPartIndexOffset);
    if (part_index < battle::kEnemyPartCount) {
      const uint32_t part = battle::EnemyPart(static_cast<uint32_t>(slot), part_index);
      out->name_id = ReadGuest<uint16_t>(part + battle::kEnemyNameIdOffset);
      out->level = static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemyLevelOffset));
      out->hp = static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpCurOffset));
      out->hp_max = static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpMaxOffset));
    }
  }
  return true;
}

}  // namespace

void BindBattleSystem(rex::Runtime* runtime) { g_runtime = runtime; }

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (see src/eternalsonata_battle_api.h)
// ---------------------------------------------------------------------------

using namespace eternalsonata;

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataBattleAbiVersion(void) {
  return ETERNALSONATA_BATTLE_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetBattleState(
    EternalSonataBattleState* out) {
  if (!out) {
    return ETERNALSONATA_BATTLE_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }

  out->active = 1;
  out->fsm_state =
      static_cast<int32_t>(ReadGuest<uint32_t>(battle::kManager + battle::kFsmStateOffset));
  out->party_count = PartyCount();
  out->enemy_count = EnemyCount();

  const Actor actor = CurrentActor();
  out->actor_kind = actor.kind;
  out->actor_slot = actor.slot;
  out->can_win_now = CanWinNow() ? 1 : 0;
  FillTimers(out);
  return ETERNALSONATA_BATTLE_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetBattlePartyCount(void) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return PartyCount();
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetBattleEnemyCount(void) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return EnemyCount();
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetBattlePartyUnit(
    int slot, EternalSonataBattleUnit* out) {
  if (!out) {
    return ETERNALSONATA_BATTLE_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return FillUnit(ETERNALSONATA_BATTLE_ACTOR_PARTY, slot, out)
             ? ETERNALSONATA_BATTLE_OK
             : ETERNALSONATA_BATTLE_ERR_INVALID_SLOT;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetBattleEnemy(
    int slot, EternalSonataBattleUnit* out) {
  if (!out) {
    return ETERNALSONATA_BATTLE_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return FillUnit(ETERNALSONATA_BATTLE_ACTOR_ENEMY, slot, out)
             ? ETERNALSONATA_BATTLE_OK
             : ETERNALSONATA_BATTLE_ERR_INVALID_SLOT;
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetBattleStateName(int state) {
  // Which states mean what is reverse-engineering, not UI, so it lives here
  // rather than in each mod that wants to show it: a mod that hardcoded this
  // table would silently go stale the moment another state is identified.
  // Every state 1..23 is now identified, so nothing in range returns "", but
  // the fallback stays for anything out of range.
  switch (static_cast<uint32_t>(state)) {
    case battle::kFsmStateReset:
      return "resetting";
    case battle::kFsmStateWaitScene:
      return "waiting for scene";
    case battle::kFsmStateSetup:
      return "setting up";
    case battle::kFsmStateIntroSetup:
      return "intro setup";
    case battle::kFsmStateIntro:
      return "intro";
    case battle::kFsmStateTurnStart:
      return "turn start";
    case battle::kFsmStateTurnSetup:
      return "turn setup";
    case battle::kFsmStateActorCamera:
      return "actor camera";
    case battle::kFsmStateActorCameraWait:
      return "actor camera wait";
    case battle::kFsmStateTurnOpen:
      return "opening turn";
    case battle::kFsmStateCommandWait:
      return "choosing command";
    case battle::kFsmStateTurn:
      return "playing";
    case battle::kFsmStateTurnEnd:
      return "end of turn";
    case battle::kFsmStateOutcome:
      return "deciding outcome";
    case battle::kFsmStateDefeat:
      return "defeat";
    case battle::kFsmStateEscaped:
      return "escaped";
    case battle::kFsmStateVictoryPose:
      return "victory pose";
    case battle::kFsmStateEndOfBattle:
      return "end of battle";
    case battle::kFsmStateLevelUp:
      return "level up";
    case battle::kFsmStatePartyLevelCheck:
      return "party level check";
    case battle::kFsmStatePartyLevelUp:
      return "party level up";
    case battle::kFsmStateWriteBack:
      return "writing stats back";
    case battle::kFsmStateTeardown:
      return "teardown";
    default:
      return "";
  }
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetBattleEnemyName(int slot) {
  if (!Available()) {
    return "";
  }
  EternalSonataBattleUnit unit{};
  if (!FillUnit(ETERNALSONATA_BATTLE_ACTOR_ENEMY, slot, &unit)) {
    return "";
  }
  return EnemyNameFor(static_cast<uint32_t>(unit.name_id));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCanWinBattleNow(void) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return CanWinNow() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataWinBattle(void) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  PostToGuestMainThread([] { WinBattleOnGuestThread(0); });
  return ETERNALSONATA_BATTLE_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSkipBattleIntro(void) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  if (FsmState() != battle::kFsmStateIntro) {
    return ETERNALSONATA_BATTLE_ERR_NOT_IN_INTRO;
  }
  PostToGuestMainThread([] { SkipIntroOnGuestThread(); });
  return ETERNALSONATA_BATTLE_QUEUED;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetBattlePartyHp(int slot, int32_t hp) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return SetPartyHp(slot, hp) ? ETERNALSONATA_BATTLE_OK : ETERNALSONATA_BATTLE_ERR_INVALID_SLOT;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetBattleEnemyHp(int slot, int32_t hp) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  return SetEnemyHp(slot, hp) ? ETERNALSONATA_BATTLE_OK : ETERNALSONATA_BATTLE_ERR_INVALID_SLOT;
}
