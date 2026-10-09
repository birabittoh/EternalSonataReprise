// eternalsonata - The per-character party arrays, twelve entries wide.
//
// The relocation is made on the first access, from whichever guest thread
// gets there, by copying the retail arrays across, so it cannot miss state
// the game wrote first.
//
// Entries 10 and 11 of one array sit at the retail address of the next one
// (position[10] is slotbytes[0], stats_live[10] is stats_base[0]), so the
// address alone cannot say which is meant. The instruction can: each one
// keeps reaching the same array, and while only ten characters exist every
// access it makes is unambiguous. So each instruction learns its array from
// its first access, and from then on an access just past that array's
// retail end is its entry 10 or 11. An instruction seen reaching two arrays
// (a helper given pointers into both) is only ever mapped by address.

#include "party_arrays.h"
#include "character_roster.h"
#include "ps3_appkeep.h"
#include "ps3_item_tables.h"
#include "shop_stock.h"
#include "target.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/memory/address_remap.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

uint32_t EternalSonataPartyRemap(uint32_t ea, const char* function, bool listed, uint32_t pc);

namespace eternalsonata {
namespace {

struct ArrayInfo {
  const char* name;
  uint32_t retail;
  uint32_t stride;
  // Nothing else lives in the two entries past the retail end, so an access
  // there can only mean this array, even from an instruction not seen before.
  bool free_extension = false;
};

constexpr std::array<ArrayInfo, static_cast<size_t>(PartyArray::kCount)> kArrays{{
    {"position", 0x8243FC08u, 4},
    {"slotbytes", 0x8243FC30u, 1},
    {"charflags", 0x8243FCFCu, 1, true},
    {"stats_live", 0x8243FD08u, 48},
    {"stats_base", 0x8243FEE8u, 48},
    {"charwords", 0x824400C8u, 2},
    {"template", 0x82016150u, 136},
    {"portrait_a", 0x8202CA28u, 2},
    {"portrait_b", 0x8202CA3Cu, 2, true},
}};

// The [address_remap] ranges: every array out to its twelfth entry.
constexpr rex::memory::GuestAddressRange kRanges[] = {
    {0x8243FC08u, 0x8243FC3Cu},
    {0x8243FCFCu, 0x8243FD08u},
    {0x8243FD08u, 0x82440128u},
    {0x82016150u, 0x820167B0u},
    {0x8202CA28u, 0x8202CA54u},
    {0x82560114u, 0x825601A2u},
};

// Crescendo's and Serenade's starting stats, extracted from the PS3 EBOOT
// by scripts/ps3_party_template.py.
constexpr uint8_t kPs3Templates[][136] = {
#include "ps3_party_template.inc"
};
constexpr size_t kPs3TemplateCount = std::size(kPs3Templates);

// New arrays keep their retail address modulo 16, so an aligned vector access
// that stays inside an array stays inside it after the move.
constexpr uint32_t kSlotSize = 0x800;

std::once_flag g_once;
std::atomic<uint32_t> g_block{0};

uint32_t RelocatedBase(size_t i, uint32_t block) {
  return block + static_cast<uint32_t>(i) * kSlotSize + (kArrays[i].retail & 0xF);
}

// The master item table: u32 flags at +4, bit 2 + c "character c may wear
// it" (docs/equipment.md). Image data, so written around its protection.
constexpr uint32_t kMasterTable = 0x82017630u;
constexpr uint32_t kMasterEnd = 0x82023DCCu;
constexpr uint32_t kMasterStride = 100;

// A modded character may wear what its base may; a vacant slot nothing.
void MirrorEquipBits(rex::memory::Memory* memory, int character, int base) {
  auto* heap = memory->LookupHeap(kMasterTable);
  uint32_t old_protect = 0;
  const uint32_t size = kMasterEnd - kMasterTable;
  if (!heap || !heap->Protect(kMasterTable, size,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect))
    return;
  const uint32_t own = 4u << character;
  for (uint32_t at = kMasterTable + 4; at < kMasterEnd; at += kMasterStride) {
    auto* flags = memory->TranslateVirtual<uint8_t*>(at);
    const uint32_t value = rex::memory::load_and_swap<uint32_t>(flags);
    const bool may = base && (value & (4u << base));
    rex::memory::store_and_swap<uint32_t>(flags, may ? value | own : value & ~own);
  }
  heap->Protect(kMasterTable, size, old_protect, nullptr);
}

// A modded slot's template row, camp faces and equipment start as its base's;
// a vacant 360 slot gets Allegretto's faces so a stray draw shows someone.
void SeedSlot(rex::memory::Memory* memory, uint32_t block, int character) {
  if (IsPs3Target())
    return;
  MirrorEquipBits(memory, character, IsModdedCharacter(character) ? CharacterBase(character) : 0);
  const int base = IsModdedCharacter(character) ? CharacterBase(character) : 1;
  const uint32_t c = static_cast<uint32_t>(character - 1);
  const uint32_t from = static_cast<uint32_t>(base - 1);
  for (PartyArray array : {PartyArray::kTemplate, PartyArray::kPortraitA, PartyArray::kPortraitB}) {
    const size_t i = static_cast<size_t>(array);
    if (array == PartyArray::kTemplate && !IsModdedCharacter(character))
      continue;
    auto* table = memory->TranslateVirtual<uint8_t*>(RelocatedBase(i, block));
    std::memcpy(table + kArrays[i].stride * c, table + kArrays[i].stride * from, kArrays[i].stride);
  }
}

void Relocate() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  const uint32_t block = memory->SystemHeapAlloc(kSlotSize * kArrays.size(), 16);
  if (!block) {
    REXLOG_ERROR("party arrays: could not allocate the relocated arrays");
    return;
  }
  for (size_t i = 0; i < kArrays.size(); ++i) {
    const ArrayInfo& a = kArrays[i];
    static_assert(kPartyCharacterCount * 136 + 0xF <= kSlotSize);
    auto* dst = memory->TranslateVirtual<uint8_t*>(RelocatedBase(i, block));
    std::memset(dst, 0, kPartyCharacterCount * a.stride);
    std::memcpy(dst, memory->TranslateVirtual<uint8_t*>(a.retail),
                kRetailCharacterCount * a.stride);
    if (static_cast<PartyArray>(i) == PartyArray::kTemplate && IsPs3Target())
      std::memcpy(dst + kRetailCharacterCount * a.stride, kPs3Templates,
                  kPs3TemplateCount * a.stride);
  }
  // Portraits: the PS3's own. The tables hold image ids, the AppKeep slot
  // plus one.
  if (IsPs3Target()) {
    for (uint32_t set = 0; set < 2; ++set) {
      const size_t i = static_cast<size_t>(PartyArray::kPortraitA) + set;
      auto* table = memory->TranslateVirtual<uint8_t*>(RelocatedBase(i, block));
      for (uint32_t c = kRetailCharacterCount; c < kPartyCharacterCount; ++c)
        rex::memory::store_and_swap<uint16_t>(
            table + 2 * c,
            static_cast<uint16_t>(kPs3PortraitSlot + 2 * set + (c - kRetailCharacterCount) + 1));
    }
  }
  for (uint32_t c = kRetailCharacterCount; c < kPartyCharacterCount; ++c)
    SeedSlot(memory, block, static_cast<int>(c + 1));
  g_block.store(block, std::memory_order_release);
  REXLOG_INFO("party arrays: relocated to {:08X}", block);
}

uint32_t Relocated(int array, uint32_t offset, uint32_t ea) {
  std::call_once(g_once, Relocate);
  const uint32_t block = g_block.load(std::memory_order_acquire);
  return block ? RelocatedBase(static_cast<size_t>(array), block) + offset : ea;
}

// Where an address lands: in an array's retail entries, past its retail end
// within twelve, or both (the next array's start).
struct Hit {
  int retail = -1;
  uint32_t retail_offset = 0;
  int extension = -1;
  uint32_t extension_offset = 0;
};

int StaticOwner(uint32_t pc, const Hit& hit) {
  // The status page can first touch live stats while showing character 11 or
  // 12, where their extension overlaps the base stats array.
  if (pc >= 0x8222FFE8u && pc < 0x82230560u &&
      hit.extension == static_cast<int>(PartyArray::kStatsLive))
    return hit.extension;
  if (pc >= 0x82232C10u && pc < 0x82233B38u &&
      (hit.extension == static_cast<int>(PartyArray::kStatsLive) ||
       hit.extension == static_cast<int>(PartyArray::kTemplate)))
    return hit.extension;
  // Status option text always comes from the selected character's live stats.
  if (pc >= 0x82234290u && pc < 0x82234404u &&
      hit.extension == static_cast<int>(PartyArray::kStatsLive))
    return hit.extension;
  // The battle member copy reads only live stats and the template; for 11 and
  // 12 live stats overlap base stats, so it would take Allegretto's magic.
  if (pc >= 0x821E7358u && pc < 0x821E7664u &&
      (hit.extension == static_cast<int>(PartyArray::kStatsLive) ||
       hit.extension == static_cast<int>(PartyArray::kTemplate)))
    return hit.extension;
  // The camp swap redraws level and EXP from base stats and HP from live
  // stats; for 11 and 12 those land on charwords and on Allegretto's base.
  if (hit.extension == static_cast<int>(PartyArray::kStatsBase)) {
    switch (pc) {
      case 0x82238BC8u:
      case 0x82238C70u:
      case 0x82238EACu:
      // and the other layouts' swaps
      case 0x82238038u:
      case 0x822380E8u:
      case 0x822385C8u:
      case 0x82238670u:
      case 0x822394D4u:
      case 0x822396BCu:
      case 0x82239BB0u:
      case 0x82239D48u:
        return hit.extension;
    }
  }
  if (hit.extension == static_cast<int>(PartyArray::kStatsLive)) {
    switch (pc) {
      case 0x82238C00u:
      case 0x82238C38u:
      case 0x82238CC4u:
      case 0x82238CC8u:
      case 0x82238EE4u:
      case 0x82238F1Cu:
      case 0x82238F3Cu:
      case 0x82238F40u:
      case 0x82238078u:
      case 0x822380B0u:
      case 0x8223813Cu:
      case 0x82238140u:
      case 0x82238600u:
      case 0x82238638u:
      case 0x822386C0u:
      case 0x822386CCu:
      case 0x8223950Cu:
      case 0x82239544u:
      case 0x82239598u:
      case 0x8223959Cu:
      case 0x822396F4u:
      case 0x8223972Cu:
      case 0x8223974Cu:
      case 0x82239750u:
      case 0x82239BE8u:
      case 0x82239C20u:
      case 0x82239C40u:
      case 0x82239C44u:
      case 0x82239D80u:
      case 0x82239DB8u:
      case 0x82239DD8u:
      case 0x82239DDCu:
      // sub_821E93B0, the level its magic list is filtered by
      case 0x821E9428u:
        return hit.extension;
    }
  }
  // sub_821DED50's six portrait_a loads, one per camp layout, and the camp
  // swap's; with 11 or 12 first they would land on portrait_b[0], Allegretto's.
  if (hit.extension == static_cast<int>(PartyArray::kPortraitA)) {
    switch (pc) {
      case 0x82238AD0u:
      case 0x821DEE6Cu:
      case 0x821DF5BCu:
      case 0x821DFD04u:
      case 0x821E0430u:
      case 0x821E0984u:
      case 0x821E0F68u:
      // sub_8221B5A0's four to six member rows, the same loads
      case 0x8221EEA8u:
      case 0x8221F624u:
      case 0x8221FCA4u:
      case 0x82220330u:
      case 0x82220804u:
      case 0x82220CD8u:
        return hit.extension;
    }
  }
  // Camp item use (sub_821F8F78..sub_821F9600) and its HP add (sub_821E8C78)
  // can first touch 11 or 12; their entries overlap the next array.
  if (pc >= 0x821F8F78u && pc < 0x821F9BD8u &&
      (hit.extension == static_cast<int>(PartyArray::kPosition) ||
       hit.extension == static_cast<int>(PartyArray::kStatsLive)))
    return hit.extension;
  // Its last store is the base copy; for 1 and 2 that overlaps live[10..11].
  if (pc >= 0x821E8C78u && pc < 0x821E8CE4u &&
      hit.extension == static_cast<int>(PartyArray::kStatsLive))
    return hit.extension;
  if (pc == 0x821E8CE4u && hit.extension == static_cast<int>(PartyArray::kStatsBase))
    return hit.extension;
  // The item target list (sub_8221B5A0) draws 11 and 12 too.
  if (pc >= 0x8221B5A0u && pc < 0x82222434u &&
      (hit.extension == static_cast<int>(PartyArray::kPosition) ||
       hit.extension == static_cast<int>(PartyArray::kStatsLive) ||
       (hit.extension == static_cast<int>(PartyArray::kPortraitB) && hit.retail < 0)))
    return hit.extension;
  // Closing the status page restores the camp cursor from position[c - 1];
  // for 11 and 12 that lands on slotbytes[0], which it would otherwise learn.
  if (pc == 0x822367DCu && hit.extension == static_cast<int>(PartyArray::kPosition))
    return hit.extension;
  return -1;
}

Hit Classify(uint32_t ea) {
  Hit hit;
  for (size_t i = 0; i < kArrays.size(); ++i) {
    const uint32_t offset = ea - kArrays[i].retail;
    if (offset < kRetailCharacterCount * kArrays[i].stride) {
      hit.retail = static_cast<int>(i);
      hit.retail_offset = offset;
    } else if (offset < kPartyCharacterCount * kArrays[i].stride) {
      hit.extension = static_cast<int>(i);
      hit.extension_offset = offset;
    }
  }
  return hit;
}

// What each instruction (or memcpy call site) has been seen reaching, in an
// open addressed table: pc in the high word, the array + 1 in the low byte.
constexpr uint64_t kMulti = 0x100;
constexpr uint64_t kReported = 0x200;
constexpr size_t kOwnerSlots = 1 << 14;
std::array<std::atomic<uint64_t>, kOwnerSlots> g_owners{};

std::atomic<uint64_t>* OwnerSlot(uint32_t pc) {
  size_t i = (pc * 0x9E3779B1u) >> 18;
  for (size_t probe = 0; probe < kOwnerSlots; ++probe, i = (i + 1) & (kOwnerSlots - 1)) {
    uint64_t v = g_owners[i].load(std::memory_order_relaxed);
    if (v >> 32 == pc)
      return &g_owners[i];
    if (!v) {
      if (g_owners[i].compare_exchange_strong(v, static_cast<uint64_t>(pc) << 32))
        return &g_owners[i];
      if (v >> 32 == pc)
        return &g_owners[i];
    }
  }
  return nullptr;
}

void SetBits(std::atomic<uint64_t>& slot, uint64_t bits) {
  slot.fetch_or(bits, std::memory_order_relaxed);
}

// Resolves an access by `pc` (0 when unknown) given where it lands.
uint32_t Resolve(uint32_t ea, uint32_t pc, const Hit& hit) {
  const auto by_address = [&] {
    return hit.retail >= 0 ? Relocated(hit.retail, hit.retail_offset, ea) : ea;
  };
  const int static_owner = StaticOwner(pc, hit);
  if (static_owner >= 0)
    return Relocated(static_owner, hit.extension_offset, ea);
  std::atomic<uint64_t>* slot = pc ? OwnerSlot(pc) : nullptr;
  if (!slot)
    return by_address();
  uint64_t state = slot->load(std::memory_order_relaxed);
  if (state & kMulti)
    return by_address();
  const int owner = static_cast<int>(state & 0xFF) - 1;
  if (owner < 0) {
    if (hit.retail < 0) {
      if (hit.extension < 0 || !kArrays[hit.extension].free_extension)
        return ea;
      SetBits(*slot, static_cast<uint64_t>(hit.extension + 1) | kReported);
      return Relocated(hit.extension, hit.extension_offset, ea);
    }
    SetBits(*slot, static_cast<uint64_t>(hit.retail + 1));
    return by_address();
  }
  if (owner == hit.retail)
    return by_address();
  if (owner == hit.extension) {
    if (!(state & kReported)) {
      SetBits(*slot, kReported);
      REXLOG_INFO("party arrays: {:08X} reaches {}[{}]", pc, kArrays[owner].name,
                  hit.extension_offset / kArrays[owner].stride);
    }
    return Relocated(owner, hit.extension_offset, ea);
  }
  if (hit.retail >= 0) {
    SetBits(*slot, kMulti);
    REXLOG_WARN("party arrays: {:08X} reaches both {} and {}, mapping it by address only", pc,
                kArrays[owner].name, kArrays[hit.retail].name);
  }
  return by_address();
}

void ReportUnlisted(const char* function) {
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  std::lock_guard lock(mutex);
  if (seen.emplace(function).second)
    REXLOG_WARN("party arrays: {} touches a party array but is not in config/party.toml",
                function);
}

// A memcpy or memset side resolves as one span: inside one retail array it
// learns and extends like an instruction, keyed by its call site; across
// several arrays (the save loader's copies) every byte keeps its own array.
uint32_t RemapCrt(uint32_t address, uint32_t caller, uint32_t span, uint32_t size) {
  const Hit hit = Classify(address);
  const Hit first = Classify(span);
  const Hit last = Classify(span + size - 1);
  const auto shares = [&](int a) { return a >= 0 && (a == last.retail || a == last.extension); };
  const bool one_array = shares(first.retail) || shares(first.extension);
  if (!one_array)
    return hit.retail >= 0 ? Relocated(hit.retail, hit.retail_offset, address) : address;
  // Learn and decide on the span's first byte, then follow it.
  const uint32_t key = caller | 2;
  if (address == span)
    return Resolve(address, key, hit);
  const uint32_t start = Resolve(span, key, first);
  return start == span ? (hit.retail >= 0 ? Relocated(hit.retail, hit.retail_offset, address)
                                          : address)
                       : start + (address - span);
}

}  // namespace

uint32_t PartyArrayAddress(PartyArray array, uint32_t index) {
  const size_t i = static_cast<size_t>(array);
  std::call_once(g_once, Relocate);
  const uint32_t block = g_block.load(std::memory_order_acquire);
  const uint32_t base = block ? RelocatedBase(i, block) : kArrays[i].retail;
  return base + index * kArrays[i].stride;
}

void SeedModdedSlot(int character) {
  const uint32_t block = g_block.load(std::memory_order_acquire);
  auto* runtime = rex::Runtime::instance();
  if (block && runtime && runtime->memory())
    SeedSlot(runtime->memory(), block, character);
}

namespace {

constexpr uint32_t kExtra = kPartyCharacterCount - kRetailCharacterCount;

// Entries 10 and 11 of every array the save holds, as sub_821E7138 left them.
std::array<std::string, static_cast<size_t>(PartyArray::kTemplate)> g_new_game;
std::mutex g_new_game_mutex;

std::string RecordKey(size_t i) {
  return std::string("party.") + kArrays[i].name;
}

uint8_t* ExtraEntries(size_t i) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  return memory ? memory->TranslateVirtual<uint8_t*>(
                      PartyArrayAddress(static_cast<PartyArray>(i), kRetailCharacterCount))
                : nullptr;
}

std::string Hex(const uint8_t* bytes, size_t size) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += kDigits[bytes[i] >> 4];
    out += kDigits[bytes[i] & 15];
  }
  return out;
}

bool Unhex(const std::string& text, uint8_t* bytes, size_t size) {
  if (text.size() != size * 2)
    return false;
  for (size_t i = 0; i < size; ++i) {
    const unsigned long v = std::strtoul(text.substr(i * 2, 2).c_str(), nullptr, 16);
    bytes[i] = static_cast<uint8_t>(v);
  }
  return true;
}

void CaptureNewGame() {
  std::lock_guard lock(g_new_game_mutex);
  for (size_t i = 0; i < g_new_game.size(); ++i)
    if (uint8_t* entries = ExtraEntries(i))
      g_new_game[i] = Hex(entries, kExtra * kArrays[i].stride);
}

}  // namespace

void SavePartyRecord(SaveRecord& record) {
  for (size_t i = 0; i < g_new_game.size(); ++i)
    if (const uint8_t* entries = ExtraEntries(i))
      record[RecordKey(i)] = Hex(entries, kExtra * kArrays[i].stride);
}

void LoadPartyRecord(const SaveRecord& record) {
  std::lock_guard lock(g_new_game_mutex);
  for (size_t i = 0; i < g_new_game.size(); ++i) {
    uint8_t* entries = ExtraEntries(i);
    if (!entries)
      continue;
    const size_t size = kExtra * kArrays[i].stride;
    const auto it = record.find(RecordKey(i));
    if (it != record.end() && Unhex(it->second, entries, size))
      continue;
    if (!Unhex(g_new_game[i], entries, size))
      std::memset(entries, 0, size);
  }
}

void InitPartyArrays(rex::Runtime* runtime) {
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  rex::memory::SetGuestAddressRemap(memory, kRanges, RemapCrt);
}

}  // namespace eternalsonata

uint32_t EternalSonataPartyRemap(uint32_t ea, const char* function, bool listed, uint32_t pc) {
  using namespace eternalsonata;
  if (const uint32_t icon = Ps3ItemIconAddress(ea, pc))
    return icon;
  if (ea >= 0x82560114u && ea < 0x825601A2u)
    return ShopStockAddress(ea, pc);
  const Hit hit = Classify(ea);
  if (hit.retail < 0 && hit.extension < 0)
    return ea;
  const uint32_t resolved = Resolve(ea, pc, hit);
  if (!listed && resolved != ea)
    ReportUnlisted(function);
  return resolved;
}

// Every new game and reset starts the characters here; what it leaves in the
// extra entries is what a save without a record loads with.
REX_EXTERN(__imp__sub_821E7138);
REX_HOOK_RAW(sub_821E7138) {
  __imp__sub_821E7138(ctx, base);
  eternalsonata::CaptureNewGame();
}
