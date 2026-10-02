// eternalsonata - Script natives only the PS3 executable has.
//
// PS3 scripts import party natives 5026..5033 that the 360's 5000 table (26
// entries) lacks; docs/ps3-assets.md section 2 has their PS3 addresses.
// Unregistered ids are worse than missing: sub_820FF748's range check is
// inclusive, so 5026 reads past off_8240CA88 into the battle native table.
//
// Characters are passed 0 based and are the same numbering as the 360's
// party arrays, so the roster and HP natives run against those directly.
// Characters 11 and 12 have no 360 storage and read as absent.
//
// The rest is the PS3's costumes (Allegretto, Polka and Beat only), which the
// costume system (costume_system.cpp) keeps with their locks, and two flags of
// its camp menu, which the 360 lacks, so they go into the save record
// (save_record.h).

#include "ps3_natives.h"

#include "generated/eternalsonata_init.h"
#include "costume_system.h"
#include "target.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace {

constexpr uint32_t kFirstId = 5026;
constexpr uint32_t kCount = 8;

// Same layout as party_system.cpp.
constexpr uint32_t kCharacters = 10;
constexpr uint32_t kPositionsAddr = 0x8243FC08u;
constexpr uint32_t kBaseStatsAddr = 0x8243FEE8u;
constexpr uint32_t kLiveStatsAddr = 0x8243FD08u;
constexpr uint32_t kStatsStride = 48u;
constexpr uint32_t kStatHp = 0x0Cu;
constexpr uint32_t kStatHpMax = 0x10u;

// The PS3 party block's camp menu flags at +0x920 and +0x921.
std::mutex g_mutex;
std::array<uint8_t, 2> g_menu_flags{};

uint32_t g_table = 0;

// The VM stack is in the physical heap, so arguments need the host offset.
uint32_t Load(uint8_t* base, uint32_t addr) {
  return REX_LOAD_U32(addr);
}

void Store(uint8_t* base, uint32_t addr, uint32_t value) {
  REX_STORE_U32(addr, value);
}

int32_t Arg(const PPCContext& ctx, uint8_t* base, uint32_t index) {
  return static_cast<int32_t>(Load(base, ctx.r3.u32 + 4 * index));
}

void Return(PPCContext& ctx, int32_t value) {
  ctx.r3.s64 = value;
}

int32_t Position(uint8_t* base, int32_t character) {
  if (character < 0 || character >= static_cast<int32_t>(kCharacters))
    return 0;
  return static_cast<int32_t>(Load(base, kPositionsAddr + 4 * character));
}

// 5026 (character, variant): unlock a costume.
void UnlockCostume(PPCContext& ctx, uint8_t* base) {
  eternalsonata::Ps3UnlockCostume(Arg(ctx, base, 0), Arg(ctx, base, 1));
  Return(ctx, 0);
}

// 5027 (character, variant): costume unlocked; variant 1 always is.
void HasCostume(PPCContext& ctx, uint8_t* base) {
  Return(ctx, eternalsonata::Ps3CostumeUnlocked(Arg(ctx, base, 0), Arg(ctx, base, 1)));
}

// 5028 (character): selected costume variant.
void SelectedCostume(PPCContext& ctx, uint8_t* base) {
  Return(ctx, eternalsonata::Ps3WornCostume(Arg(ctx, base, 0)));
}

// 5029 (x): camp menu flag +0x921 = x != 0.
void SetMenuFlag921(PPCContext& ctx, uint8_t* base) {
  const bool set = Arg(ctx, base, 0) != 0;
  std::lock_guard lock(g_mutex);
  g_menu_flags[1] = set;
  Return(ctx, 0);
}

// 5032 (x): camp menu flag +0x920 = x == 0.
void SetMenuFlag920(PPCContext& ctx, uint8_t* base) {
  const bool set = Arg(ctx, base, 0) == 0;
  std::lock_guard lock(g_mutex);
  g_menu_flags[0] = set;
  Return(ctx, 0);
}

// 5030 (character): has joined the party.
void HasJoined(PPCContext& ctx, uint8_t* base) {
  Return(ctx, Position(base, Arg(ctx, base, 0)) > 0);
}

// 5031 (character, delta): add to current HP, clamped to [1, max] as
// sub_1ECB60 does, in both stat copies. Returns the new HP.
void AddHp(PPCContext& ctx, uint8_t* base) {
  const int32_t character = static_cast<int16_t>(Arg(ctx, base, 0));
  const int32_t delta = Arg(ctx, base, 1);
  if (character < 0 || character >= static_cast<int32_t>(kCharacters))
    return Return(ctx, 0);
  const uint32_t live = kLiveStatsAddr + kStatsStride * character;
  const int32_t max = static_cast<int32_t>(Load(base, live + kStatHpMax));
  const int32_t hp = std::min(std::max(static_cast<int32_t>(Load(base, live + kStatHp)) + delta, 1), max);
  Store(base, live + kStatHp, hp);
  Store(base, kBaseStatsAddr + kStatsStride * character + kStatHp, hp);
  Return(ctx, hp);
}

// 5033 (character): in the active party (display positions 1..3).
void InActiveParty(PPCContext& ctx, uint8_t* base) {
  const int32_t position = Position(base, Arg(ctx, base, 0));
  Return(ctx, position >= 1 && position <= 3);
}

constexpr std::array<PPCFunc*, kCount> kNatives = {
    &UnlockCostume,    // 5026
    &HasCostume,       // 5027
    &SelectedCostume,  // 5028
    &SetMenuFlag921,   // 5029
    &HasJoined,        // 5030
    &AddHp,            // 5031
    &SetMenuFlag920,   // 5032
    &InActiveParty,    // 5033
};

uint32_t BuildTable() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (!memory || !dispatcher)
    return 0;
  const uint32_t table = memory->SystemHeapAlloc(4 * kCount, 0x20);
  if (!table)
    return 0;
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint32_t thunk = dispatcher->AllocateThunk(kNatives[i], 0);
    if (!thunk)
      return 0;
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(table + 4 * i), thunk);
  }
  return table;
}

// The PS3 runs 32 task lists where the 360 runs 16, so PS3 scripts name a PS3
// list when they spawn a task. Each 360 list maps to the PS3 list of the same
// engine tasks (docs/ps3-assets.md section 2); a PS3 list between two of those
// goes to the lower one, or to a 360 list no engine task uses, so tasks keep
// the PS3's run order against every engine task. 18 is where the PS3 engine
// spawns scripts and 20 its ObjectQueueTask, both in the 360's list 9.
constexpr std::array<uint8_t, 32> kTaskList = {
    0, 0, 0, 0, 1, 2, 2, 2, 3, 3, 3, 4, 5, 5, 5, 6,
    7, 8, 9, 9, 9, 10, 10, 10, 11, 12, 12, 12, 13, 14, 14, 15,
};

// Rewrites the priority argument in place; the VM pops it after the call.
void TranslateTaskList(const PPCContext& ctx, uint8_t* base, uint32_t index) {
  if (!eternalsonata::IsPs3Target())
    return;
  const uint32_t at = ctx.r3.u32 + 4 * index;
  const uint32_t list = Load(base, at);
  if (list < kTaskList.size())
    Store(base, at, kTaskList[list]);
}

std::string Bits(const uint8_t* bytes, size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i)
    out += bytes[i] ? '1' : '0';
  return out;
}

void ReadBits(const eternalsonata::SaveRecord& record, const char* key, uint8_t* bytes,
              size_t count) {
  const auto it = record.find(key);
  for (size_t i = 0; i < count; ++i)
    bytes[i] = it != record.end() && i < it->second.size() && it->second[i] == '1';
}

}  // namespace

namespace eternalsonata {

void ResetPs3Record() {
  std::lock_guard lock(g_mutex);
  g_menu_flags.fill(0);
}

void SavePs3Record(SaveRecord& record) {
  if (!IsPs3Target())
    return;
  std::lock_guard lock(g_mutex);
  record["ps3.camp_menu_flags"] = Bits(g_menu_flags.data(), g_menu_flags.size());
}

void LoadPs3Record(const SaveRecord& record) {
  std::lock_guard lock(g_mutex);
  ReadBits(record, "ps3.camp_menu_flags", g_menu_flags.data(), g_menu_flags.size());
}

}  // namespace eternalsonata

// Builtins 5, 6, 7 and 19 spawn a script task on the list in args[2], or
// args[3] for 6.
REX_EXTERN(__imp__sub_82102500);
REX_EXTERN(__imp__sub_82102518);
REX_EXTERN(__imp__sub_82102538);
REX_EXTERN(__imp__sub_821029A0);

REX_HOOK_RAW(sub_82102500) {
  TranslateTaskList(ctx, base, 2);
  __imp__sub_82102500(ctx, base);
}

REX_HOOK_RAW(sub_82102518) {
  TranslateTaskList(ctx, base, 3);
  __imp__sub_82102518(ctx, base);
}

REX_HOOK_RAW(sub_82102538) {
  TranslateTaskList(ctx, base, 2);
  __imp__sub_82102538(ctx, base);
}

REX_HOOK_RAW(sub_821029A0) {
  TranslateTaskList(ctx, base, 2);
  __imp__sub_821029A0(ctx, base);
}

// sub_820F91A8 (named MEMORY_HEAP__Init in config/rtti_names.toml) registers
// the field native tables. The extra table is added right after; sub_820FF028
// ignores a table it already holds, so a second call is harmless.
REX_EXTERN(__imp__MEMORY_HEAP__Init);

REX_HOOK_RAW(MEMORY_HEAP__Init) {
  PPCContext call = ctx;
  __imp__MEMORY_HEAP__Init(ctx, base);
  if (!eternalsonata::IsPs3Target())
    return;
  if (!g_table && !(g_table = BuildTable())) {
    REXLOG_ERROR("ps3 natives: could not build the native table");
    return;
  }
  // The table's count is inclusive of its last id, as sub_820FF748 checks it.
  call.r3.u64 = g_table;
  call.r4.u64 = kCount - 1;
  call.r5.u64 = kFirstId;
  sub_820FF028(call, base);
}
