// eternalsonata - AppKeep.bmd from the PS3.
//
// sub_82162058 loads AppKeep.bmd into the APPKEEP heap and copies its entry
// pointers into dword_82420AFC, which executable code and data tables index
// by 360 slot. The PS3 file has its own slot order (docs/ps3-assets.md
// section 4), so in PS3 mode the array is rebuilt in the 360's numbering:
//
//   0..9     field characters, which the PS3 keeps in pc*_v1.p3obj (ALG,
//            PLK, BET) and appkeep2.bmd (the rest, plus CRS and SRN)
//   camp     portraits, gems, skill icons and three effects the PS3 moved
//            into campdata/camp_char.bmd
//   the rest the PS3 file's own entries, slot by slot
//
// PS3 only entries follow the 360's 412 slots. Scripts name slots in the
// PS3's numbering (native 1052), so those are translated.

#include "ps3_appkeep.h"

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

// sub_821125A0(heap, size, align): allocates from one heap.
REX_EXTERN(sub_821125A0);

namespace {

constexpr uint32_t kHeap = 0x82420948u;
constexpr uint32_t kSlotArray = eternalsonata::kAppKeepSlotArray;
constexpr uint32_t kSlots360 = 412;
constexpr uint32_t kSlotsPs3 = 374;
constexpr uint32_t kAlign = 0x1000;

enum class Source : uint8_t { kAppKeep, kAppKeep2, kCampChar, kCharacter };

// 360 slots [slot, slot + count) hold source entries [first, first + count).
struct Run {
  uint16_t slot;
  uint16_t count;
  Source source;
  uint16_t first;
};

// Matched by content against the 360 file: models and effects by their
// sections, textures by their decoded image. 360 slots 164, 165, 194 and 245
// have no PS3 counterpart and nothing in the executable names them.
constexpr Run kRuns[] = {
    {0, 3, Source::kCharacter, 0},  {3, 7, Source::kAppKeep2, 2},
    {10, 154, Source::kAppKeep, 9}, {170, 24, Source::kAppKeep, 169},
    {195, 10, Source::kAppKeep, 194}, {205, 3, Source::kCampChar, 0},
    {208, 1, Source::kCampChar, 50}, {209, 3, Source::kCampChar, 3},
    {212, 7, Source::kCampChar, 32}, {219, 3, Source::kCampChar, 6},
    {222, 7, Source::kCampChar, 41}, {229, 1, Source::kCampChar, 51},
    {230, 15, Source::kAppKeep, 235}, {246, 17, Source::kAppKeep, 251},
    {263, 20, Source::kAppKeep, 270}, {283, 4, Source::kCampChar, 55},
    {287, 3, Source::kCampChar, 52}, {290, 17, Source::kAppKeep, 299},
    {307, 3, Source::kCampChar, 60}, {310, 36, Source::kAppKeep, 319},
    {346, 1, Source::kCampChar, 59}, {347, 10, Source::kAppKeep, 356},
};

// PS3 entries with no 360 slot, at 360 slot 412 on: art for CRS and SRN
// (204, 205, 268, 269, 290, 291), the costume menu's icons (366, 367, 372, 373)
// and their equipment icons (368..371).
constexpr uint16_t kPs3Only[] = {204, 205, 268, 269, 290, 291, 366,
                                 367, 368, 369, 370, 371, 372, 373};
static_assert(kSlots360 == eternalsonata::kPs3MenuPortraitSlot && kPs3Only[0] == 204);
// CRS and SRN's two camp portraits, after the ten of each set (camp_char 32..38
// and 41..47 hold characters 3..9), at kPs3PortraitSlot on.
constexpr uint16_t kCampPortraits[] = {39, 40, 48, 49};
static_assert(kSlots360 + std::size(kPs3Only) == eternalsonata::kPs3PortraitSlot);
// The costumes' status portraits (camp_char 3 * kind + character + 11, kind 3,
// and 27 for PLK v3), in costume_system's order.
constexpr uint16_t kCostumePortraits[] = {21, 22, 27, 23};
static_assert(eternalsonata::kPs3PortraitSlot + std::size(kCampPortraits) ==
              eternalsonata::kPs3CostumePortraitSlot);
static_assert(eternalsonata::kPs3CostumePortraitSlot + std::size(kCostumePortraits) ==
              eternalsonata::kFirstFreeAppKeepSlot);
static_assert(eternalsonata::kFirstFreeAppKeepSlot <= eternalsonata::kAppKeepSlotCount);

constexpr const char* kCharacterFiles[] = {"pcalg_v1.p3obj", "pcplk_v1.p3obj",
                                           "pcbet_v1.p3obj"};
constexpr uint32_t kAppKeep2Entries = 9;

std::array<uint16_t, kSlotsPs3> g_ps3_to_360{};
std::array<uint32_t, kAppKeep2Entries> g_appkeep2{};

uint32_t ReadBe32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
         (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
}

bool ReadFile(const std::string& name, std::vector<uint8_t>& out) {
  std::ifstream in(eternalsonata::GameDataRoot() / name, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// A .bmd's entries as [begin, end) file ranges; an empty one for a null slot.
struct Bmd {
  std::vector<uint8_t> data;
  std::vector<std::pair<uint32_t, uint32_t>> entries;

  bool Load(const std::string& name) {
    if (!ReadFile(name, data) || data.size() < 12 || (std::memcmp(data.data(), "BMD ", 4) != 0 && std::memcmp(data.data(), "CAMP", 4) != 0))
      return false;
    const uint32_t count = ReadBe32(data, 8);
    if (data.size() < 12 + 4ull * count)
      return false;
    std::vector<uint32_t> starts;
    for (uint32_t i = 0; i < count; ++i)
      starts.push_back(ReadBe32(data, 12 + 4 * i));
    for (uint32_t start : starts) {
      uint32_t end = static_cast<uint32_t>(data.size());
      for (uint32_t other : starts)
        if (other > start && other < end)
          end = other;
      entries.emplace_back(start, start && start < data.size() ? end : start);
    }
    return true;
  }
};

class Loader {
 public:
  Loader(PPCContext& ctx, uint8_t* base)
      : ctx_(ctx), base_(base), memory_(rex::Runtime::instance()->memory()) {}

  // A copy in the APPKEEP heap, or in physical memory once that is full.
  uint32_t Place(const uint8_t* data, uint32_t size) {
    uint32_t at = HeapAlloc(size);
    if (!at) {
      at = memory_->SystemHeapAlloc(size, kAlign, rex::memory::kSystemHeapPhysical);
      overflow_ += size;
    }
    if (at)
      std::memcpy(memory_->TranslateVirtual(at), data, size);
    return at;
  }

  uint32_t Place(const Bmd& bmd, uint32_t index) {
    if (index >= bmd.entries.size())
      return 0;
    const auto [begin, end] = bmd.entries[index];
    return begin < end ? Place(bmd.data.data() + begin, end - begin) : 0;
  }

  uint32_t overflow() const { return overflow_; }

 private:
  // On the hook's own context, saved around the call.
  uint32_t HeapAlloc(uint32_t size) {
    PPCContext& ctx = ctx_;
    uint8_t* base = base_;
    const PPCContext saved = ctx;
    ctx.r3.u64 = kHeap;
    ctx.r4.u64 = size;
    ctx.r5.u64 = kAlign;
    sub_821125A0(ctx, base);
    const uint32_t at = ctx.r3.u32;
    ctx = saved;
    return at;
  }

  PPCContext& ctx_;
  uint8_t* base_;
  rex::memory::Memory* memory_;
  uint32_t overflow_ = 0;
};

}  // namespace

namespace eternalsonata {

void BuildPs3AppKeep(PPCContext& ctx, uint8_t* base) {
  Loader loader(ctx, base);
  g_ps3_to_360.fill(0xFFFF);
  Bmd appkeep2;
  if (!appkeep2.Load("appkeep2.bmd") || appkeep2.entries.size() != kAppKeep2Entries) {
    REXLOG_ERROR("ps3 appkeep: appkeep2.bmd missing or not {} entries", kAppKeep2Entries);
    appkeep2.entries.clear();
  }

  std::array<uint32_t, kSlotsPs3> file{};
  Bmd camp;
  // The PS3 file's last entry is its slot 373; the 360's are null from 365.
  if (REX_LOAD_U32(kSlotArray + 4 * (kSlotsPs3 - 1)) == 0 ||
      REX_LOAD_U32(kSlotArray + 4 * kSlotsPs3) != 0) {
    REXLOG_ERROR("ps3 appkeep: AppKeep.bmd is not the PS3's; rerun ps3_convert.py");
  } else {
    for (uint32_t i = 0; i < kSlotsPs3; ++i)
      file[i] = REX_LOAD_U32(kSlotArray + 4 * i);
    for (uint32_t i = 2; i < appkeep2.entries.size(); ++i)
      g_appkeep2[i] = loader.Place(appkeep2, i);
    if (!camp.Load("campdata/camp_char.bmd"))
      REXLOG_ERROR("ps3 appkeep: campdata/camp_char.bmd missing");
    std::array<uint32_t, std::size(kCharacterFiles)> characters{};
    for (size_t i = 0; i < std::size(kCharacterFiles); ++i) {
      std::vector<uint8_t> data;
      if (ReadFile(kCharacterFiles[i], data) && data.size() >= 8 &&
          std::memcmp(data.data(), "NOBJ", 4) == 0)
        characters[i] = loader.Place(data.data(), static_cast<uint32_t>(data.size()));
      else
        REXLOG_ERROR("ps3 appkeep: {} missing or not a NOBJ", kCharacterFiles[i]);
    }

    std::array<uint32_t, kPs3PortraitSlot + std::size(kCampPortraits)> slots{};
    for (const Run& run : kRuns) {
      for (uint32_t k = 0; k < run.count; ++k) {
        const uint32_t i = run.first + k;
        uint32_t& slot = slots[run.slot + k];
        switch (run.source) {
          case Source::kAppKeep:
            slot = file[i];
            g_ps3_to_360[i] = static_cast<uint16_t>(run.slot + k);
            break;
          case Source::kAppKeep2: slot = g_appkeep2[i]; break;
          case Source::kCampChar: slot = loader.Place(camp, i); break;
          case Source::kCharacter: slot = characters[i]; break;
        }
      }
    }
    for (size_t k = 0; k < std::size(kPs3Only); ++k) {
      slots[kSlots360 + k] = file[kPs3Only[k]];
      g_ps3_to_360[kPs3Only[k]] = static_cast<uint16_t>(kSlots360 + k);
    }
    for (size_t k = 0; k < std::size(kCampPortraits); ++k)
      slots[kPs3PortraitSlot + k] = loader.Place(camp, kCampPortraits[k]);
    for (uint32_t i = 0; i < slots.size(); ++i)
      REX_STORE_U32(kSlotArray + 4 * i, slots[i]);
  }

  // CRS and SRN are only reached through native 1141, so they go last and
  // are the ones to spill if the heap is full.
  for (uint32_t i = 0; i < 2 && i < appkeep2.entries.size(); ++i)
    g_appkeep2[i] = loader.Place(appkeep2, i);
  // Only the status page shows these, so they spill after CRS and SRN.
  for (size_t k = 0; k < std::size(kCostumePortraits) && !camp.entries.empty(); ++k)
    REX_STORE_U32(kSlotArray + 4 * (kPs3CostumePortraitSlot + k),
                  loader.Place(camp, kCostumePortraits[k]));
  if (loader.overflow())
    REXLOG_INFO("ps3 appkeep: {} bytes past the APPKEEP heap", loader.overflow());
}

uint32_t Ps3AppKeepImageId(uint32_t ps3_id) {
  if (ps3_id == 0 || ps3_id > kSlotsPs3 || g_ps3_to_360[ps3_id - 1] == 0xFFFF)
    return 0;
  return g_ps3_to_360[ps3_id - 1] + 1u;
}

uint32_t Ps3AppKeep2Model(uint32_t index) {
  return index < kAppKeep2Entries ? g_appkeep2[index] : 0;
}

}  // namespace eternalsonata

REX_EXTERN(__imp__sub_820EA260);

// Native 1052 (dword_82420AF8 index, ...): an AppKeep effect, slot + 1.
REX_HOOK_RAW(sub_820EA260) {
  const uint32_t args = ctx.r3.u32;
  const uint32_t index = REX_LOAD_U32(args);
  if (!eternalsonata::IsPs3Target() || index == 0 || index > kSlotsPs3 ||
      g_ps3_to_360[index - 1] == 0xFFFF)
    return __imp__sub_820EA260(ctx, base);
  REX_STORE_U32(args, g_ps3_to_360[index - 1] + 1u);
  __imp__sub_820EA260(ctx, base);
  REX_STORE_U32(args, index);
}
