// eternalsonata - status/pause menu option strip.
//
// The option row (Items, Item Set, Status, Party Level, Score Pieces, Photos,
// Piano Music, Music, Options, Load, Save) is drawn from a strip of eleven
// type-100 icon records in the screen's display list, all at y=11,
// x = 295 + 75 * index. The options are sprites, not text: no BTX lookup ever
// resolves an option name, which is why hooking the text lookup found only the
// hint bar, the title and the bottom info bar.
//
// Drawing and selection are two separate runs of records. A second run, at its
// own coordinates (measured: y=174, x = 375 + 75 * index), is what makes the
// options selectable: the interpreter's call site at 0x821F4BB0 passes
// sub_821FFEF8(ui, group, x = rec[+4], y = rec[+8], tag = rec[+0xC]). The tag
// is the option's identity and it is stored in the record, not derived from the
// position, so the eleven tags are the non-sequential
//
//   1 Items, 12 Item Set, 2 Status, 5 Party Level, 3 Score Pieces, 4 Photos,
//   9 Piano Music, 10 Music, 8 Options, 7 Load, 6 Save
//
// Editing only the icon run is what left the hover labels one place to the
// right of the art, with an eleventh selectable still reachable past Save.
//
// A third run, of type-110 records at y=-21, is the glow the cursor lights up
// behind the hovered icon. Each one is cut to its own option's icon shape, so
// leaving it in place put the wrong silhouette behind the moved entries.
//
// What consumes the tag: sub_82236CD0 switches on it through byte_82082148
// (index = tag - 1, offsets into its own jump table) and stores a screen id in
// dword_8243F364. State 6 of the strip's tick sub_821DC3C0 copies that id to
// the byte at dword_824400E4 + 0x661, and sub_821F62B8 calls
// off_8238E270[id](arg0, arg1) every frame, so the id is a direct index into
// that tick table: 0 the strip itself, 9 Piano Music, 0xA Music, 0xC Item Set.
// Tag 11 is the only value in 1..12 whose byte_82082148 entry points at the
// switch's default arm, so it is free and selecting it is a no-op.
//
// Ruled out, so it is not re-derived:
//   * dword_8243FC08[10] is the party roster, not the menu. word_8202CA3C =
//     {220..229} are per-character portrait icons. sub_821DD808 appends the
//     party portrait row; sub_820E78B8 / sub_820E7948 add and remove party
//     members from the script VM.
//   * BTX id 28 "Switch Character" is a button hint and id 29 "Party Level" is
//     the bottom info bar. Neither is an option, so option labels cannot be
//     found by their text.
//   * The selectable groups at ui + 0x188 (ui = *dword_824400E8) cannot be
//     patched after the fact: the list is still empty when the interpreter
//     returns, because the groups are torn down and rebuilt around it.
//
// sub_821EC050 copies the static list into a stack buffer before the
// interpreter sees it (word by word, up to the 0xFFFF terminator), so offsets
// from the list start are preserved and the copy is freely writable. That is
// what this edits: no read-only guest page is involved, unlike the Options
// screen rows in eternalsonata_options.cpp.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

#include "eternalsonata_asset_container.h"
#include "option_strip.h"
#include "ps3_appkeep.h"
#include "scrapbook_menu.h"

// sub_82179160(manager, handle, argb, 0, 0, -1): a screen object's colour.
REX_EXTERN(__imp__sub_82179160);

namespace option_strip {
namespace {

// The strip, as laid out in the shipped list.
constexpr std::uint32_t kIconStride = 0x1Cu;  // a type-100 record
constexpr std::uint32_t kIconRecordType = 100u;
constexpr int kStripCount = 11;
constexpr std::int32_t kIconSpacing = 75;  // shared by both runs
constexpr std::int32_t kIconY = 11;
constexpr std::uint32_t kFirstIconId = 185u;

// The hover glow is a third run: type-110 records, {type, id, x, y, 1000, 1000,
// argb, 2}, 0x20 each, at y=-21 and x = 263 + 75 * index, ids 291 + index. It is
// one glow per option, cut to that option's icon shape, so it has to travel with
// the art the same way the selectable does.
constexpr std::uint32_t kGlowRecordType = 110u;
constexpr std::uint32_t kGlowStride = 0x20u;
constexpr std::int32_t kGlowY = -21;
constexpr std::uint32_t kListTerminator = 0xFFFFu;
constexpr std::uint32_t kMaxListWords = 0x800u;

// Index 2 is Status, the entry that has no screen of its own and only moves the
// cursor down into the party row. It is repurposed as Achievements and moved to
// index 7, just after Music.
constexpr int kSourceIndex = 2;
constexpr int kTargetIndex = 7;

// Old index drawn at each new position.
constexpr int kOrder[kStripCount] = {0, 1, 3, 4, 5, 6, 7, 2, 8, 9, 10};

// The fallback when the list cannot be widened: Item Set's records carry the
// Scrapbook instead, just after Photos as on the PS3, and Item Set leaves.
constexpr int kScrapbookSource = 1;
constexpr int kScrapbookIndex = 4;
constexpr int kOrderScrapbook[kStripCount] = {0, 3, 4, 5, 1, 6, 7, 2, 8, 9, 10};
static_assert(kOrderScrapbook[kScrapbookIndex] == kScrapbookSource &&
              kOrderScrapbook[kTargetIndex] == kSourceIndex);

// The PS3's twelve entry strip (sub_273788): the Scrapbook is a twelfth entry
// after Photos, at the PS3's selectable x. The eleven stock records keep kOrder
// and only move; the Scrapbook's icon and glow are appended to the end of the
// list, so every stock screen object keeps the index the game reads it by
// (glows from +144, Items' icon at +92, Save's at +132).
constexpr int kWideCount = 12;
constexpr int kWideScrapbookIndex = 5;
constexpr std::int32_t kWideSelectableX[kWideCount] = {
    375, 445, 515, 585, 655, 725, 795, 865, 935, 1005, 1075, 1149};
constexpr std::uint32_t kSelectableStride = 16u;  // {1502, x, y, tag}

// The list opcodes that wrap the appended records like the stock runs: 2100 n
// pushes the strip's parent node n and 2101 pops it; 3000 n sets the draw
// priority, 1 for the icons and 0, where the list leaves it, for the glows.
constexpr std::uint32_t kPushParent = 2100u;
constexpr std::uint32_t kPopParent = 2101u;
constexpr std::uint32_t kSetPriority = 3000u;

constexpr std::uint32_t kObjectManagerAddress = 0x824CF500u;

// Its art: the PS3 AppKeep's camp icon and hover glow (PS3 ids 367 and 368).
constexpr std::uint32_t kScrapbookIconPs3 = 367u;
constexpr std::uint32_t kScrapbookGlowPs3 = 368u;

// Item Set's BTX id in sub_8223B2F8's switch, answered for the Scrapbook.
constexpr std::uint32_t kItemSetLabelSid = 33u;

// The selectable record's tag is the option's identity: sub_82236CD0 switches
// on it through byte_82082148 (index = tag - 1) and stores a screen id in
// dword_8243F364. Tag 11 is the one value in 1..12 whose table entry points at
// the switch's default arm, so it is free and opening it is a no-op until a
// screen exists.
constexpr std::uint32_t kAchievementsTag = 11u;

// The BTX id sub_8223B2F8's switch picks for slot 2 (Status). The hover label
// is resolved through sub_8223B780, so answering that id while the strip asks
// for the moved slot is what renames it.
constexpr std::uint32_t kStatusLabelSid = 23u;

constexpr std::uint32_t kLanguageIndexAddress = 0x8243D370u;
constexpr std::uint32_t kJapaneseLanguage = 0u;
constexpr std::uint32_t kSpanishLanguage = 6u;
constexpr std::uint32_t kMenuRootAddress = 0x824400E8u;

// The tags the shipped strip carries, in drawn order. sub_8223B3E8 hardcodes
// this same sequence, which is what makes it safe to recognise its work.
constexpr std::uint32_t kCanonicalTags[kStripCount] = {1, 12, 2, 5, 3, 4,
                                                       9, 10, 8, 7, 6};

// Indexed by dword_8243D370: 0 JPN, 1 USA, 2 GBR, 3 FRA, 4 ITA, 5 DEU, 6 ESP.
// Latin-1 bytes, not UTF-8: the text records are single byte, so a multi-byte
// source character would draw as two garbled glyphs.
// English says Trophies rather than Achievements: it fits the strip's label
// slot and the screen heading without crowding them.
constexpr const char* kAchievementsLabel[7] = {
    "Trophies",  "Trophies", "Trophies", "Succ\xE8s",
    "Obiettivi", "Erfolge",  "Logros",
};

std::uint32_t ReadU32(const std::uint8_t* base, std::uint32_t address) {
  return rex::memory::load_and_swap<std::uint32_t>(base + address);
}

void WriteU32(std::uint8_t* base, std::uint32_t address, std::uint32_t value) {
  rex::memory::store_and_swap<std::uint32_t>(base + address, value);
}

std::int32_t ReadI32(const std::uint8_t* base, std::uint32_t address) {
  return static_cast<std::int32_t>(ReadU32(base, address));
}

// Offset of the list's 0xFFFF terminator word, or 0.
std::uint32_t FindListEnd(const std::uint8_t* base, std::uint32_t list) {
  for (std::uint32_t i = 0; i < kMaxListWords; ++i) {
    const std::uint32_t at = list + 4u * i;
    if (ReadU32(base, at) == kListTerminator) {
      return at;
    }
  }
  return 0;
}

// Where the icon strip starts, or 0. Scanned rather than taken as a fixed
// offset: each language has its own copy of the list (index 0 JPN, 1 USA,
// 2 GBR, 3 FRA, 4 ITA, 5 DEU, 6 ESP) and they do not share a layout, so
// 0x82057018's 0x24C does not carry over to the USA list's 0x228.
//
// The signature is two consecutive icon records, 185 then 233, both at y=11.
// One record alone is too weak: icon ids recur elsewhere in the list.
std::uint32_t FindIconStrip(const std::uint8_t* base, std::uint32_t list,
                            std::uint32_t end) {
  for (std::uint32_t at = list; at + kIconStride * kStripCount <= end;
       at += 4u) {
    if (ReadU32(base, at) != kIconRecordType ||
        ReadU32(base, at + 4u) != kFirstIconId ||
        ReadI32(base, at + 12u) != kIconY) {
      continue;
    }
    const std::uint32_t next = at + kIconStride;
    if (ReadU32(base, next) == kIconRecordType &&
        ReadU32(base, next + 4u) == 233u) {
      return at;
    }
  }
  return 0;
}

// Where the glow run starts, or 0. Matched on its own shape rather than on an
// id, for the same reason the icon strip is scanned: every language has its own
// copy of the list and they do not share a layout.
std::uint32_t FindGlowRun(const std::uint8_t* base, std::uint32_t list,
                          std::uint32_t end) {
  for (std::uint32_t at = list; at + kGlowStride * kStripCount <= end;
       at += 4u) {
    if (ReadU32(base, at) != kGlowRecordType ||
        ReadI32(base, at + 12u) != kGlowY) {
      continue;
    }
    const std::int32_t x0 = ReadI32(base, at + 8u);
    bool ok = true;
    for (int k = 1; ok && k < kStripCount; ++k) {
      const std::uint32_t rec = at + kGlowStride * static_cast<std::uint32_t>(k);
      ok = ReadU32(base, rec) == kGlowRecordType &&
           ReadI32(base, rec + 12u) == kGlowY &&
           ReadI32(base, rec + 8u) == x0 + kIconSpacing * k;
    }
    if (ok) {
      return at;
    }
  }
  return 0;
}

struct Run {
  std::uint32_t start = 0;
  std::uint32_t stride = 0;
};

// The selectable run: eleven records of one type, at a uniform stride, sharing
// a y and stepping x by 75. The record size is not assumed, it is whatever
// spacing satisfies the run, because this record type's layout beyond
// {type, x, y, tag} was never needed and so was never derived.
bool FindSelectableRun(const std::uint8_t* base, std::uint32_t list,
                       std::uint32_t end, Run* out) {
  for (std::uint32_t at = list; at + 16u <= end; at += 4u) {
    const std::uint32_t type = ReadU32(base, at);
    const std::int32_t x0 = ReadI32(base, at + 4u);
    const std::int32_t y0 = ReadI32(base, at + 8u);
    if (type == kIconRecordType || type == kListTerminator) {
      continue;
    }
    for (std::uint32_t stride = 4u; stride <= 0x80u; stride += 4u) {
      const std::uint32_t next = at + stride;
      if (next + 16u > end) {
        break;
      }
      if (ReadU32(base, next) != type || ReadI32(base, next + 8u) != y0 ||
          ReadI32(base, next + 4u) != x0 + kIconSpacing) {
        continue;
      }
      bool ok = true;
      for (int k = 2; ok && k < kStripCount; ++k) {
        const std::uint32_t rec = at + stride * static_cast<std::uint32_t>(k);
        ok = rec + 16u <= end && ReadU32(base, rec) == type &&
             ReadI32(base, rec + 8u) == y0 &&
             ReadI32(base, rec + 4u) == x0 + kIconSpacing * k;
      }
      if (ok) {
        out->start = at;
        out->stride = stride;
        return true;
      }
      break;  // the tightest spacing did not hold, so this is not the run
    }
  }
  return false;
}

void AdjustJapanesePartyLevel(std::uint8_t* base, std::uint32_t list,
                              std::uint32_t end) {
  if (ReadU32(base, kLanguageIndexAddress) != kJapaneseLanguage) {
    return;
  }
  for (std::uint32_t at = list; at + 16u <= end; at += 4u) {
    if (ReadU32(base, at) == 300u && ReadI32(base, at + 8u) == 375 &&
        ReadI32(base, at + 12u) == 620) {
      WriteU32(base, at + 8u, 415u);
      return;
    }
  }
}

// Reorder one run in place: record at new position i is old position order[i],
// with x restated so the run stays evenly spaced from its own first record.
// Both runs are permuted the same way, which is what keeps the art, the cursor
// and the label in step. Safe because the list is the interpreter's writable
// stack copy.
void PermuteRun(std::uint8_t* base, std::uint32_t start, std::uint32_t stride,
                std::uint32_t x_offset, const int* order) {
  constexpr std::uint32_t kMaxStride = 0x80u;
  std::uint32_t saved[kStripCount][kMaxStride / 4u];
  for (int i = 0; i < kStripCount; ++i) {
    for (std::uint32_t off = 0; off < stride; off += 4u) {
      saved[i][off / 4u] =
          ReadU32(base, start + stride * static_cast<std::uint32_t>(i) + off);
    }
  }
  const std::int32_t first_x = static_cast<std::int32_t>(saved[0][x_offset / 4u]);
  for (int i = 0; i < kStripCount; ++i) {
    const std::uint32_t dst = start + stride * static_cast<std::uint32_t>(i);
    for (std::uint32_t off = 0; off < stride; off += 4u) {
      WriteU32(base, dst + off, saved[order[i]][off / 4u]);
    }
    WriteU32(base, dst + x_offset,
             static_cast<std::uint32_t>(first_x + kIconSpacing * i));
  }
}

std::mutex g_edited_mutex;
bool g_edited = false;
bool g_scrapbook = false;  // the edit put the Scrapbook in
bool g_wide = false;       // as a twelfth entry, rather than over Item Set

std::uint32_t g_list_copy = 0;  // guest heap copy handed to the interpreter

// The appended glow's object handle. Its slot in the screen's object array is
// not fixed: sub_821DD808 appends the party row to the list first, so the
// glow lands after a party sized run. It is caught as sub_821EF0E8 makes it.
std::atomic<std::uint32_t> g_extra_glow_handle{0xFFFFFFFFu};
std::atomic<bool> g_awaiting_extra_glow{false};

// Set only for the duration of the stock sub_8223B2F8 call made for the moved
// slot, so the one BTX lookup that call performs is ours. Thread local because
// the flag is only ever meaningful inside that single guest call.
thread_local bool t_want_achievements_label = false;
thread_local bool t_want_scrapbook_label = false;

std::uint32_t g_label_string = 0;  // guest addresses, allocated on first use
std::uint32_t g_scrapbook_label_string = 0;

bool StripEdited() {
  std::lock_guard<std::mutex> lock(g_edited_mutex);
  return g_edited;
}

bool StripHasScrapbook() {
  std::lock_guard<std::mutex> lock(g_edited_mutex);
  return g_scrapbook;
}

bool StripIsWide() {
  std::lock_guard<std::mutex> lock(g_edited_mutex);
  return g_wide;
}

int StripCount() { return StripIsWide() ? kWideCount : kStripCount; }

// The stock record drawn at a wide strip position, or -1 for the Scrapbook.
int WideRecordAt(int index) {
  if (index == kWideScrapbookIndex) {
    return -1;
  }
  return index < kWideScrapbookIndex ? index : index - 1;
}

// The stock slot whose label and glow the entry at `index` carries, or -1 for
// the wide strip's Scrapbook, which has neither.
int StockSlotAt(int index) {
  if (StripIsWide()) {
    const int record = WideRecordAt(index);
    return record < 0 ? -1 : kOrder[record];
  }
  return (StripHasScrapbook() ? kOrderScrapbook : kOrder)[index];
}

bool ScrapbookAt(int index) {
  if (StripIsWide()) {
    return index == kWideScrapbookIndex;
  }
  return StripHasScrapbook() && index == kScrapbookIndex;
}

std::uint32_t TagAt(int index) {
  if (index == kTargetIndex) {
    return kAchievementsTag;
  }
  if (StripHasScrapbook() && index == kScrapbookIndex) {
    return scrapbook_menu::kTag;
  }
  return kCanonicalTags[StockSlotAt(index)];
}

std::uint32_t WriteLabel(std::uint8_t* base, std::uint32_t* address,
                         const std::string& label) {
  if (!*address) {
    auto* mem = rex::system::kernel_memory();
    *address = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
    if (!*address) {
      return 0;
    }
  }
  // Rewritten on every lookup: the language can change without a restart.
  const std::size_t length = std::min<std::size_t>(label.size(), 63);
  for (std::size_t i = 0; i < length; ++i) {
    REX_STORE_U8(*address + static_cast<std::uint32_t>(i),
                 static_cast<std::uint8_t>(label[i]));
  }
  REX_STORE_U8(*address + static_cast<std::uint32_t>(length), 0);
  return *address;
}

const char* AchievementsLabel(const std::uint8_t* base) {
  const std::uint32_t language = ReadU32(base, kLanguageIndexAddress);
  if (language == 0u) {
    // The Xbox's own Japanese word for achievements.
    static const std::string japanese = [] {
      std::string out;
      eternalsonata::assets::EncodeShiftJis("実績", out);
      return out;
    }();
    return japanese.c_str();
  }
  return kAchievementsLabel[language < 7u ? language : 1u];
}

}  // namespace

const char* AchievementsWord(const std::uint8_t* base) {
  return AchievementsLabel(base);
}

// Restates the eleven stock records at the wide strip's x, then copies the list
// to the guest heap with the Scrapbook's selectable inserted after Photos and
// its icon and glow appended. Returns the copy, or 0 to fall back.
std::uint32_t WidenStrip(std::uint8_t* base, std::uint32_t list,
                         std::uint32_t end, std::uint32_t strip,
                         std::uint32_t glow, const Run& sel) {
  // The glow run sits directly under the parent push the appended records
  // reuse; the icon run is under the same parent in every language's list.
  if (sel.stride != kSelectableStride || glow < list + 8u ||
      ReadU32(base, glow - 8u) != kPushParent) {
    return 0;
  }
  const std::uint32_t parent = ReadU32(base, glow - 4u);
  const std::int32_t icon_dx = ReadI32(base, strip + 8u) - ReadI32(base, sel.start + 4u);
  const std::int32_t glow_dx = ReadI32(base, glow + 8u) - ReadI32(base, sel.start + 4u);

  PermuteRun(base, strip, kIconStride, 8u, kOrder);
  PermuteRun(base, glow, kGlowStride, 8u, kOrder);
  PermuteRun(base, sel.start, sel.stride, 4u, kOrder);
  WriteU32(base, sel.start + sel.stride * static_cast<std::uint32_t>(kTargetIndex) + 12u,
           kAchievementsTag);
  for (int index = 0; index < kWideCount; ++index) {
    const int record = WideRecordAt(index);
    if (record < 0) {
      continue;
    }
    const auto at = static_cast<std::uint32_t>(record);
    const std::int32_t x = kWideSelectableX[index];
    WriteU32(base, sel.start + sel.stride * at + 4u, static_cast<std::uint32_t>(x));
    WriteU32(base, strip + kIconStride * at + 8u, static_cast<std::uint32_t>(x + icon_dx));
    WriteU32(base, glow + kGlowStride * at + 8u, static_cast<std::uint32_t>(x + glow_dx));
  }

  std::vector<std::uint32_t> words;
  for (std::uint32_t at = list; at < end; at += 4u) {
    words.push_back(ReadU32(base, at));
  }
  const std::int32_t x = kWideSelectableX[kWideScrapbookIndex];
  const std::uint32_t sel_word =
      (sel.start + sel.stride * static_cast<std::uint32_t>(kWideScrapbookIndex) - list) / 4u;
  const std::uint32_t selectable[] = {ReadU32(base, sel.start), static_cast<std::uint32_t>(x),
                                      ReadU32(base, sel.start + 8u), scrapbook_menu::kTag};
  words.insert(words.begin() + sel_word, std::begin(selectable), std::end(selectable));

  // Copies of the first stock icon and glow, with the Scrapbook's art and x.
  words.insert(words.end(), {kPushParent, parent, kSetPriority, 1u});
  for (std::uint32_t off = 0; off < kIconStride; off += 4u) {
    words.push_back(ReadU32(base, strip + off));
  }
  const std::size_t icon_word = words.size() - kIconStride / 4u;
  words[icon_word + 1u] = eternalsonata::Ps3AppKeepImageId(kScrapbookIconPs3);
  words[icon_word + 2u] = static_cast<std::uint32_t>(x + icon_dx);
  words.insert(words.end(), {kSetPriority, 0u, kPopParent, kPushParent, parent});
  for (std::uint32_t off = 0; off < kGlowStride; off += 4u) {
    words.push_back(ReadU32(base, glow + off));
  }
  const std::size_t glow_word = words.size() - kGlowStride / 4u;
  words[glow_word + 1u] = eternalsonata::Ps3AppKeepImageId(kScrapbookGlowPs3);
  words[glow_word + 2u] = static_cast<std::uint32_t>(x + glow_dx);
  words.push_back(kPopParent);

  constexpr std::uint32_t kCopyWords = kMaxListWords + 64u;
  if (words.size() + 1u > kCopyWords) {
    return 0;
  }
  if (!g_list_copy) {
    auto* mem = rex::system::kernel_memory();
    g_list_copy = mem ? mem->SystemHeapAlloc(4u * kCopyWords, 0x20) : 0;
    if (!g_list_copy) {
      return 0;
    }
  }
  std::uint32_t out = g_list_copy;
  for (const std::uint32_t word : words) {
    WriteU32(base, out, word);
    out += 4u;
  }
  WriteU32(base, out, kListTerminator);
  return g_list_copy;
}

std::uint32_t MaybeEditOptionStrip(std::uint8_t* base, std::uint32_t list) {
  if (!list) {
    return 0;
  }
  const std::uint32_t end = FindListEnd(base, list);
  if (!end) {
    return 0;
  }
  const std::uint32_t strip = FindIconStrip(base, list, end);
  if (!strip) {
    return 0;
  }
  Run sel;
  if (!FindSelectableRun(base, list, end, &sel)) {
    return 0;  // never edit the art without the selection, or they drift apart
  }
  const std::uint32_t glow = FindGlowRun(base, list, end);
  if (!glow) {
    return 0;  // same for the glow: a stale one sits behind the wrong icon
  }

  if (sel.stride > 0x80u) {
    return 0;  // beyond what PermuteRun can hold; refuse rather than corrupt
  }

  AdjustJapanesePartyLevel(base, list, end);

  const bool scrapbook = scrapbook_menu::StripShowsScrapbook() &&
                         eternalsonata::Ps3AppKeepImageId(kScrapbookIconPs3) &&
                         eternalsonata::Ps3AppKeepImageId(kScrapbookGlowPs3);
  if (scrapbook) {
    if (const std::uint32_t copy = WidenStrip(base, list, end, strip, glow, sel)) {
      std::lock_guard<std::mutex> lock(g_edited_mutex);
      g_edited = true;
      g_scrapbook = true;
      g_wide = true;
      g_extra_glow_handle = 0xFFFFFFFFu;
      g_awaiting_extra_glow = true;
      return copy;
    }
  }

  // Both runs keep eleven records, so nothing has to be inserted or deleted and
  // the cursor still stops at Save. The selectable records carry their own tags
  // and travel with their art, so every option that moves keeps its screen.
  const int* order = scrapbook ? kOrderScrapbook : kOrder;
  PermuteRun(base, strip, kIconStride, 8u, order);
  PermuteRun(base, glow, kGlowStride, 8u, order);
  PermuteRun(base, sel.start, sel.stride, 4u, order);
  WriteU32(base,
           sel.start + sel.stride * static_cast<std::uint32_t>(kTargetIndex) +
               12u,
           kAchievementsTag);
  if (scrapbook) {
    const auto at = static_cast<std::uint32_t>(kScrapbookIndex);
    WriteU32(base, strip + kIconStride * at + 4u,
             eternalsonata::Ps3AppKeepImageId(kScrapbookIconPs3));
    WriteU32(base, glow + kGlowStride * at + 4u,
             eternalsonata::Ps3AppKeepImageId(kScrapbookGlowPs3));
    WriteU32(base, sel.start + sel.stride * at + 12u, scrapbook_menu::kTag);
  }

  {
    std::lock_guard<std::mutex> lock(g_edited_mutex);
    g_edited = true;
    g_scrapbook = scrapbook;
    g_wide = false;
    g_awaiting_extra_glow = false;
  }
  return 0;
}

void MaybeCatchExtraGlow(std::uint32_t image_id, std::uint32_t handle) {
  if (g_awaiting_extra_glow &&
      image_id == eternalsonata::Ps3AppKeepImageId(kScrapbookGlowPs3)) {
    g_extra_glow_handle = handle;
    g_awaiting_extra_glow = false;
  }
}

// The wide strip's Scrapbook glow, lit or dimmed the way sub_8223B288 does the
// stock ones.
void SetExtraGlow(PPCContext& ctx, std::uint8_t* base, bool lit) {
  const std::uint32_t handle = g_extra_glow_handle;
  if (handle == 0xFFFFFFFFu) {
    return;
  }
  const std::uint32_t saved[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                 ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  ctx.r3.u32 = kObjectManagerAddress;
  ctx.r4.u32 = handle;
  ctx.r5.u32 = lit ? 0xFFFFFFFFu : 0u;
  ctx.r6.u32 = 0u;
  ctx.r7.u32 = 0u;
  ctx.r8.u32 = 0xFFFFFFFFu;
  __imp__sub_82179160(ctx, base);
  ctx.r3.u32 = saved[0];
  ctx.r4.u32 = saved[1];
  ctx.r5.u32 = saved[2];
  ctx.r6.u32 = saved[3];
  ctx.r7.u32 = saved[4];
  ctx.r8.u32 = saved[5];
}

// Spanish throws the selectable run away and rebuilds it by hand, so the
// permutation applied to the display list has to be applied again here.
void RepermuteSpanishSelectables(std::uint8_t* base) {
  if (!StripEdited() ||
      ReadU32(base, kLanguageIndexAddress) != kSpanishLanguage) {
    return;
  }
  // The menu root and everything it points at live in the 0xE physical
  // aperture, which base + address does not reach: it reads back as zeros
  // rather than faulting. REX_LOAD_U32 translates, so it is used from here on.
  const std::uint32_t menu = REX_LOAD_U32(kMenuRootAddress);
  if (!menu) {
    return;
  }
  std::uint32_t group = REX_LOAD_U32(menu + 392u);
  for (unsigned count = 0; group && count < 48u; ++count) {
    if (REX_LOAD_U32(group) == 0u) {
      break;
    }
    group = REX_LOAD_U32(group + 48u);
  }
  if (!group || REX_LOAD_U32(group) != 0u) {
    return;
  }
  const std::uint32_t entries = REX_LOAD_U32(group + 4u);
  if (!entries) {
    return;
  }

  // Only touch a group that is exactly the eleven canonical tags: anything else
  // is a different screen's group, or a rebuild that no longer matches.
  for (int i = 0; i < kStripCount; ++i) {
    const std::uint32_t rec = entries + 16u * static_cast<std::uint32_t>(i);
    if (REX_LOAD_U32(rec) != static_cast<std::uint32_t>(i) ||
        REX_LOAD_U32(rec + 12u) != kCanonicalTags[i]) {
      return;
    }
  }
  if (REX_LOAD_U32(entries + 16u * kStripCount) != 0xFFFFFFFFu) {
    return;
  }

  for (int i = 0; i < kStripCount; ++i) {
    REX_STORE_U32(entries + 16u * static_cast<std::uint32_t>(i) + 12u, TagAt(i));
  }
}

// Answers the hover label's BTX lookup while the strip is asking for the moved
// slot. Called from the sub_8223B780 hook in eternalsonata_options.cpp, which
// owns that hook. Returns a guest string address, or 0 to fall through.
std::uint32_t AchievementsLabelOverride(std::uint8_t* base, std::uint32_t sid) {
  if (!t_want_achievements_label || sid != kStatusLabelSid) {
    return 0;
  }
  return WriteLabel(base, &g_label_string, AchievementsLabel(base));
}

// The same for the Scrapbook, which rides Item Set's id.
std::uint32_t ScrapbookLabelOverride(std::uint8_t* base, std::uint32_t sid) {
  if (!t_want_scrapbook_label || sid != kItemSetLabelSid) {
    return 0;
  }
  return WriteLabel(base, &g_scrapbook_label_string, scrapbook_menu::EntryLabel(base));
}

}  // namespace option_strip

// The hover label is a third, independent thing: a single static title record
// (type 200, string id 22, x=35, y=17) whose text the game rewrites per cursor
// move. sub_8223B2F8(slot) is what rewrites it, an eleven-way switch on the
// *slot* (identity table at byte_82082138) picking a BTX id:
//
//   0 Items 22, 1 Item Set 33, 2 Status 23, 3 Party Level 29,
//   4 Score Pieces 26, 5 Photos 27, 6 Piano Music 38, 7 Music 39,
//   8 Options 45, 9 Load 32, 10 Save 30
//
// Nothing ties it to the record the slot came from, so reordering the strip
// leaves the labels behind unless the same permutation is applied here. Its
// only two callers are the strip's own sub_82236678 / sub_82236810, so this
// affects nothing else.
//
// The switch picks its BTX id from an immediate, so the moved slot cannot be
// given a new id through r3. It keeps Status' id 23 and the lookup the switch
// then performs is answered with our own string instead.
REX_EXTERN(__imp__sub_8223B2F8);

REX_HOOK_RAW(sub_8223B2F8) {
  const std::uint32_t slot = ctx.r3.u32;
  if (!option_strip::StripEdited() ||
      slot >= static_cast<std::uint32_t>(option_strip::StripCount())) {
    __imp__sub_8223B2F8(ctx, base);
    return;
  }
  const int index = static_cast<int>(slot);
  const int stock = option_strip::StockSlotAt(index);
  // The Scrapbook rides Item Set's slot, whose one lookup it then answers.
  ctx.r3.u32 = static_cast<std::uint32_t>(stock < 0 ? option_strip::kScrapbookSource : stock);
  option_strip::t_want_achievements_label = stock == option_strip::kSourceIndex;
  option_strip::t_want_scrapbook_label = option_strip::ScrapbookAt(index);
  __imp__sub_8223B2F8(ctx, base);
  option_strip::t_want_achievements_label = false;
  option_strip::t_want_scrapbook_label = false;
}

// sub_821EF0E8(ui, image id, ...): makes the image objects of list records
// 100 and 110 and returns the handle.
REX_EXTERN(__imp__sub_821EF0E8);

REX_HOOK_RAW(sub_821EF0E8) {
  const std::uint32_t image_id = ctx.r4.u32;
  __imp__sub_821EF0E8(ctx, base);
  option_strip::MaybeCatchExtraGlow(image_id, ctx.r3.u32);
}

// sub_8223B288(slot) lights the glow at object slot and dims the other ten. On
// the wide strip a slot past the Scrapbook names the record before it, and the
// Scrapbook's own glow is the appended one.
REX_EXTERN(__imp__sub_8223B288);

REX_HOOK_RAW(sub_8223B288) {
  const std::uint32_t slot = ctx.r3.u32;
  if (!option_strip::StripIsWide() ||
      slot >= static_cast<std::uint32_t>(option_strip::kWideCount)) {
    __imp__sub_8223B288(ctx, base);
    return;
  }
  const int record = option_strip::WideRecordAt(static_cast<int>(slot));
  ctx.r3.u32 = static_cast<std::uint32_t>(record < 0 ? option_strip::kStripCount : record);
  __imp__sub_8223B288(ctx, base);
  option_strip::SetExtraGlow(ctx, base, record < 0);
}

// Spanish alone re-registers the eleven selectables by hand (x = 445 + 70 * i,
// pitch 70 rather than the list's 75) after clearing group 0, with the tags
// hardcoded in drawn order. That undoes the permutation MaybeEditOptionStrip
// made, leaving the art and labels moved but every option opening the screen
// that used to sit at its position, so the tags are restated here. The wide
// strip is already at the PS3's tighter pitch, so Spanish keeps it as built.
REX_EXTERN(__imp__sub_8223B3E8);

REX_HOOK_RAW(sub_8223B3E8) {
  if (option_strip::StripIsWide()) {
    return;
  }
  __imp__sub_8223B3E8(ctx, base);
  option_strip::RepermuteSpanishSelectables(base);
}
