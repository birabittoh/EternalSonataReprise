// eternalsonata - Party state: reading, writing, and the mod-facing API.
//
// Everything here was derived from the retail xex; docs/party-system.md is the
// long-form write-up, and the short version is:
//
//   * The party is ten fixed character slots, numbered 1..10 in the game's own
//     order (Allegretto, Polka, Beat, Frederic, Viola, Salsa, Jazz, Falsetto,
//     Claves, March). Every per-character table below is keyed by that number,
//     never by screen order.
//   * dword_8243FC08[c-1] is character c's 1-based *display position*, 0 when
//     it is not in the party. Positions 1..3 are the active party. It is the
//     same storage as IDA's dword_8243FC04[c] (1-based alias of the same
//     words), which is why the game's own routines appear to use two tables.
//   * Stats live in two parallel arrays of 48-byte structs: the character's own
//     stats at 0x8243FEE8, and the equipment-adjusted copy the screens draw at
//     0x8243FD08. sub_821E7898(c, live) recomputes the second from the first.
//   * Joining runs sub_821FBFC0 (own the character) then sub_821E6740 (roster
//     + party-level budget) then sub_820E78B8 (display position + battle party).
//     Leaving is sub_820E7948 (close the gap in the display order + battle
//     party). Both of the latter end in sub_821E6428, the battle-party rebuild.
//
// Threading: the exported entry points are called from mods, i.e. usually from
// the ImGui draw thread, where there is no guest ThreadState and a guest call
// crashes the game (see guest_main_thread.h). Reads are plain guest-memory
// loads and answer immediately; anything that has to run guest code is queued
// onto the guest main thread and reports ETERNALSONATA_PARTY_QUEUED.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>

#include "eternalsonata_party_api.h"
#include "guest_main_thread.h"
#include "party_relocation.h"
#include "party_slots.h"
#include "party_system.h"
#include "room_presence.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// Party block base. sub_821E6740 and the reset path sub_821E5D68 address
// everything below as offsets from here; the absolute forms are used instead
// because that is how they read in IDA.
constexpr uint32_t kPartyBase = 0x8243F3E8u;

// u32 party level, 1..6. Only the low byte is ever meaningful.
constexpr uint32_t kPartyLevelAddr = 0x8243F3ECu;
// u8 pair: how much of the party-level budget is left, and how much is spent.
// sub_821E6740 keeps them summing to the level's cap.
constexpr uint32_t kBudgetFreeAddr = 0x8243FCC4u;
constexpr uint32_t kBudgetUsedAddr = 0x8243FCC5u;
// u16[] indexed by party level - 1: the budget cap per level.
constexpr uint32_t kBudgetCapTableAddr = 0x8202CA70u;

// u32[12]: display position of character c at index c-1 (0 = not in party).
// Relocated along with every other per-character array; see party_relocation.h.
constexpr uint32_t kPositionsAddr = kPositionBase;

// 48-byte stat structs, indexed by character number - 1:
//   kBaseStatsAddr  the character's own stats (what a save holds)
//   kLiveStatsAddr  the same with equipment folded in - what the status and
//                   equipment screens draw
//
// Neither array lives at its retail address any more. Raising the cast to twelve
// moved every per-character array out of the original block, which sat back to
// back with no padding. The mid-ASM hooks rewrite the *guest code* that
// references them, which does nothing for the direct guest-memory reads below,
// so these constants have to follow the arrays to their new home by hand. See
// party_relocation.h.
constexpr uint32_t kBaseStatsAddr = kStatsBaseBase;
constexpr uint32_t kLiveStatsAddr = kStatsLiveBase;

// Offsets within a stat struct, all confirmed against the equipment screen's
// own painter sub_822352F8 (which draws +0x10, +0x0C, then +0x14/+0x16/+0x18/
// +0x1A) and against sub_821E7898's equipment maths.
constexpr uint32_t kStatLevel = 0x00u;   // u32
constexpr uint32_t kStatHp = 0x0Cu;      // u32, current
constexpr uint32_t kStatHpMax = 0x10u;   // u32
constexpr uint32_t kStatAttack = 0x14u;  // u16
constexpr uint32_t kStatMagic = 0x16u;   // u16
constexpr uint32_t kStatDefense = 0x18u;  // u16
constexpr uint32_t kStatSpeed = 0x1Au;   // u16
// The game clamps all four u16 stats here, so writes clamp the same way rather
// than storing a number the next recompute would immediately cut down.
constexpr uint32_t kStatMax = 999u;

// The owned-entity table sub_821FBFC0 fills and sub_821FBF20 queries: 512
// records of {u16 id, u8 count, u8 spoken for}, keyed by the same id space the
// characters live in, which is why a character has to be "owned" before it can
// join. dword_8255EED8 is the object that owns it and is what both routines
// take as their first argument.
constexpr uint32_t kOwnedTableObject = 0x8255EED8u;

// Guest routines. All are called through the typed imports below, never by
// address, so the recompiler resolves them at link time.
//   sub_821FBFC0(db, id, count, refresh)  give N of an entity (a character is
//                                         an entity) - the eligibility gate
//   sub_821E6740(id)                      roster add: validates against the
//                                         master table, charges the party-level
//                                         budget. 0 ok / 1 roster full /
//                                         2 budget / 3 not owned
//   sub_820E78B8(&index)                  give a character the next free
//                                         display position, then resync
//   sub_820E7948(&index)                  drop a character's position, closing
//                                         the gap behind it, then resync
//   sub_821E6428()                        rebuild the battle party from the
//                                         position table
//   sub_821E7898(c, live)                 recompute live stats from own stats
//                                         plus equipment
REX_IMPORT(__imp__sub_821FBFC0, g_own_entity, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__sub_821E6740, g_roster_add, u32(u32));
REX_IMPORT(__imp__sub_820E78B8, g_place_member, u32(u32));
REX_IMPORT(__imp__sub_820E7948, g_unplace_member, u32(u32));
REX_IMPORT(__imp__sub_821E6428, g_rebuild_battle_party, u32());
REX_IMPORT(__imp__sub_821E7898, g_refresh_stats, u32(u32, u32));

constexpr int kCharacterCount = ETERNALSONATA_CHARACTER_COUNT;

// How many characters the retail game itself has, as opposed to how many slots
// exist now. The two differ since the cast was widened to twelve, and the
// difference matters wherever this file reads one of the *game's* own ten-entry
// tables rather than one of the widened ones.
constexpr int kNativeCharacterCount = ETERNALSONATA_NATIVE_CHARACTER_COUNT;
static_assert(kCharacterCount >= kNativeCharacterCount,
              "the cast cannot be smaller than the game's own");

// The cast in character-number order, as the game's own text blocks store it.
// Note Polka is 2, Beat 3, Frederic 4.
//
// Only the retail cast is in here. The added slots have no name of their own:
// they are vacant until a mod defines one, and the definition is what supplies
// the name (see party_slots.h). The host inventing a "Character 11" would be
// the host inventing content for a slot that exists precisely so a mod can.
constexpr const char* kDefaultNames[kNativeCharacterCount + 1] = {
    "",      "Allegretto", "Polka",    "Beat",   "Frederic", "Viola",
    "Salsa", "Jazz",       "Falsetto", "Claves", "March"};

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;
// Per character slot: the name to answer text lookups with, empty for the
// game's own, and the guest strings backing it (plain and ruby forms).
std::array<std::string, kCharacterCount + 1> g_names;
std::array<uint32_t, kCharacterCount + 1> g_name_guest{};
std::array<uint32_t, kCharacterCount + 1> g_name_guest_ruby{};
// Guest address -> character slot, built once from the game's own text blocks.
// See DiscoverNameStrings.
struct NameString {
  uint32_t address = 0;
  int slot = 0;
  bool ruby = false;
  // The bytes that were there at boot, so a rename can be undone, and so the
  // in-place patch knows how much room it has.
  std::string original;
};
std::vector<NameString> g_name_strings;
bool g_name_strings_scanned = false;
// A 16-byte guest scratch buffer for the two routines that take a pointer to
// their argument. Allocated on first use, from the guest thread.
uint32_t g_scratch = 0;

rex::memory::Memory* Mem() { return g_runtime ? g_runtime->memory() : nullptr; }

// ---------------------------------------------------------------------------
// Guest memory access
// ---------------------------------------------------------------------------

bool Readable(uint32_t address, uint32_t span) {
  auto* memory = Mem();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + span - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

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

void WriteGuestString(uint32_t address, const std::string& text) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    std::memcpy(host, text.c_str(), text.size() + 1);
  }
}

// ---------------------------------------------------------------------------
// Party queries (guest memory only, safe from any thread)
// ---------------------------------------------------------------------------

uint32_t PositionAddr(int slot) { return kPositionsAddr + 4u * (slot - 1); }

int PositionOf(int slot) {
  return static_cast<int>(ReadGuest<uint32_t>(PositionAddr(slot)));
}

int PartySize() {
  int n = 0;
  for (int c = 1; c <= kCharacterCount; ++c) {
    if (PositionOf(c) != 0) {
      ++n;
    }
  }
  return n;
}

// "Available" means a save is actually loaded. The tables are readable but
// blank at the title screen, and every guest routine below misbehaves against
// blank party state, so an empty party counts as unavailable rather than as an
// empty one: a loaded save always has at least the leader in it.
bool Available() {
  return Readable(kPartyBase, 8) && Readable(kLiveStatsAddr, kStatsStride * kCharacterCount) &&
         PartySize() > 0;
}

bool Editable() { return Available() && !GetRoomPresence().IsBattleActive(); }

// ---------------------------------------------------------------------------
// Character resolution
// ---------------------------------------------------------------------------

// Maps an API `character` to the character number it means, or a negative
// ETERNALSONATA_PARTY_ERR_* for anything outside the cast. Callers hold
// g_mutex.
int SlotForLocked(int character) {
  if (character >= 1 && character <= kCharacterCount) {
    return character;
  }
  return ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER;
}

// Same, but refuses an added slot no mod has defined. Everything that puts a
// character into the world - joining, stat writes, healing - goes through this
// one rather than through SlotForLocked, so a vacant slot is inert no matter
// which entry point is called. Reads stay permissive: answering "position 0,
// stats zero" for a vacant slot is both true and useful to a mod enumerating
// the cast.
int DefinedSlotForLocked(int character) {
  const int slot = SlotForLocked(character);
  if (slot > 0 && !IsCharacterDefined(slot)) {
    return ETERNALSONATA_PARTY_ERR_SLOT_VACANT;
  }
  return slot;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// The cast's names are not stored once. They appear as a run of ten
// NUL-terminated strings at the head of each language block of the packed UI
// message blob (around 0x82033000 and up), again with the "<r>" ruby marker in
// front of each, and again in a second family of blocks near 0x82385900 that
// carry their own {id, offset} index and are what the battle side reads. The
// blocks are found by content rather than by address, both because this title
// applies a title-update delta patch over the base image and because that is
// the only way to catch every copy.
//
// Only the first four names are the same in every language - the Italian block
// spells Viola "Arpa" and Falsetto "Mazurka" - so that run is the anchor. One
// family of blocks calls the fourth character Chopin rather than Frederic, so
// both spellings anchor.
void DiscoverNameStringsLocked() {
  if (g_name_strings_scanned) {
    return;
  }
  g_name_strings_scanned = true;
  auto* memory = Mem();
  if (!memory) {
    g_name_strings_scanned = false;
    return;
  }

  struct Anchor {
    const char* bytes;
    size_t length;
    bool ruby;
  };
  static constexpr char kPlain[] = "Allegretto\0Polka\0Beat\0Frederic";
  static constexpr char kPlainChopin[] = "Allegretto\0Polka\0Beat\0Chopin";
  static constexpr char kRuby[] = "<r>Allegretto\0<r>Polka\0<r>Beat";
  static const Anchor kAnchors[] = {
      {kPlain, sizeof(kPlain) - 1, false},
      {kPlainChopin, sizeof(kPlainChopin) - 1, false},
      {kRuby, sizeof(kRuby) - 1, true},
  };
  size_t longest = 0;
  for (const Anchor& anchor : kAnchors) {
    longest = std::max(longest, anchor.length);
  }

  // The name blocks live in the image's data, which stretches well past the
  // message blob, so the whole image is swept a page at a time; an unmapped
  // hole is skipped rather than faulted on.
  constexpr uint32_t kScanBegin = 0x82000000u;
  constexpr uint32_t kScanEnd = 0x82600000u;
  constexpr uint32_t kChunk = 0x1000u;

  for (uint32_t page = kScanBegin; page < kScanEnd; page += kChunk) {
    // Overlap by the longest anchor so a match straddling two pages is still
    // found.
    const uint32_t span = kChunk + static_cast<uint32_t>(longest);
    if (!Readable(page, span)) {
      continue;
    }
    const auto* host = memory->TranslateVirtual<const uint8_t*>(page);
    if (!host) {
      continue;
    }
    for (uint32_t i = 0; i < kChunk; ++i) {
      const uint8_t* at = host + i;
      bool ruby = false;
      bool hit = false;
      for (const Anchor& anchor : kAnchors) {
        if (std::memcmp(at, anchor.bytes, anchor.length) == 0) {
          ruby = anchor.ruby;
          hit = true;
          break;
        }
      }
      if (!hit) {
        continue;
      }
      // Walk the ten names of this block, recording where each one starts and
      // what it says, and refuse anything that does not look like a name so a
      // coincidental match cannot lead the patcher into unrelated bytes.
      //
      // Ten, not kCharacterCount: these are the game's own packed name tables
      // and they hold one string per *retail* character. Demanding twelve here
      // would reject every real block and silently turn off name discovery for
      // the ten characters that do have names.
      uint32_t address = page + i;
      NameString found[kNativeCharacterCount];
      int count = 0;
      for (int slot = 1; slot <= kNativeCharacterCount; ++slot) {
        const auto* text = memory->TranslateVirtual<const char*>(address);
        if (!text) {
          break;
        }
        const size_t length = std::strlen(text);
        if (length == 0 || length > 32) {
          break;
        }
        found[count++] = NameString{address, slot, ruby, std::string(text, length)};
        address += static_cast<uint32_t>(length) + 1;
      }
      if (count != kNativeCharacterCount) {
        continue;
      }
      for (const NameString& name : found) {
        g_name_strings.push_back(name);
      }
    }
  }
}

// The name a slot answers to: its own override, or the built-in one.
// Callers hold g_mutex.
const std::string& NameForSlotLocked(int slot) {
  static const std::string kEmpty;
  if (slot < 1 || slot > kCharacterCount) {
    return kEmpty;
  }
  return g_names[slot];
}

// Publishes a slot's override into guest memory, in both the plain and ruby
// forms, so the text-lookup hook has something to return. Callers hold
// g_mutex.
void PublishNameLocked(int slot) {
  auto* memory = Mem();
  if (!memory || slot < 1 || slot > kCharacterCount) {
    return;
  }
  const std::string& name = g_names[slot];
  if (name.empty()) {
    return;  // nothing to publish; the hook falls through to the game's own
  }
  // 64 bytes is far more than the widest name the screens have room for, and
  // the allocation is per slot and permanent, so a rename reuses it.
  if (!g_name_guest[slot]) {
    g_name_guest[slot] = memory->SystemHeapAlloc(64, 0x20);
  }
  if (!g_name_guest_ruby[slot]) {
    g_name_guest_ruby[slot] = memory->SystemHeapAlloc(64, 0x20);
  }
  std::string plain = name.substr(0, 63);
  if (g_name_guest[slot]) {
    WriteGuestString(g_name_guest[slot], plain);
  }
  if (g_name_guest_ruby[slot]) {
    WriteGuestString(g_name_guest_ruby[slot], "<r>" + plain.substr(0, 60));
  }
}

// ---------------------------------------------------------------------------
// Guest-side work
// ---------------------------------------------------------------------------

// Runs `work` where guest calls are legal: right here if we are already on the
// guest main thread, otherwise on its next frame. See guest_main_thread.h for
// why a guest call from the ImGui thread is not an option.
int RunOnGuestThread(std::function<int()> work) {
  if (OnGuestMainThread()) {
    return work();
  }
  PostToGuestMainThread([work] { work(); });
  return ETERNALSONATA_PARTY_QUEUED;
}

// The guest scratch argument for sub_820E78B8 / sub_820E7948, which both take
// a pointer to the character index rather than the index itself. Guest thread
// only.
uint32_t ScratchArg(uint32_t value) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  if (!g_scratch) {
    g_scratch = memory->SystemHeapAlloc(16, 0x20);
  }
  if (g_scratch) {
    WriteGuest<uint32_t>(g_scratch, value);
  }
  return g_scratch;
}

// The join sequence, on the guest thread. `slot` is a character number.
int JoinOnGuestThread(int slot) {
  // Own the character. Characters share the entity id space with items, so the
  // roster add's eligibility gate (sub_821FBF20) is really "do you have one of
  // these" - which is false for anyone the story has not handed you yet.
  g_own_entity(kOwnedTableObject, static_cast<u32>(slot), 1, 0);

  const u32 result = g_roster_add(static_cast<u32>(slot));
  switch (result) {
    case 1:
      return ETERNALSONATA_PARTY_ERR_ROSTER_FULL;
    case 2:
      return ETERNALSONATA_PARTY_ERR_PARTY_LEVEL;
    case 3:
      return ETERNALSONATA_PARTY_ERR_NOT_ELIGIBLE;
    default:
      break;
  }

  // sub_821E6740 fills the roster and the budget but knows nothing about the
  // display order, so the position comes from the party menu's own routine -
  // which also rebuilds the battle party.
  const uint32_t arg = ScratchArg(static_cast<uint32_t>(slot - 1));
  if (!arg) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  g_place_member(arg);
  return ETERNALSONATA_PARTY_OK;
}

int LeaveOnGuestThread(int slot) {
  const uint32_t arg = ScratchArg(static_cast<uint32_t>(slot - 1));
  if (!arg) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  g_unplace_member(arg);
  return ETERNALSONATA_PARTY_OK;
}

int RefreshStatsOnGuestThread(int slot) {
  g_refresh_stats(static_cast<u32>(slot), kLiveStatsAddr + kStatsStride * (slot - 1));
  return ETERNALSONATA_PARTY_OK;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

void ReadStats(uint32_t base_address, int slot, EternalSonataCharacterStats* out) {
  const uint32_t at = base_address + kStatsStride * (slot - 1);
  std::memset(out, 0, sizeof(*out));
  out->level = static_cast<int32_t>(ReadGuest<uint32_t>(at + kStatLevel));
  out->hp = static_cast<int32_t>(ReadGuest<uint32_t>(at + kStatHp));
  out->hp_max = static_cast<int32_t>(ReadGuest<uint32_t>(at + kStatHpMax));
  out->attack = ReadGuest<uint16_t>(at + kStatAttack);
  out->magic = ReadGuest<uint16_t>(at + kStatMagic);
  out->defense = ReadGuest<uint16_t>(at + kStatDefense);
  out->speed = ReadGuest<uint16_t>(at + kStatSpeed);
}

uint32_t ClampU32(int32_t value) { return value < 0 ? 0u : static_cast<uint32_t>(value); }

uint16_t ClampStat(int32_t value) {
  if (value < 0) {
    return 0;
  }
  return static_cast<uint16_t>(std::min<uint32_t>(static_cast<uint32_t>(value), kStatMax));
}

void WriteStats(uint32_t base_address, int slot, const EternalSonataCharacterStats& stats) {
  const uint32_t at = base_address + kStatsStride * (slot - 1);
  WriteGuest<uint32_t>(at + kStatLevel, ClampU32(stats.level));
  WriteGuest<uint32_t>(at + kStatHp, ClampU32(stats.hp));
  WriteGuest<uint32_t>(at + kStatHpMax, ClampU32(stats.hp_max));
  WriteGuest<uint16_t>(at + kStatAttack, ClampStat(stats.attack));
  WriteGuest<uint16_t>(at + kStatMagic, ClampStat(stats.magic));
  WriteGuest<uint16_t>(at + kStatDefense, ClampStat(stats.defense));
  WriteGuest<uint16_t>(at + kStatSpeed, ClampStat(stats.speed));
}

// Writing the character's own stats alone would leave the screens showing the
// old numbers until something else recomputed them, and recomputing scales
// current HP by however much maximum HP moved. Mirroring the new values into
// the live struct first makes that ratio exactly 1, so the recompute only adds
// the equipment bonus back.
void ApplyStats(int slot, const EternalSonataCharacterStats& stats) {
  WriteStats(kBaseStatsAddr, slot, stats);
  WriteStats(kLiveStatsAddr, slot, stats);
  RunOnGuestThread([slot] { return RefreshStatsOnGuestThread(slot); });
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindPartySystem(rex::Runtime* runtime) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_runtime = runtime;
}

uint32_t PartyNameOverrideFor(uint32_t text_address) {
  if (!text_address) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_runtime) {
    return 0;
  }
  // Nothing to do until some mod actually renames somebody, which keeps the
  // one-off scan out of the boot path entirely for the common case.
  bool any = false;
  for (int slot = 1; slot <= kCharacterCount; ++slot) {
    if (!g_names[slot].empty()) {
      any = true;
      break;
    }
  }
  if (!any) {
    return 0;
  }
  DiscoverNameStringsLocked();
  for (const NameString& name : g_name_strings) {
    if (name.address != text_address) {
      continue;
    }
    if (g_names[name.slot].empty()) {
      return 0;
    }
    return name.ruby ? g_name_guest_ruby[name.slot] : g_name_guest[name.slot];
  }
  return 0;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (see src/eternalsonata_party_api.h)
// ---------------------------------------------------------------------------

using namespace eternalsonata;

namespace {

// Resolves an API character to a character number, or a negative error.
int ResolveSlot(int character) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int slot = SlotForLocked(character);
  if (slot < 0) {
    return slot;
  }
  if (slot == 0) {
    return ETERNALSONATA_PARTY_ERR_INVALID_CHARACTER;
  }
  return slot;
}

// The same, for the entry points that write: a vacant slot is refused rather
// than quietly written to.
int ResolveDefinedSlot(int character) {
  const int slot = ResolveSlot(character);
  if (slot > 0 && !IsCharacterDefined(slot)) {
    return ETERNALSONATA_PARTY_ERR_SLOT_VACANT;
  }
  return slot;
}

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataPartyAbiVersion(void) {
  return ETERNALSONATA_PARTY_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsPartyAvailable(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Available() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsPartyEditable(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Editable() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetCharacterName(int character) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int slot = SlotForLocked(character);
  if (slot < 1 || slot > kCharacterCount) {
    return "";
  }
  const std::string& name = NameForSlotLocked(slot);
  if (!name.empty()) {
    return name.c_str();
  }
  // An added slot has no name of its own. A defined one always has an override,
  // since defining it goes through the rename path; a vacant one answers "",
  // which is what a caller listing the cast should skip over - use
  // EternalSonataIsCharacterDefined to tell vacant from named.
  return slot <= kNativeCharacterCount ? kDefaultNames[slot] : "";
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPartySize(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  return PartySize();
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPartyMembers(int* out, int max) {
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  // Walk positions rather than slots so the result is in the order the game
  // draws them. A position the game never assigned simply has no member.
  int written = 0;
  for (int position = 1; position <= kCharacterCount; ++position) {
    for (int slot = 1; slot <= kCharacterCount; ++slot) {
      if (PositionOf(slot) != position) {
        continue;
      }
      if (written < max) {
        out[written] = slot;
      }
      ++written;
      break;
    }
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsCharacterInParty(int character) {
  const int slot = ResolveSlot(character);
  if (slot < 0) {
    return 0;
  }
  return slot != 0 && PositionOf(slot) != 0 ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsCharacterActive(int character) {
  const int slot = ResolveSlot(character);
  if (slot <= 0) {
    return 0;
  }
  const int position = PositionOf(slot);
  return position >= 1 && position <= ETERNALSONATA_ACTIVE_PARTY_SIZE ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetCharacterPosition(int character) {
  const int slot = ResolveSlot(character);
  if (slot < 0) {
    return slot;
  }
  if (slot == 0) {
    return 0;
  }
  return PositionOf(slot);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPartyLevel(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  return static_cast<int>(ReadGuest<uint32_t>(kPartyLevelAddr));
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPartyLevelBudget(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  const int level = static_cast<int>(ReadGuest<uint32_t>(kPartyLevelAddr));
  if (level < 1 || level > 6) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  return ReadGuest<uint16_t>(kBudgetCapTableAddr + 2u * (level - 1)) & 0xFF;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPartyLevelBudgetUsed(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  return ReadGuestByte(kBudgetUsedAddr);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPartyLevelBudgetFree(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  return ReadGuestByte(kBudgetFreeAddr);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetCharacterStats(
    int character, EternalSonataCharacterStats* out) {
  if (!out) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  const int slot = ResolveSlot(character);
  if (slot < 0) {
    return slot;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  ReadStats(kLiveStatsAddr, slot, out);
  return ETERNALSONATA_PARTY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetCharacterBaseStats(
    int character, EternalSonataCharacterStats* out) {
  if (!out) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  const int slot = ResolveSlot(character);
  if (slot < 0) {
    return slot;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  ReadStats(kBaseStatsAddr, slot, out);
  return ETERNALSONATA_PARTY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCharacterStats(
    int character, const EternalSonataCharacterStats* stats) {
  if (!stats) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  const int slot = ResolveDefinedSlot(character);
  if (slot < 0) {
    return slot;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  ApplyStats(slot, *stats);
  return ETERNALSONATA_PARTY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataHealCharacter(int character) {
  const int slot = ResolveDefinedSlot(character);
  if (slot < 0) {
    return slot;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  EternalSonataCharacterStats stats{};
  ReadStats(kBaseStatsAddr, slot, &stats);
  stats.hp = stats.hp_max;
  ApplyStats(slot, stats);
  return ETERNALSONATA_PARTY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataHealParty(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  for (int slot = 1; slot <= kCharacterCount; ++slot) {
    if (PositionOf(slot) == 0) {
      continue;
    }
    EternalSonataCharacterStats stats{};
    ReadStats(kBaseStatsAddr, slot, &stats);
    stats.hp = stats.hp_max;
    ApplyStats(slot, stats);
  }
  return ETERNALSONATA_PARTY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataAddCharacterToParty(int character) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  if (!Editable()) {
    return ETERNALSONATA_PARTY_ERR_IN_BATTLE;
  }

  const int slot = DefinedSlotForLocked(character);
  if (slot < 0) {
    return slot;
  }
  if (PositionOf(slot) != 0) {
    return ETERNALSONATA_PARTY_ERR_ALREADY_IN_PARTY;
  }

  return RunOnGuestThread([slot] {
    const int joined = JoinOnGuestThread(slot);
    if (joined != ETERNALSONATA_PARTY_OK) {
      return joined;
    }
    // A definition may carry starting stats, which are the mod's answer to the
    // stat template not being able to express everything. They are applied once,
    // on the first join, so a character that has since levelled up is not reset
    // when it is benched and brought back.
    EternalSonataCharacterStats starting{};
    if (TakeStartingStats(slot, &starting)) {
      WriteStats(kBaseStatsAddr, slot, starting);
      WriteStats(kLiveStatsAddr, slot, starting);
    }
    RefreshStatsOnGuestThread(slot);
    return joined;
  });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataRemoveCharacterFromParty(int character) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  if (!Editable()) {
    return ETERNALSONATA_PARTY_ERR_IN_BATTLE;
  }
  const int slot = SlotForLocked(character);
  if (slot < 0) {
    return slot;
  }
  if (PositionOf(slot) == 0) {
    return ETERNALSONATA_PARTY_ERR_NOT_IN_PARTY;
  }
  return RunOnGuestThread([slot] { return LeaveOnGuestThread(slot); });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCharacterPosition(int character,
                                                                      int position) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  if (!Editable()) {
    return ETERNALSONATA_PARTY_ERR_IN_BATTLE;
  }
  const int slot = SlotForLocked(character);
  if (slot < 0) {
    return slot;
  }
  if (slot == 0 || PositionOf(slot) == 0) {
    return ETERNALSONATA_PARTY_ERR_NOT_IN_PARTY;
  }
  const int size = PartySize();
  if (position < 1 || position > size) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  const int current = PositionOf(slot);
  if (current == position) {
    return ETERNALSONATA_PARTY_OK;
  }

  // Exchange with whoever holds the target position. A rotation would also be
  // valid, but an exchange is what the game's own "swap two members" does and
  // it cannot leave a gap in the order.
  for (int other = 1; other <= kCharacterCount; ++other) {
    if (PositionOf(other) == position) {
      WriteGuest<uint32_t>(PositionAddr(other), static_cast<uint32_t>(current));
      break;
    }
  }
  WriteGuest<uint32_t>(PositionAddr(slot), static_cast<uint32_t>(position));
  REXLOG_INFO("party: character {} moved from position {} to {}, rebuilding battle party",
              slot, current, position);
  return RunOnGuestThread([] {
    g_rebuild_battle_party();
    REXLOG_INFO("party: battle party rebuild returned");
    return ETERNALSONATA_PARTY_OK;
  });
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSwapCharacterPositions(int a, int b) {
  const int position_a = EternalSonataGetCharacterPosition(a);
  if (position_a < 0) {
    return position_a;
  }
  if (position_a == 0) {
    return ETERNALSONATA_PARTY_ERR_NOT_IN_PARTY;
  }
  return EternalSonataSetCharacterPosition(b, position_a);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetPartyLevel(int level) {
  if (level < 1 || level > 6) {
    return ETERNALSONATA_PARTY_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Available()) {
    return ETERNALSONATA_PARTY_ERR_UNAVAILABLE;
  }
  WriteGuest<uint32_t>(kPartyLevelAddr, static_cast<uint32_t>(level));
  // Rebuild the budget the way sub_821E6740 leaves it: the level's cap, minus
  // what the current members already spend. Spend is left as the game recorded
  // it, since it is charged per member as each one joins.
  const uint32_t cap = ReadGuest<uint16_t>(kBudgetCapTableAddr + 2u * (level - 1)) & 0xFF;
  const uint32_t used = ReadGuestByte(kBudgetUsedAddr);
  WriteGuestByte(kBudgetUsedAddr, static_cast<uint8_t>(std::min(used, cap)));
  WriteGuestByte(kBudgetFreeAddr, static_cast<uint8_t>(cap - std::min(used, cap)));
  return ETERNALSONATA_PARTY_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetCharacterName(int character,
                                                                  const char* name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int slot = SlotForLocked(character);
  if (slot < 0) {
    return slot;
  }
  g_names[slot] = name ? name : "";
  PublishNameLocked(slot);
  return ETERNALSONATA_PARTY_OK;
}

