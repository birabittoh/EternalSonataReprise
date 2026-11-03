// eternalsonata - The PS3's three extra Music gallery tracks.
//
// The PS3 adds The Royal Mirror (66), 321 - Summer! (67) and 321 - Crescendo!
// (68); their MP166..MP168.cxs light flags 66..68 like any other track, and the
// PS3 text already titles them. Its tabs hold 22, 33 and 10 rows (EBOOT
// 0x46A908, stride 36), so tab 1 no longer fits the 360's 32 byte rows at
// byte_8238E128. In PS3 mode every reader of that table takes a copy with 64
// byte rows, its stride shifts double, and sub_821FEBC8 accepts ids up to 68.

#include "ps3_music_tabs.h"

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

constexpr uint32_t kRetailTable = 0x8238E128u;  // byte_8238E128, u8[3][32]
constexpr uint32_t kTabCounts = 0x822FF594u;    // byte_822FF594, u8[3]
constexpr uint32_t kRetailStride = 32;
constexpr uint32_t kPs3Stride = 64;
constexpr int kRetailIdMax = 66;
constexpr int kPs3IdMax = 68;

constexpr uint8_t kPs3Counts[3] = {22, 33, 10};
constexpr uint8_t kPs3Tabs[3][33] = {
    {1, 4, 54, 57, 63, 64, 65, 56, 61, 62, 53, 49, 50, 52, 34, 3, 7, 8, 59, 60, 9, 10},
    {19, 20, 21, 23, 24, 25, 26, 27, 28, 22, 30, 31, 29, 58, 32, 33, 55,
     40, 35, 36, 37, 38, 39, 41, 66, 42, 43, 44, 45, 46, 47, 48, 67},
    {11, 12, 14, 13, 15, 16, 68, 5, 17, 18},
};

// Room past the last tab: the Achievements screen walks rows past its count.
constexpr uint32_t kCopySize = 512;

bool g_ps3 = false;
std::once_flag g_copy_once;
uint32_t g_copy = 0;

void MakeCopy() {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory || !(g_copy = memory->SystemHeapAlloc(kCopySize, 16)))
    return;
  auto* host = memory->TranslateVirtual<uint8_t*>(g_copy);
  std::memset(host, 0, kCopySize);
  for (uint32_t tab = 0; tab < 3; ++tab)
    std::memcpy(host + kPs3Stride * tab, kPs3Tabs[tab], sizeof(kPs3Tabs[tab]));
}

uint32_t Copy() {
  std::call_once(g_copy_once, MakeCopy);
  return g_copy;
}

bool Ps3Tabs() { return g_ps3 && Copy(); }

}  // namespace

namespace eternalsonata {

void ApplyPs3MusicTabs() {
  if (!IsPs3Target())
    return;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(kTabCounts) : nullptr;
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(kTabCounts, sizeof(kPs3Counts),
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect)) {
    REXLOG_ERROR("ps3 music: could not write the tab counts");
    return;
  }
  std::memcpy(memory->TranslateVirtual(kTabCounts), kPs3Counts, sizeof(kPs3Counts));
  heap->Protect(kTabCounts, sizeof(kPs3Counts), old_protect, nullptr);
  g_ps3 = true;
}

uint32_t MusicTabTable() { return Ps3Tabs() ? Copy() : kRetailTable; }

uint32_t MusicTabStride() { return Ps3Tabs() ? kPs3Stride : kRetailStride; }

int MusicTrackIdMax() { return g_ps3 ? kPs3IdMax : kRetailIdMax; }

}  // namespace eternalsonata

// The lis/addi pairs that load byte_8238E128.
extern "C++" void EternalSonataMusicTabTable(PPCRegister& table);

void EternalSonataMusicTabTable(PPCRegister& table) {
  if (Ps3Tabs())
    table.u64 = Copy();
}

// The `slwi tab, 5` that steps between rows.
extern "C++" void EternalSonataMusicTabStride(PPCRegister& offset);

void EternalSonataMusicTabStride(PPCRegister& offset) {
  if (Ps3Tabs())
    offset.u64 = offset.u32 << 1;
}

// sub_821FEBC8's `track_id - 1 <= 0x41`; nothing reads r11 after the compare.
extern "C++" void EternalSonataMusicTrackRange(PPCRegister& index);

void EternalSonataMusicTrackRange(PPCRegister& index) {
  if (g_ps3 && index.u32 < kPs3IdMax)
    index.u64 = 0;
}
