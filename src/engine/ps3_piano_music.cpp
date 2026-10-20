// eternalsonata - The PS3's eight Piano Music pieces.
//
// The PS3 inserts Little Dog Waltz as the fifth piece, on flag 88 (so
// sound\cxs\MP188.wav lights it like any other) with six history pages. The
// 360 menu is seven wide in four places, each grown here in PS3 mode only:
//
//   * the flag id table (dword_82016134, whose next word is other data) is
//     read from a guest copy, and sub_8222B348's walk runs one entry longer;
//   * the page counts (byte_82029D30) take their eighth byte from padding;
//   * the screen list's row frames and cursor stops become eight at the
//     PS3's 68px pitch, which fits the same 543px box, and sub_8222A650
//     places eight rows (layer ids 0x194.. reach 0x1B0, which nothing else on
//     the screen uses);
//   * sub_8222B570's title art table is eight per language, read from a copy
//     of the PS3's, whose atlas rows also differ for the first seven.

#include "ps3_piano_music.h"

#include "generated/eternalsonata_init.h"
#include "target.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

#include "ps3_piano_music.inc"

constexpr uint32_t kFlagTable = 0x82016134u;  // dword_82016134
constexpr uint32_t kPageCounts = 0x82029D30u;  // byte_82029D30
constexpr uint32_t kLanguage = 0x8243D370u;   // dword_8243D370, 0 is Japanese
constexpr int kRetailCount = 7;
constexpr int kPs3Count = 8;

// The Piano Music screen list per language, as sub_821EC050 hands them over.
constexpr uint32_t kLists[] = {0x82031068u, 0x8205B680u, 0x82060578u, 0x82065448u,
                               0x8206A328u, 0x8206F200u, 0x820740D8u};
constexpr uint32_t kListEnd = 0xFFFFu;
constexpr uint32_t kMaxListWords = 0x400u;

// Row frames {1, x, y, w, h} and cursor stops {1502, x, y, n}.
constexpr uint32_t kFrameWidth = 440;
constexpr uint32_t kCursorX = 480;
constexpr uint32_t kCursorY = 167;
constexpr uint32_t kRetailPitch = 75;
constexpr uint32_t kPs3Pitch = 68;

bool g_eight = false;
std::once_flag g_copies_once;
uint32_t g_flags = 0;
uint32_t g_art = 0;
std::mutex g_lists_mutex;
std::array<uint32_t, std::size(kLists)> g_list_copies{};

rex::memory::Memory* Memory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

void MakeCopies() {
  auto* memory = Memory();
  if (!memory)
    return;
  if ((g_flags = memory->SystemHeapAlloc(sizeof(kPs3PianoFlags), 16)))
    for (size_t i = 0; i < std::size(kPs3PianoFlags); ++i)
      rex::memory::store_and_swap<uint32_t>(
          memory->TranslateVirtual<uint8_t*>(g_flags + 4 * static_cast<uint32_t>(i)),
          kPs3PianoFlags[i]);
  const auto* art = &kPs3PianoArt[0][0][0];
  constexpr size_t kArtWords = sizeof(kPs3PianoArt) / sizeof(uint32_t);
  if ((g_art = memory->SystemHeapAlloc(sizeof(kPs3PianoArt), 16)))
    for (size_t i = 0; i < kArtWords; ++i)
      rex::memory::store_and_swap<uint32_t>(
          memory->TranslateVirtual<uint8_t*>(g_art + 4 * static_cast<uint32_t>(i)), art[i]);
}

uint32_t Flags() {
  std::call_once(g_copies_once, MakeCopies);
  return g_flags;
}

uint32_t Art() {
  std::call_once(g_copies_once, MakeCopies);
  return g_art;
}

// Replaces `count` records of `width` words that `match` accepts, starting at
// the first record it accepts, with `with`. False if the run is not there.
template <typename Match>
bool ReplaceRun(std::vector<uint32_t>& words, size_t width, int count, Match match,
                const std::vector<uint32_t>& with) {
  for (size_t at = 0; at + width * count <= words.size(); ++at) {
    bool found = true;
    for (int k = 0; k < count && found; ++k)
      found = match(&words[at + width * k], k);
    if (found) {
      words.erase(words.begin() + at, words.begin() + at + width * count);
      words.insert(words.begin() + at, with.begin(), with.end());
      return true;
    }
  }
  return false;
}

uint32_t BuildList(uint32_t list) {
  auto* memory = Memory();
  if (!memory)
    return 0;
  std::vector<uint32_t> words;
  for (uint32_t i = 0; i < kMaxListWords; ++i) {
    const uint32_t word =
        rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(list + 4 * i));
    if (word == kListEnd)
      break;
    words.push_back(word);
  }
  std::vector<uint32_t> frames, stops;
  for (uint32_t k = 0; k < kPs3Count; ++k) {
    frames.insert(frames.end(), {1, 0, kPs3Pitch * k, kFrameWidth, kPs3Pitch});
    stops.insert(stops.end(), {1502, kCursorX, kCursorY + kPs3Pitch * k, k + 1});
  }
  const bool ok =
      words.size() < kMaxListWords &&
      ReplaceRun(words, 5, kRetailCount, [](const uint32_t* r, int k) {
        return r[0] == 1 && r[1] == 0 && r[2] == kRetailPitch * k && r[3] == kFrameWidth &&
               r[4] == kRetailPitch;
      }, frames) &&
      ReplaceRun(words, 4, kRetailCount, [](const uint32_t* r, int k) {
        return r[0] == 1502 && r[1] == kCursorX && r[2] == kCursorY + kRetailPitch * k &&
               r[3] == static_cast<uint32_t>(k + 1);
      }, stops);
  if (!ok) {
    REXLOG_WARN("ps3 piano: list {:08X} does not hold the seven rows", list);
    return 0;
  }
  const uint32_t copy = memory->SystemHeapAlloc(4 * static_cast<uint32_t>(words.size() + 1), 16);
  if (!copy)
    return 0;
  words.push_back(kListEnd);
  for (size_t i = 0; i < words.size(); ++i)
    rex::memory::store_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(copy + 4 * static_cast<uint32_t>(i)), words[i]);
  return copy;
}

}  // namespace

namespace eternalsonata {

void ApplyPs3PianoMusic() {
  if (!IsPs3Target())
    return;
  auto* memory = Memory();
  auto* heap = memory ? memory->LookupHeap(kPageCounts) : nullptr;
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(kPageCounts, sizeof(kPs3PianoPages),
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect)) {
    REXLOG_ERROR("ps3 piano: could not write the page counts");
    return;
  }
  std::memcpy(memory->TranslateVirtual(kPageCounts), kPs3PianoPages, sizeof(kPs3PianoPages));
  heap->Protect(kPageCounts, sizeof(kPs3PianoPages), old_protect, nullptr);
  g_eight = true;
}

int PianoMusicCount() {
  return g_eight && Flags() ? kPs3Count : kRetailCount;
}

uint32_t PianoMusicFlagTable() {
  return PianoMusicCount() == kPs3Count ? Flags() : kFlagTable;
}

uint32_t Ps3PianoMusicList(uint32_t list) {
  if (!g_eight)
    return 0;
  const auto* it = std::find(std::begin(kLists), std::end(kLists), list);
  if (it == std::end(kLists))
    return 0;
  std::lock_guard<std::mutex> lock(g_lists_mutex);
  uint32_t& copy = g_list_copies[static_cast<size_t>(it - std::begin(kLists))];
  if (!copy)
    copy = BuildList(list);
  return copy;
}

uint32_t PianoMusicKeyMask(uint32_t retail_mask) {
  if (PianoMusicCount() != kPs3Count)
    return retail_mask;
  // Rows from Little Dog Waltz on move down one. The key only ever answers
  // with all seven, and the PS3 piece has no file in it, so it rides along.
  constexpr uint32_t kAll = (1u << kRetailCount) - 1;
  constexpr uint32_t kBefore = (1u << 4) - 1;
  uint32_t mask = (retail_mask & kBefore) | ((retail_mask & ~kBefore & kAll) << 1);
  if ((retail_mask & kAll) == kAll)
    mask |= 1u << 4;
  return mask;
}

}  // namespace eternalsonata

using eternalsonata::PianoMusicCount;

// The lis/addi pairs that load dword_82016134.
extern "C++" void EternalSonataPianoFlagTable(PPCRegister& table);

void EternalSonataPianoFlagTable(PPCRegister& table) {
  if (PianoMusicCount() == 8)
    table.u64 = eternalsonata::PianoMusicFlagTable();
}

// sub_8222B348's end of walk, the table plus seven entries.
extern "C++" void EternalSonataPianoFlagTableEnd(PPCRegister& end);

void EternalSonataPianoFlagTableEnd(PPCRegister& end) {
  if (PianoMusicCount() == 8)
    end.u64 = end.u32 + 4;
}

// sub_8222A650 measures the widest title over pieces 0..6.
extern "C++" bool EternalSonataPianoTitleWidthLoop(PPCRegister& piece);

bool EternalSonataPianoTitleWidthLoop(PPCRegister& piece) {
  return PianoMusicCount() == 8 && piece.u32 == 7;
}

// sub_8222A650 places a row per layer id up to 0x1B0, 75px apart.
extern "C++" bool EternalSonataPianoRowLoop(PPCRegister& y, PPCRegister& id);

bool EternalSonataPianoRowLoop(PPCRegister& y, PPCRegister& id) {
  if (PianoMusicCount() != 8)
    return false;
  y.u64 = y.u32 - (kRetailPitch - kPs3Pitch);
  return id.u32 == 0x1B0;
}

// sub_8222B570's title art, unk_82029D38 indexed 7 * language + piece.
extern "C++" void EternalSonataPianoArtTable(PPCRegister& table);

void EternalSonataPianoArtTable(PPCRegister& table) {
  if (PianoMusicCount() == 8 && Art())
    table.u64 = Art();
}

extern "C++" void EternalSonataPianoArtStride(PPCRegister& language);

void EternalSonataPianoArtStride(PPCRegister& language) {
  if (PianoMusicCount() == 8 && Art())
    language.u64 = language.u32 / kRetailCount * kPs3Count;
}

// The number strip's row, the piece index on the 360.
extern "C++" void EternalSonataPianoStripRow(PPCRegister& row);

void EternalSonataPianoStripRow(PPCRegister& row) {
  auto* memory = Memory();
  if (PianoMusicCount() != 8 || row.u32 >= kPs3Count || !memory)
    return;
  const bool japanese =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kLanguage)) == 0;
  row.u64 = kPs3PianoStripRows[japanese ? 0 : 1][row.u32];
}
