// eternalsonata - Enemy stats, and the per-type overrides that rebalance them.
//
// The public C ABI is eternalsonata_enemy_api.h and the guest layout every
// offset here comes from is battle_layout.h, which also carries the
// reverse-engineering writeup for each field. The short version:
//
//   * Everything an encounter knows about one monster is a 168-byte block at
//     part+300, inside whichever of the record's two "part" sub-records is
//     live. There is no per-type table anywhere else: the block is populated
//     per instance when the encounter loads, so a rebalance is not a matter of
//     editing one row, it is a matter of reasserting the numbers on every
//     enemy of every battle.
//   * Attack, defense and speed each have a pristine copy 168 bytes further
//     on, which the game clamps ability buffs against (base * [0.7, 1.3]).
//     Writing only the live field would let the next buff drag the value back
//     towards the original, so both are written.
//   * Current HP is mirrored by a cached current/max ratio at record+0x7E18,
//     and it is the ratio, not the counter, that the game's own liveness
//     checks read. Anything moving HP has to move both.
//
// ---------------------------------------------------------------------------
// Why overrides are computed from a snapshot
//
// An override is reapplied every frame, because nothing stops the game from
// rewriting a record: an encounter loads its enemies afresh, and a boss can
// swap to its second part mid-battle. Applying "x1.5" to the current value
// each time would compound to x1.5 per frame, so the host instead snapshots a
// record's untouched stats the first time it sees it and computes every
// application from that. This also makes clearing an override restore the real
// original rather than whatever happened to be written last.
//
// A record is recognised as new by its name id changing, or by the battle
// ending; the snapshot is per (slot, part) so a two-form boss keeps one for
// each of its forms.
//
// The one stat that is NOT reapplied per frame is current HP. Reasserting it
// sixty times a second would make an enemy unkillable, which is a bug rather
// than a rebalance, so an HP override is applied once, when the record is
// first seen, and describes the health the enemy enters the battle with.
//
// Threading: the exported entry points are called from mods, i.e. usually from
// the ImGui draw thread. Every one of them is a plain guest-memory access or a
// host-side table edit, so none of them queues; the per-frame reapplication
// runs on the guest main thread out of the render pump's present hook.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "battle_layout.h"
#include "enemy_system.h"
#include "eternalsonata_enemy_api.h"
#include "room_presence.h"

namespace eternalsonata {
namespace {

std::mutex g_mutex;
rex::Runtime* g_runtime = nullptr;

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

template <typename T>
void WriteGuest(uint32_t address, T value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    rex::memory::store_and_swap<T>(host, value);
  }
}

uint8_t ReadGuestByte(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : uint8_t{0};
}

void WriteGuestByte(uint32_t address, uint8_t value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    *host = value;
  }
}

float ReadGuestFloat(uint32_t address) {
  const uint32_t bits = ReadGuest<uint32_t>(address);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteGuestFloat(uint32_t address, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteGuest<uint32_t>(address, bits);
}

// Routed through RoomPresence for the same reason battle_system.cpp does it:
// one accessor decides whether a battle is running, so nothing can disagree.
bool Available() { return g_runtime != nullptr && GetRoomPresence().IsBattleActive(); }

int EnemyCount() { return static_cast<int>(ReadGuestByte(battle::kEnemyCountAddr)); }

// --- The stat table ------------------------------------------------------
//
// One row per ETERNALSONATA_ENEMY_STAT_*, in enum order, so the id indexes it
// directly. Everything that varies between stats lives here rather than in a
// switch, which is what keeps adding a stat to an additive change.

enum class Kind {
  kI8,     // signed byte
  kI16,    // signed halfword
  kI32,    // signed word
  kPct,    // float, exposed as a percentage of 1.0
};

struct StatInfo {
  const char* name;
  uint32_t offset;  // within the part record
  Kind kind;
  // Attack, defense and speed carry a pristine copy kEnemyBaseStatsOffset
  // further on that the game clamps buffs against; it has to move with the
  // live field.
  bool has_base_copy;
  // Never let an override drive this below zero: a negative reward or HP is
  // not something the game's own screens are prepared for.
  bool non_negative;
  // Applied once, when the record is first seen, rather than every frame.
  // Current HP only; see the header comment.
  bool apply_once;
};

constexpr StatInfo kStats[ETERNALSONATA_ENEMY_STAT_COUNT] = {
    {"level", battle::kEnemyLevelOffset, Kind::kI16, false, true, false},
    {"hp", battle::kEnemyHpCurOffset, Kind::kI32, false, true, true},
    {"hp max", battle::kEnemyHpMaxOffset, Kind::kI32, false, true, false},
    {"attack", battle::kEnemyAttackOffset, Kind::kI16, true, true, false},
    {"defense", battle::kEnemyDefenseOffset, Kind::kI16, true, true, false},
    {"speed", battle::kEnemySpeedOffset, Kind::kI16, true, true, false},
    {"physical resist", battle::kEnemyPhysicalResistOffset, Kind::kI8, false, false, false},
    {"magic resist", battle::kEnemyMagicResistOffset, Kind::kI8, false, false, false},
    {"crit rate", battle::kEnemyCritRateOffset, Kind::kI8, false, true, false},
    {"exp", battle::kEnemyExpOffset, Kind::kI32, false, true, false},
    {"gold", battle::kEnemyGoldOffset, Kind::kI32, false, true, false},
    {"drop item 1", battle::kEnemyDropItem1Offset, Kind::kI16, false, true, false},
    {"drop rate 1", battle::kEnemyDropRate1Offset, Kind::kI8, false, true, false},
    {"drop item 2", battle::kEnemyDropItem2Offset, Kind::kI16, false, true, false},
    {"drop rate 2", battle::kEnemyDropRate2Offset, Kind::kI8, false, true, false},
    {"move range", battle::kEnemyMoveRangeOffset, Kind::kPct, false, true, false},
    {"chase range", battle::kEnemyChaseRangeOffset, Kind::kPct, false, true, false},
    {"scale", battle::kEnemyScaleOffset, Kind::kPct, false, true, false},
};

bool ValidStat(int stat) { return stat >= 0 && stat < ETERNALSONATA_ENEMY_STAT_COUNT; }

int32_t Clamp(int64_t value, int64_t lo, int64_t hi) {
  return static_cast<int32_t>(value < lo ? lo : (value > hi ? hi : value));
}

// Fits `value` to what the field actually holds, so a wild multiplier wraps
// into a small negative number instead of a large positive one.
int32_t ClampToField(const StatInfo& info, int64_t value) {
  int64_t lo = 0;
  int64_t hi = 0;
  switch (info.kind) {
    case Kind::kI8:
      lo = -128;
      hi = 127;
      break;
    case Kind::kI16:
      lo = -32768;
      hi = 32767;
      break;
    case Kind::kI32:
    case Kind::kPct:
      lo = INT32_MIN;
      hi = INT32_MAX;
      break;
  }
  if (info.non_negative) {
    lo = 0;
  }
  return Clamp(value, lo, hi);
}

int32_t ReadStat(uint32_t part, int stat) {
  const StatInfo& info = kStats[stat];
  switch (info.kind) {
    case Kind::kI8:
      return static_cast<int8_t>(ReadGuestByte(part + info.offset));
    case Kind::kI16:
      return static_cast<int16_t>(ReadGuest<uint16_t>(part + info.offset));
    case Kind::kI32:
      return static_cast<int32_t>(ReadGuest<uint32_t>(part + info.offset));
    case Kind::kPct:
      return static_cast<int32_t>(std::lround(ReadGuestFloat(part + info.offset) * 100.0f));
  }
  return 0;
}

// The cached current/max ratio the game's own liveness checks read. Kept in
// the invariant the game maintains (max * ratio == cur) by every HP write.
void RefreshHpRatio(uint32_t record, uint32_t part) {
  const int32_t hp = static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpCurOffset));
  const int32_t hp_max = static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpMaxOffset));
  const float ratio =
      hp_max > 0 ? static_cast<float>(hp) / static_cast<float>(hp_max) : 0.0f;
  WriteGuestFloat(record + battle::kEnemyHpRatioOffset, ratio);
}

void WriteStat(uint32_t record, uint32_t part, int stat, int32_t value) {
  const StatInfo& info = kStats[stat];
  const int32_t clamped = ClampToField(info, value);

  // Raising the maximum without moving the current would hand the player a
  // half-dead enemy; hold the fraction instead, so "x2 HP" is a fight twice as
  // long rather than one that starts already won.
  if (stat == ETERNALSONATA_ENEMY_STAT_HP_MAX) {
    const int32_t old_max =
        static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpMaxOffset));
    const int32_t old_hp =
        static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpCurOffset));
    WriteGuest<uint32_t>(part + battle::kEnemyHpMaxOffset, static_cast<uint32_t>(clamped));
    const int64_t scaled =
        old_max > 0 ? (static_cast<int64_t>(old_hp) * clamped) / old_max : clamped;
    WriteGuest<uint32_t>(part + battle::kEnemyHpCurOffset,
                         static_cast<uint32_t>(Clamp(scaled, 0, clamped)));
    RefreshHpRatio(record, part);
    return;
  }

  switch (info.kind) {
    case Kind::kI8:
      WriteGuestByte(part + info.offset, static_cast<uint8_t>(static_cast<int8_t>(clamped)));
      break;
    case Kind::kI16:
      WriteGuest<uint16_t>(part + info.offset, static_cast<uint16_t>(static_cast<int16_t>(clamped)));
      if (info.has_base_copy) {
        WriteGuest<uint16_t>(part + info.offset + battle::kEnemyBaseStatsOffset,
                             static_cast<uint16_t>(static_cast<int16_t>(clamped)));
      }
      break;
    case Kind::kI32:
      WriteGuest<uint32_t>(part + info.offset, static_cast<uint32_t>(clamped));
      break;
    case Kind::kPct:
      WriteGuestFloat(part + info.offset, static_cast<float>(clamped) / 100.0f);
      break;
  }

  if (stat == ETERNALSONATA_ENEMY_STAT_HP) {
    const int32_t hp_max =
        static_cast<int32_t>(ReadGuest<uint32_t>(part + battle::kEnemyHpMaxOffset));
    WriteGuest<uint32_t>(part + battle::kEnemyHpCurOffset,
                         static_cast<uint32_t>(Clamp(clamped, 0, hp_max)));
    RefreshHpRatio(record, part);
  }
}

// --- Overrides -----------------------------------------------------------

struct Override {
  int mode = ETERNALSONATA_ENEMY_OVERRIDE_MULTIPLY;
  int32_t value = 0;
  float multiplier = 1.0f;
};

// Keyed by type * stat count + stat, so one probe answers "does this type
// override this stat".
uint64_t OverrideKey(int type, int stat) {
  return static_cast<uint64_t>(static_cast<uint32_t>(type)) * ETERNALSONATA_ENEMY_STAT_COUNT +
         static_cast<uint64_t>(stat);
}

std::unordered_map<uint64_t, Override> g_overrides;

// What one enemy's stats were before anything was written to them, plus enough
// identity to notice the record being reused for a different monster.
struct Snapshot {
  int32_t name_id = 0;
  int32_t stats[ETERNALSONATA_ENEMY_STAT_COUNT] = {};
};

// Keyed by slot * part count + part.
std::unordered_map<uint32_t, Snapshot> g_snapshots;

const Override* FindOverrideLocked(int name_id, int stat) {
  // A type's own rule beats the catch-all, so a mod can say "everything x1.5,
  // except this boss" without ordering games.
  const auto specific = g_overrides.find(OverrideKey(name_id, stat));
  if (specific != g_overrides.end()) {
    return &specific->second;
  }
  const auto any = g_overrides.find(OverrideKey(ETERNALSONATA_ENEMY_TYPE_ANY, stat));
  return any != g_overrides.end() ? &any->second : nullptr;
}

int32_t ValueFor(const Override& rule, const StatInfo& info, int32_t original) {
  if (rule.mode == ETERNALSONATA_ENEMY_OVERRIDE_ABSOLUTE) {
    return ClampToField(info, rule.value);
  }
  const double scaled = static_cast<double>(original) * static_cast<double>(rule.multiplier);
  if (!std::isfinite(scaled)) {
    return ClampToField(info, original);
  }
  return ClampToField(info, static_cast<int64_t>(std::llround(scaled)));
}

// Applies every override that names `name_id` to one part record. `first_seen`
// is true only on the frame the snapshot was taken, which is when the
// apply-once stats get their one chance.
void ApplyToPartLocked(uint32_t record, uint32_t part, const Snapshot& snapshot, bool first_seen) {
  for (int stat = 0; stat < ETERNALSONATA_ENEMY_STAT_COUNT; ++stat) {
    const StatInfo& info = kStats[stat];
    if (info.apply_once && !first_seen) {
      continue;
    }
    const Override* rule = FindOverrideLocked(snapshot.name_id, stat);
    if (!rule) {
      continue;
    }
    WriteStat(record, part, stat, ValueFor(*rule, info, snapshot.stats[stat]));
  }
}

void RestorePartLocked(uint32_t record, uint32_t part, const Snapshot& snapshot) {
  for (int stat = 0; stat < ETERNALSONATA_ENEMY_STAT_COUNT; ++stat) {
    if (kStats[stat].apply_once) {
      // Putting the enemy's opening HP back mid-fight would undo the battle.
      continue;
    }
    WriteStat(record, part, stat, snapshot.stats[stat]);
  }
}

uint32_t SnapshotKey(int slot, uint32_t part_index) {
  return static_cast<uint32_t>(slot) * battle::kEnemyPartCount + part_index;
}

// Walks every live enemy and both of its parts, taking a snapshot of anything
// new and reasserting the overrides on everything. Called once per guest
// frame; costs one pass over at most 16 records when no override is set.
void ApplyAllLocked(bool restore_only) {
  const int live = EnemyCount();
  for (int slot = 0; slot < live && slot < ETERNALSONATA_ENEMY_MAX_SLOTS; ++slot) {
    const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));
    const bool two_parts = ReadGuestByte(record + battle::kEnemyHasSecondPartOffset) != 0;
    const uint32_t parts = two_parts ? battle::kEnemyPartCount : 1u;
    for (uint32_t part_index = 0; part_index < parts; ++part_index) {
      const uint32_t part = battle::EnemyPart(static_cast<uint32_t>(slot), part_index);
      const int32_t name_id =
          static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemyNameIdOffset));
      if (name_id <= 0) {
        // An uninitialised part; the second one of a single-form enemy reads
        // as this even when the record claims to have it.
        continue;
      }

      const uint32_t key = SnapshotKey(slot, part_index);
      auto it = g_snapshots.find(key);
      bool first_seen = false;
      if (it == g_snapshots.end() || it->second.name_id != name_id) {
        Snapshot fresh;
        fresh.name_id = name_id;
        for (int stat = 0; stat < ETERNALSONATA_ENEMY_STAT_COUNT; ++stat) {
          fresh.stats[stat] = ReadStat(part, stat);
        }
        it = g_snapshots.insert_or_assign(key, fresh).first;
        first_seen = true;
      }

      if (restore_only) {
        RestorePartLocked(record, part, it->second);
      } else {
        ApplyToPartLocked(record, part, it->second, first_seen);
      }
    }
  }
}

// --- Live enemy access ---------------------------------------------------

// Resolves the live part of one enemy slot, or 0 if the slot is out of range
// or the record's part index is garbage (which would address outside it).
uint32_t LivePart(int slot, uint32_t* part_index_out = nullptr) {
  if (slot < 0 || slot >= EnemyCount()) {
    return 0;
  }
  const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));
  const uint32_t part_index = ReadGuest<uint32_t>(record + battle::kEnemyPartIndexOffset);
  if (part_index >= battle::kEnemyPartCount) {
    return 0;
  }
  if (part_index_out) {
    *part_index_out = part_index;
  }
  return battle::EnemyPart(static_cast<uint32_t>(slot), part_index);
}

std::string ReadGuestString(uint32_t address, size_t max_length = 64) {
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

// Type names are read out of the record, which goes away with the battle, so
// they are interned here to keep the returned pointer valid for the life of
// the process as the header promises.
std::unordered_map<std::string, std::string> g_type_names;

const char* InternTypeNameLocked(const std::string& name) {
  if (name.empty()) {
    return "";
  }
  // unordered_map is node-based, so this stays valid as the map grows.
  return g_type_names.emplace(name, name).first->second.c_str();
}

// --- Events --------------------------------------------------------------

// Called with g_mutex NOT held: a subscriber may call straight back in.
void PublishOverrideEvent(const char* event_name, int type, int stat, double value) {
  rex::Runtime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    runtime = g_runtime;
  }
  if (!runtime || !runtime->mod_registry()) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = (static_cast<uint64_t>(static_cast<uint32_t>(type)) << 32) |
                static_cast<uint32_t>(stat);
  payload.f64 = value;
  runtime->mod_registry()->Publish(event_name, payload);
}

}  // namespace

void BindEnemySystem(rex::Runtime* runtime) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_runtime = runtime;
  g_snapshots.clear();
}

void EnemySystemTick() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    // Between battles the records are stale; start clean for the next one so
    // nothing is measured against a previous encounter's numbers.
    g_snapshots.clear();
    return;
  }
  if (g_overrides.empty() && g_snapshots.empty()) {
    return;
  }
  ApplyAllLocked(false);
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (see src/eternalsonata_enemy_api.h)
// ---------------------------------------------------------------------------

using namespace eternalsonata;

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataEnemyAbiVersion(void) {
  return ETERNALSONATA_ENEMY_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsEnemySystemAvailable(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Available() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetEnemyStatName(int stat) {
  return ValidStat(stat) ? kStats[stat].name : "";
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEnemyStats(
    int slot, EternalSonataEnemyStats* out) {
  if (!out) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_ENEMY_ERR_UNAVAILABLE;
  }
  uint32_t part_index = 0;
  const uint32_t part = LivePart(slot, &part_index);
  if (part == 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_SLOT;
  }
  const uint32_t record = battle::EnemyRecord(static_cast<uint32_t>(slot));

  out->slot = slot;
  out->part = static_cast<int32_t>(part_index);
  out->name_id = static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemyNameIdOffset));
  out->level = ReadStat(part, ETERNALSONATA_ENEMY_STAT_LEVEL);
  out->hp = ReadStat(part, ETERNALSONATA_ENEMY_STAT_HP);
  out->hp_max = ReadStat(part, ETERNALSONATA_ENEMY_STAT_HP_MAX);
  out->attack = ReadStat(part, ETERNALSONATA_ENEMY_STAT_ATTACK);
  out->defense = ReadStat(part, ETERNALSONATA_ENEMY_STAT_DEFENSE);
  out->speed = ReadStat(part, ETERNALSONATA_ENEMY_STAT_SPEED);
  out->physical_resist = ReadStat(part, ETERNALSONATA_ENEMY_STAT_PHYSICAL_RESIST);
  out->magic_resist = ReadStat(part, ETERNALSONATA_ENEMY_STAT_MAGIC_RESIST);
  out->crit_rate = ReadStat(part, ETERNALSONATA_ENEMY_STAT_CRIT_RATE);
  out->exp = ReadStat(part, ETERNALSONATA_ENEMY_STAT_EXP);
  out->gold = ReadStat(part, ETERNALSONATA_ENEMY_STAT_GOLD);
  out->drop_item_1 = ReadStat(part, ETERNALSONATA_ENEMY_STAT_DROP_ITEM_1);
  out->drop_rate_1 = ReadStat(part, ETERNALSONATA_ENEMY_STAT_DROP_RATE_1);
  out->drop_item_2 = ReadStat(part, ETERNALSONATA_ENEMY_STAT_DROP_ITEM_2);
  out->drop_rate_2 = ReadStat(part, ETERNALSONATA_ENEMY_STAT_DROP_RATE_2);
  out->move_range_pct = ReadStat(part, ETERNALSONATA_ENEMY_STAT_MOVE_RANGE_PCT);
  out->chase_range_pct = ReadStat(part, ETERNALSONATA_ENEMY_STAT_CHASE_RANGE_PCT);
  out->scale_pct = ReadStat(part, ETERNALSONATA_ENEMY_STAT_SCALE_PCT);
  out->flags = static_cast<int32_t>(ReadGuest<uint32_t>(record + battle::kEnemyFlagsOffset));
  return ETERNALSONATA_ENEMY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEnemyStat(int slot, int stat) {
  if (!ValidStat(stat)) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_STAT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_ENEMY_ERR_UNAVAILABLE;
  }
  const uint32_t part = LivePart(slot);
  if (part == 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_SLOT;
  }
  return ReadStat(part, stat);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetEnemyStat(int slot, int stat, int32_t value) {
  if (!ValidStat(stat)) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_STAT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_ENEMY_ERR_UNAVAILABLE;
  }
  uint32_t part_index = 0;
  const uint32_t part = LivePart(slot, &part_index);
  if (part == 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_SLOT;
  }
  WriteStat(battle::EnemyRecord(static_cast<uint32_t>(slot)), part, stat, value);
  return ETERNALSONATA_ENEMY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEnemyType(int slot) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_ENEMY_ERR_UNAVAILABLE;
  }
  const uint32_t part = LivePart(slot);
  if (part == 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_SLOT;
  }
  return static_cast<int16_t>(ReadGuest<uint16_t>(part + battle::kEnemyNameIdOffset));
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetEnemyTypeName(int slot) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return "";
  }
  const uint32_t part = LivePart(slot);
  if (part == 0) {
    return "";
  }
  return InternTypeNameLocked(ReadGuestString(part + battle::kEnemyTypeNameOffset));
}

// --- Per-type overrides ----------------------------------------------------

namespace {

int SetOverride(int type, int stat, const Override& rule) {
  if (type < 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  if (!ValidStat(stat)) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_STAT;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_overrides[OverrideKey(type, stat)] = rule;
    // Take effect on this frame rather than the next one when a battle is
    // already running, so a UI slider tracks.
    if (Available()) {
      ApplyAllLocked(false);
    }
  }
  PublishOverrideEvent(ETERNALSONATA_ENEMY_EVENT_OVERRIDE_SET, type, stat,
                       rule.mode == ETERNALSONATA_ENEMY_OVERRIDE_ABSOLUTE
                           ? static_cast<double>(rule.value)
                           : static_cast<double>(rule.multiplier));
  return ETERNALSONATA_ENEMY_OK;
}

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetEnemyTypeStatMultiplier(
    int type, int stat, float multiplier) {
  if (!std::isfinite(multiplier) || multiplier < 0.0f) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  Override rule;
  rule.mode = ETERNALSONATA_ENEMY_OVERRIDE_MULTIPLY;
  rule.multiplier = multiplier;
  return SetOverride(type, stat, rule);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetEnemyTypeStatValue(int type, int stat,
                                                                       int32_t value) {
  Override rule;
  rule.mode = ETERNALSONATA_ENEMY_OVERRIDE_ABSOLUTE;
  rule.value = value;
  return SetOverride(type, stat, rule);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataClearEnemyTypeStat(int type, int stat) {
  if (type < 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  if (!ValidStat(stat)) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_STAT;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_overrides.erase(OverrideKey(type, stat)) == 0) {
      return ETERNALSONATA_ENEMY_ERR_NO_OVERRIDE;
    }
    if (Available()) {
      // Put the snapshot back first, then let whatever overrides are left
      // reassert themselves over it, so clearing one of several is not the
      // same as clearing them all.
      ApplyAllLocked(true);
      ApplyAllLocked(false);
    }
  }
  PublishOverrideEvent(ETERNALSONATA_ENEMY_EVENT_OVERRIDE_CLEARED, type, stat, 0.0);
  return ETERNALSONATA_ENEMY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataClearEnemyTypeOverrides(int type) {
  if (type < 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  std::vector<int> cleared;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int stat = 0; stat < ETERNALSONATA_ENEMY_STAT_COUNT; ++stat) {
      if (g_overrides.erase(OverrideKey(type, stat)) != 0) {
        cleared.push_back(stat);
      }
    }
    if (!cleared.empty() && Available()) {
      ApplyAllLocked(true);
      ApplyAllLocked(false);
    }
  }
  for (int stat : cleared) {
    PublishOverrideEvent(ETERNALSONATA_ENEMY_EVENT_OVERRIDE_CLEARED, type, stat, 0.0);
  }
  return static_cast<int>(cleared.size());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataClearAllEnemyOverrides(void) {
  std::vector<std::pair<int, int>> cleared;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& entry : g_overrides) {
      cleared.emplace_back(static_cast<int>(entry.first / ETERNALSONATA_ENEMY_STAT_COUNT),
                           static_cast<int>(entry.first % ETERNALSONATA_ENEMY_STAT_COUNT));
    }
    g_overrides.clear();
    if (!cleared.empty() && Available()) {
      ApplyAllLocked(true);
    }
  }
  for (const auto& entry : cleared) {
    PublishOverrideEvent(ETERNALSONATA_ENEMY_EVENT_OVERRIDE_CLEARED, entry.first, entry.second,
                         0.0);
  }
  return static_cast<int>(cleared.size());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEnemyTypeOverride(
    int type, int stat, EternalSonataEnemyOverride* out) {
  if (!out || type < 0) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  if (!ValidStat(stat)) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_STAT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_overrides.find(OverrideKey(type, stat));
  if (it == g_overrides.end()) {
    return ETERNALSONATA_ENEMY_ERR_NO_OVERRIDE;
  }
  out->type = type;
  out->stat = stat;
  out->mode = it->second.mode;
  out->value = it->second.value;
  out->multiplier = it->second.multiplier;
  return ETERNALSONATA_ENEMY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEnemyOverrideCount(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<int>(g_overrides.size());
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetEnemyOverrides(
    EternalSonataEnemyOverride* out, int max) {
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_ENEMY_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (max == 0) {
    return static_cast<int>(g_overrides.size());
  }
  int written = 0;
  for (const auto& entry : g_overrides) {
    if (written >= max) {
      break;
    }
    EternalSonataEnemyOverride& row = out[written++];
    std::memset(&row, 0, sizeof(row));
    row.type = static_cast<int32_t>(entry.first / ETERNALSONATA_ENEMY_STAT_COUNT);
    row.stat = static_cast<int32_t>(entry.first % ETERNALSONATA_ENEMY_STAT_COUNT);
    row.mode = entry.second.mode;
    row.value = entry.second.value;
    row.multiplier = entry.second.multiplier;
  }
  return written;
}
