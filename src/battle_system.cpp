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
#include <rex/system/mod_registry.h>

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

constexpr uint32_t kMagicTableAddr = 0x82015380u;
constexpr uint32_t kMagicStride = 12u;
constexpr uint32_t kMagicKindOffset = 4u;
constexpr uint32_t kPartyAbilityTableOffset = 80936u;
constexpr uint32_t kEnemyAbilityTableAddr = 0x82550E98u;
constexpr uint32_t kAbilityIdMax = 512u;
constexpr uint32_t kEchoDisplayObjectOffset = 533072u;
constexpr uint32_t kEchoCountOffset = 10224u;
constexpr uint32_t kParryFlagOffset = 537177u;

uint32_t g_last_action_id = 0;
int g_last_action_strength = 0;
int g_last_action_flags = 0;
bool g_last_critical = false;

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

uint32_t FsmState() { return ReadGuest<uint32_t>(battle::kManager + battle::kFsmStateOffset); }

// True when the current turn can be skipped: the FSM is in state 12
// (gameplay) and someone is acting.
bool CanSkipTurn() {
  if (FsmState() != battle::kFsmStateTurn) {
    return false;
  }
  const Actor actor = CurrentActor();
  return actor.kind != ETERNALSONATA_BATTLE_ACTOR_NONE;
}

void WinBattleOnGuestThread(int frames_waited) {
  // The battle may have ended on its own while the request was pending.
  if (!Available()) {
    return;
  }
  if (!CanWinNow()) {
    if (frames_waited < kMaxDeferredFrames) {
      // If an enemy holds the turn, skip it so we get to a party turn faster
      // rather than waiting for the enemy to finish naturally.
      if (CanSkipTurn() && CurrentActor().kind == ETERNALSONATA_BATTLE_ACTOR_ENEMY) {
        WriteGuest32(battle::kManager + battle::kFsmStateOffset, battle::kFsmStateTurnEnd);
      }
      PostToGuestMainThread([frames_waited] { WinBattleOnGuestThread(frames_waited + 1); });
    }
    return;
  }
  KillAllEnemies();
}

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
    out->status_mask =
        static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kPartyStatusMaskOffset));
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
    out->status_mask =
        static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kEnemyStatusMaskOffset));
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

int LightStateFor(int kind, int slot, uint32_t ability) {
  if (kind == ETERNALSONATA_BATTLE_ACTOR_ENEMY && slot >= 0 && slot < EnemyCount()) {
    const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));
    const uint32_t part_index = ReadGuest<uint32_t>(record + battle::kEnemyPartIndexOffset);
    if (part_index < battle::kEnemyPartCount) {
      const uint32_t flags = ReadGuest<uint32_t>(
          battle::EnemyPart(static_cast<uint32_t>(slot), part_index) +
          battle::kEnemyFlagBitsOffset);
      return (flags & (1u << 17)) ? ETERNALSONATA_BATTLE_DARK
                                  : ETERNALSONATA_BATTLE_LIGHT;
    }
  }
  if (kind == ETERNALSONATA_BATTLE_ACTOR_PARTY && ability > 0 && ability <= 512) {
    const uint16_t magic_kind = ReadGuest<uint16_t>(
        kMagicTableAddr + (ability - 1u) * kMagicStride + kMagicKindOffset);
    if (magic_kind == 2) {
      return ETERNALSONATA_BATTLE_LIGHT;
    }
    if (magic_kind == 3) {
      return ETERNALSONATA_BATTLE_DARK;
    }
  }
  return ETERNALSONATA_BATTLE_LIGHT_UNKNOWN;
}

int CharacterFor(const Actor& actor) {
  if (actor.kind != ETERNALSONATA_BATTLE_ACTOR_PARTY || actor.slot < 0 ||
      actor.slot >= PartyCount()) {
    return 0;
  }
  return static_cast<int32_t>(ReadGuest<uint32_t>(
      battle::PartyRecord(static_cast<uint32_t>(actor.slot)) +
      battle::kPartyCharacterIdOffset));
}

int AbilityStrength() {
  const uint32_t object =
      ReadGuest<uint32_t>(battle::kManager + kEchoDisplayObjectOffset);
  return object ? static_cast<int>(ReadGuest<uint32_t>(object + kEchoCountOffset)) : 0;
}

void PublishBattleAction(const char* name, uint32_t id, uint32_t action,
                         uint32_t light_ability) {
  if (!Available() || !g_runtime || !g_runtime->mod_registry()) {
    return;
  }
  const Actor actor = CurrentActor();
  if (actor.kind == ETERNALSONATA_BATTLE_ACTOR_NONE) {
    return;
  }
  EternalSonataBattleAction event{};
  event.actor_kind = actor.kind;
  event.actor_slot = actor.slot;
  event.action_id = static_cast<int32_t>(id);
  event.light_state = LightStateFor(actor.kind, actor.slot, light_ability);
  event.range = ETERNALSONATA_BATTLE_RANGE_UNKNOWN;
  event.distance = -1.0f;
  event.character = CharacterFor(actor);
  event.ability_strength = AbilityStrength();
  event.harmony_chain = action == 10 ? 1 : 0;
  event.counterattack = action == 14 || action == 18 ? 1 : 0;
  g_last_action_id = id;
  g_last_action_strength = event.ability_strength;
  g_last_action_flags = event.harmony_chain
                            ? ETERNALSONATA_BATTLE_EFFECT_HARMONY_CHAIN
                            : event.counterattack
                                  ? ETERNALSONATA_BATTLE_EFFECT_COUNTERATTACK
                                  : 0;
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = id;
  payload.f64 = event.distance;
  payload.bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(&event), sizeof(event));
  g_runtime->mod_registry()->Publish(name, payload);
  if (event.harmony_chain) {
    g_runtime->mod_registry()->Publish(ETERNALSONATA_BATTLE_EVENT_HARMONY_CHAIN, payload);
  }
  if (event.counterattack) {
    g_runtime->mod_registry()->Publish(ETERNALSONATA_BATTLE_EVENT_COUNTERATTACK, payload);
  }
}

void PublishBattleEffect(int target_kind, int target_slot, int signed_amount) {
  if (!Available() || !g_runtime || !g_runtime->mod_registry()) {
    return;
  }
  const bool parried = ReadGuestByte(battle::kManager + kParryFlagOffset) != 0;
  if (signed_amount == 0 && !parried) {
    g_last_critical = false;
    return;
  }
  const Actor source = CurrentActor();
  const Actor target{target_kind, target_slot};
  EternalSonataBattleEffect event{};
  event.source_kind = source.kind;
  event.source_slot = source.slot;
  event.source_character = CharacterFor(source);
  event.target_kind = target.kind;
  event.target_slot = target.slot;
  event.target_character = CharacterFor(target);
  event.action_id = static_cast<int32_t>(g_last_action_id);
  event.amount = signed_amount < 0 ? -signed_amount : signed_amount;
  event.flags = g_last_action_flags;
  event.ability_strength = g_last_action_strength;
  if (g_last_critical) {
    event.flags |= ETERNALSONATA_BATTLE_EFFECT_CRITICAL;
  }
  if (parried) {
    event.flags |= ETERNALSONATA_BATTLE_EFFECT_PARRIED;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = static_cast<uint64_t>(event.amount);
  payload.f64 = static_cast<double>(event.amount);
  payload.bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(&event), sizeof(event));
  if (signed_amount != 0) {
    g_runtime->mod_registry()->Publish(
        signed_amount > 0 ? ETERNALSONATA_BATTLE_EVENT_DAMAGE
                          : ETERNALSONATA_BATTLE_EVENT_HEAL,
        payload);
  }
  if (event.flags & ETERNALSONATA_BATTLE_EFFECT_CRITICAL) {
    g_runtime->mod_registry()->Publish(ETERNALSONATA_BATTLE_EVENT_CRITICAL, payload);
  }
  if (event.flags & ETERNALSONATA_BATTLE_EFFECT_PARRIED) {
    g_runtime->mod_registry()->Publish(ETERNALSONATA_BATTLE_EVENT_PARRY, payload);
  }
  g_last_critical = false;
}

// --- Per-unit condition watch --------------------------------------------
//
// Statuses, buffs and deaths are all reported by sampling rather than by
// hooking the routines that cause them. The apply and clear paths are clean
// enough to hook (sub_8218CEF8 / sub_8218D140), but the expiry passes, the
// stat buffs and death itself each land somewhere different, and several of
// them only reach the fields through a register-passed unit descriptor that
// would have to be decoded by hand. Reading the fields the game has already
// settled on costs a few dozen loads a frame and cannot disagree with what
// EternalSonataGetBattlePartyUnit reports, because it is the same read.
//
// The cost is attribution: a sampled change knows the frame it happened on,
// not the ability that caused it. See EternalSonataBattleStatus::source_kind.
struct UnitWatch {
  bool valid = false;
  // Party: character id. Enemy: name id, which also moves when a boss changes
  // form, and a form change swaps in a whole new stat block. A slot whose
  // identity changed is re-baselined instead of being diffed against a unit
  // that is no longer there.
  int32_t identity = 0;
  uint32_t status_mask = 0;
  int32_t alive = 0;
  int32_t stats[3] = {0, 0, 0};
};

UnitWatch g_party_watch[ETERNALSONATA_BATTLE_MAX_PARTY];
UnitWatch g_enemy_watch[ETERNALSONATA_BATTLE_MAX_ENEMIES];

// The live buffable stats, in ETERNALSONATA_BATTLE_STAT_* order. Both sides
// store them as 16-bit; the enemy's are inside the live part record, so an
// out-of-range part index leaves the sample untouched rather than reading
// outside the record.
void ReadUnitStats(int kind, int slot, int32_t* out) {
  if (kind == ETERNALSONATA_BATTLE_ACTOR_PARTY) {
    const uint32_t record = battle::PartyRecord(static_cast<uint32_t>(slot));
    out[ETERNALSONATA_BATTLE_STAT_ATTACK] =
        ReadGuest<uint16_t>(record + battle::kPartyAttackOffset);
    out[ETERNALSONATA_BATTLE_STAT_DEFENSE] =
        ReadGuest<uint16_t>(record + battle::kPartyDefenseOffset);
    out[ETERNALSONATA_BATTLE_STAT_SPEED] =
        ReadGuest<uint16_t>(record + battle::kPartySpeedOffset);
    return;
  }
  const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));
  const uint32_t part_index = ReadGuest<uint32_t>(record + battle::kEnemyPartIndexOffset);
  if (part_index >= battle::kEnemyPartCount) {
    return;
  }
  const uint32_t part = battle::EnemyPart(static_cast<uint32_t>(slot), part_index);
  out[ETERNALSONATA_BATTLE_STAT_ATTACK] =
      static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemyAttackOffset));
  out[ETERNALSONATA_BATTLE_STAT_DEFENSE] =
      static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemyDefenseOffset));
  out[ETERNALSONATA_BATTLE_STAT_SPEED] =
      static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemySpeedOffset));
}

uint32_t ReadStatusMask(int kind, int slot) {
  if (kind == ETERNALSONATA_BATTLE_ACTOR_PARTY) {
    return ReadGuest<uint32_t>(battle::PartyRecord(static_cast<uint32_t>(slot)) +
                               battle::kPartyStatusMaskOffset);
  }
  return ReadGuest<uint32_t>(battle::EnemyRecord(static_cast<uint32_t>(slot)) +
                             battle::kEnemyStatusMaskOffset);
}

template <typename T>
void PublishStruct(const char* name, const T& event, uint64_t u64, double f64) {
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = u64;
  payload.f64 = f64;
  payload.bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(&event), sizeof(event));
  g_runtime->mod_registry()->Publish(name, payload);
}

void PublishUnitChanges(int kind, int slot, const UnitWatch& before,
                        const UnitWatch& now, const EternalSonataBattleUnit& unit) {
  const Actor source = CurrentActor();
  const int32_t target_character =
      kind == ETERNALSONATA_BATTLE_ACTOR_PARTY ? unit.character : 0;

  if (before.alive != now.alive) {
    EternalSonataBattleDown event{};
    event.target_kind = kind;
    event.target_slot = slot;
    event.target_character = target_character;
    event.hp = unit.hp;
    event.hp_max = unit.hp_max;
    event.status_mask = static_cast<int32_t>(now.status_mask);
    event.source_kind = source.kind;
    event.source_slot = source.slot;
    event.source_character = CharacterFor(source);
    PublishStruct(now.alive ? ETERNALSONATA_BATTLE_EVENT_REVIVED
                            : ETERNALSONATA_BATTLE_EVENT_DOWN,
                  event, static_cast<uint64_t>(slot),
                  static_cast<double>(unit.hp));
  }

  const uint32_t changed = before.status_mask ^ now.status_mask;
  for (uint32_t id = 0; changed && id < battle::kStatusIdCount; ++id) {
    if ((changed & (1u << id)) == 0) {
      continue;
    }
    const bool gained = (now.status_mask & (1u << id)) != 0;
    EternalSonataBattleStatus event{};
    event.target_kind = kind;
    event.target_slot = slot;
    event.target_character = target_character;
    event.status = static_cast<int32_t>(id);
    event.status_mask = static_cast<int32_t>(now.status_mask);
    event.gained = gained ? 1 : 0;
    event.source_kind = source.kind;
    event.source_slot = source.slot;
    event.source_character = CharacterFor(source);
    PublishStruct(gained ? ETERNALSONATA_BATTLE_EVENT_STATUS_GAINED
                         : ETERNALSONATA_BATTLE_EVENT_STATUS_LOST,
                  event, id, 0.0);
  }

  for (int stat = 0; stat < 3; ++stat) {
    if (before.stats[stat] == now.stats[stat]) {
      continue;
    }
    EternalSonataBattleStatChange event{};
    event.target_kind = kind;
    event.target_slot = slot;
    event.target_character = target_character;
    event.stat = stat;
    event.previous = before.stats[stat];
    event.current = now.stats[stat];
    event.delta = now.stats[stat] - before.stats[stat];
    event.source_kind = source.kind;
    event.source_slot = source.slot;
    event.source_character = CharacterFor(source);
    PublishStruct(ETERNALSONATA_BATTLE_EVENT_STAT_CHANGED, event,
                  static_cast<uint64_t>(stat), static_cast<double>(event.delta));
  }
}

void PollSide(int kind, UnitWatch* watch, int capacity) {
  const int live = kind == ETERNALSONATA_BATTLE_ACTOR_PARTY ? PartyCount() : EnemyCount();
  for (int slot = 0; slot < capacity; ++slot) {
    EternalSonataBattleUnit unit{};
    if (slot >= live || !FillUnit(kind, slot, &unit)) {
      watch[slot].valid = false;
      continue;
    }
    UnitWatch now;
    now.valid = true;
    now.identity = kind == ETERNALSONATA_BATTLE_ACTOR_PARTY ? unit.character : unit.name_id;
    now.status_mask = ReadStatusMask(kind, slot);
    now.alive = unit.alive;
    ReadUnitStats(kind, slot, now.stats);

    const UnitWatch before = watch[slot];
    watch[slot] = now;
    if (before.valid && before.identity == now.identity) {
      PublishUnitChanges(kind, slot, before, now, unit);
    }
  }
}

// Runs once per battle frame, off the back of the battle FSM. Sampling from
// the FSM rather than from the host's frame callback keeps this on the guest
// thread that owns the fields, and gives it a natural off switch: the FSM only
// runs while there is a battle.
void PollBattleUnits() {
  if (!Available() || !g_runtime || !g_runtime->mod_registry()) {
    for (auto& watch : g_party_watch) {
      watch.valid = false;
    }
    for (auto& watch : g_enemy_watch) {
      watch.valid = false;
    }
    return;
  }
  PollSide(ETERNALSONATA_BATTLE_ACTOR_PARTY, g_party_watch, ETERNALSONATA_BATTLE_MAX_PARTY);
  PollSide(ETERNALSONATA_BATTLE_ACTOR_ENEMY, g_enemy_watch, ETERNALSONATA_BATTLE_MAX_ENEMIES);
}

uint32_t AbilityIdFromRecord(uint32_t record) {
  if (!record) {
    return 0;
  }
  const Actor actor = CurrentActor();
  uint32_t table = 0;
  if (actor.kind == ETERNALSONATA_BATTLE_ACTOR_PARTY && actor.slot >= 0 &&
      actor.slot < PartyCount()) {
    table = battle::PartyRecord(static_cast<uint32_t>(actor.slot)) +
            kPartyAbilityTableOffset;
  } else if (actor.kind == ETERNALSONATA_BATTLE_ACTOR_ENEMY) {
    table = kEnemyAbilityTableAddr;
  }
  for (uint32_t id = 1; table && id <= kAbilityIdMax; ++id) {
    if (ReadGuest<uint32_t>(table + id * 4u) == record) {
      return id;
    }
  }
  return 0;
}

}  // namespace

void BindBattleSystem(rex::Runtime* runtime) { g_runtime = runtime; }

void NotifyBattleAbility(uint32_t ability, uint32_t action) {
  const uint16_t kind = ability > 0 && ability <= 512
                            ? ReadGuest<uint16_t>(kMagicTableAddr + (ability - 1u) *
                                                                     kMagicStride +
                                                 kMagicKindOffset)
                            : 0;
  PublishBattleAction(kind == 1 ? ETERNALSONATA_BATTLE_EVENT_ATTACK
                                : ETERNALSONATA_BATTLE_EVENT_ABILITY,
                      ability, action, ability);
}

void NotifyBattleItem(uint32_t item) {
  PublishBattleAction(ETERNALSONATA_BATTLE_EVENT_ITEM, item, 0, 0);
}

}  // namespace eternalsonata

REX_EXTERN(__imp__sub_8219CE70);
REX_HOOK_RAW(sub_8219CE70) {
  const u32 action = REX_LOAD_U32(ctx.r3.u32);
  const u32 ability_record = ctx.r4.u32;
  const u32 ability = eternalsonata::AbilityIdFromRecord(ability_record);
  __imp__sub_8219CE70(ctx, base);
  if (ability) {
    eternalsonata::NotifyBattleAbility(ability, action);
  }
}

REX_EXTERN(__imp__sub_821E6BA8);
REX_HOOK_RAW(sub_821E6BA8) {
  const u32 item = ctx.r3.u32;
  __imp__sub_821E6BA8(ctx, base);
  eternalsonata::NotifyBattleItem(item);
}

REX_EXTERN(__imp__sub_821AF7D0);
REX_HOOK_RAW(sub_821AF7D0) {
  __imp__sub_821AF7D0(ctx, base);
  eternalsonata::g_last_critical = ctx.r3.u32 != 0;
}

REX_EXTERN(__imp__sub_821B0A58);
REX_HOOK_RAW(sub_821B0A58) {
  const int target_kind = static_cast<int>(ctx.r4.u32);
  const int target_slot = static_cast<int>(ctx.r5.u32);
  const int amount = static_cast<int32_t>(ctx.r6.u32);
  __imp__sub_821B0A58(ctx, base);
  eternalsonata::PublishBattleEffect(target_kind, target_slot, amount);
}

// The battle state machine (battle_layout.h "Battle FSM"), which runs once per
// frame for as long as a battle is live. Sampling after it means the frame's
// status expiries, buffs and deaths have all already landed.
REX_EXTERN(__imp__sub_821ACBF8);
REX_HOOK_RAW(sub_821ACBF8) {
  __imp__sub_821ACBF8(ctx, base);
  eternalsonata::PollBattleUnits();
}

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

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSkipBattleTurn(void) {
  if (!Available()) {
    return ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  }
  if (!CanSkipTurn()) {
    return ETERNALSONATA_BATTLE_ERR_NOT_IN_TURN;
  }
  // Advance the FSM from state 12 (playing) directly to state 13 (end of
  // turn). State 13 holds until sub_821AA730 agrees the action has finished
  // resolving, then runs the end-of-turn passes and goes back to state 7
  // (turn start) for the next actor. This is safe even if an action is
  // mid-resolution: state 13 waits it out on its own.
  WriteGuest32(battle::kManager + battle::kFsmStateOffset, battle::kFsmStateTurnEnd);
  return ETERNALSONATA_BATTLE_OK;
}
