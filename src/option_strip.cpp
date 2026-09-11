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

#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

#include "option_strip.h"

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

// Reorder one run in place: record at new position i is old position kOrder[i],
// with x restated so the run stays evenly spaced from its own first record.
// Both runs are permuted the same way, which is what keeps the art, the cursor
// and the label in step. Safe because the list is the interpreter's writable
// stack copy.
void PermuteRun(std::uint8_t* base, std::uint32_t start, std::uint32_t stride,
                std::uint32_t x_offset) {
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
      WriteU32(base, dst + off, saved[kOrder[i]][off / 4u]);
    }
    WriteU32(base, dst + x_offset,
             static_cast<std::uint32_t>(first_x + kIconSpacing * i));
  }
}

std::mutex g_edited_mutex;
bool g_edited = false;

// Set only for the duration of the stock sub_8223B2F8 call made for the moved
// slot, so the one BTX lookup that call performs is ours. Thread local because
// the flag is only ever meaningful inside that single guest call.
thread_local bool t_want_achievements_label = false;

std::uint32_t g_label_string = 0;  // guest address, allocated on first use

bool StripEdited() {
  std::lock_guard<std::mutex> lock(g_edited_mutex);
  return g_edited;
}

const char* AchievementsLabel(const std::uint8_t* base) {
  const std::uint32_t language = ReadU32(base, kLanguageIndexAddress);
  return kAchievementsLabel[language < 7u ? language : 1u];
}

}  // namespace

const char* AchievementsWord(const std::uint8_t* base) {
  return AchievementsLabel(base);
}

void MaybeEditOptionStrip(std::uint8_t* base, std::uint32_t list) {
  if (!list) {
    return;
  }
  const std::uint32_t end = FindListEnd(base, list);
  if (!end) {
    return;
  }
  const std::uint32_t strip = FindIconStrip(base, list, end);
  if (!strip) {
    return;
  }
  Run sel;
  if (!FindSelectableRun(base, list, end, &sel)) {
    return;  // never edit the art without the selection, or they drift apart
  }
  const std::uint32_t glow = FindGlowRun(base, list, end);
  if (!glow) {
    return;  // same for the glow: a stale one sits behind the wrong icon
  }

  if (sel.stride > 0x80u) {
    return;  // beyond what PermuteRun can hold; refuse rather than corrupt
  }

  // Both runs keep eleven records, so nothing has to be inserted or deleted and
  // the cursor still stops at Save. The selectable records carry their own tags
  // and travel with their art, so every option that moves keeps its screen.
  PermuteRun(base, strip, kIconStride, 8u);
  PermuteRun(base, glow, kGlowStride, 8u);
  PermuteRun(base, sel.start, sel.stride, 4u);
  WriteU32(base,
           sel.start + sel.stride * static_cast<std::uint32_t>(kTargetIndex) +
               12u,
           kAchievementsTag);

  {
    std::lock_guard<std::mutex> lock(g_edited_mutex);
    g_edited = true;
  }
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
    const std::uint32_t tag = i == kTargetIndex
                                  ? kAchievementsTag
                                  : kCanonicalTags[kOrder[i]];
    REX_STORE_U32(entries + 16u * static_cast<std::uint32_t>(i) + 12u, tag);
  }
}

// Answers the hover label's BTX lookup while the strip is asking for the moved
// slot. Called from the sub_8223B780 hook in eternalsonata_options.cpp, which
// owns that hook. Returns a guest string address, or 0 to fall through.
std::uint32_t AchievementsLabelOverride(std::uint8_t* base, std::uint32_t sid) {
  if (!t_want_achievements_label || sid != kStatusLabelSid) {
    return 0;
  }
  if (!g_label_string) {
    auto* mem = rex::system::kernel_memory();
    g_label_string = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
    if (!g_label_string) {
      return 0;
    }
  }
  // Rewritten on every lookup: the language can change without a restart.
  const char* label = AchievementsLabel(base);
  for (std::uint32_t i = 0;; ++i) {
    REX_STORE_U8(g_label_string + i, static_cast<std::uint8_t>(label[i]));
    if (!label[i]) {
      break;
    }
  }
  return g_label_string;
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
      slot >= static_cast<std::uint32_t>(option_strip::kStripCount)) {
    __imp__sub_8223B2F8(ctx, base);
    return;
  }
  ctx.r3.u32 = static_cast<std::uint32_t>(option_strip::kOrder[slot]);
  option_strip::t_want_achievements_label =
      slot == static_cast<std::uint32_t>(option_strip::kTargetIndex);
  __imp__sub_8223B2F8(ctx, base);
  option_strip::t_want_achievements_label = false;
}

// Spanish alone re-registers the eleven selectables by hand (x = 445 + 70 * i,
// pitch 70 rather than the list's 75) after clearing group 0, with the tags
// hardcoded in drawn order. That undoes the permutation MaybeEditOptionStrip
// made, leaving the art and labels moved but every option opening the screen
// that used to sit at its position, so the tags are restated here.
REX_EXTERN(__imp__sub_8223B3E8);

REX_HOOK_RAW(sub_8223B3E8) {
  __imp__sub_8223B3E8(ctx, base);
  option_strip::RepermuteSpanishSelectables(base);
}
