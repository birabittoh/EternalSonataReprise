// eternalsonata - The shop's equip panel, twelve characters wide.
//
// Buying equipment shows who can wear it as a 5x2 grid of character icons
// (sub_821FA908 builds it, sub_821FAF08 redraws it) and cycles a stats
// preview through those who can (the list sub_821FA7F8 and sub_821FB808
// build). All three stop at ten. The PS3 adds a dim, an icon and an equipped
// marker for CRS and SRN to the shop's display list and lays twelve members
// out 6x2 (sub_217FD8, sub_2159D8, sub_2165C0); this follows it.
//
// The new records go at the end of the list so the stock glyph indices the
// shop code reads stay put, which puts them at glyphs 35..40 instead of next
// to their siblings. The grid is then redrawn here in full after
// sub_821FAF08 runs.

#include "party_shop_menu.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "party_arrays.h"
#include "ps3_appkeep.h"
#include "target.h"

namespace {

// sub_821EC050 hands screen 23's shipped list to the interpreter uncopied, one
// per language (JPN USA GBR FRA ITA DEU ESP).
constexpr uint32_t kShopLists[] = {0x8205AFE8u, 0x820309D0u, 0x82064DB0u,
                                   0x82069C90u, 0x8205FEE0u, 0x82073A40u,
                                   0x8206EB68u};
constexpr uint32_t kListEnd = 0x0000FFFFu;
constexpr uint32_t kMaxListWords = 0x800u;
constexpr uint32_t kTypeGlyph = 100u;  // {100, image, x, y, sx, sy, handle}
constexpr uint32_t kGlyphWords = 7u;
constexpr uint32_t kTypeLayer = 3000u;
constexpr uint32_t kTypePop = 2101u;

// PS3 image ids of CRS and SRN's shop icons (AppKeep entries 268, 269).
constexpr uint32_t kPs3IconCrs = 269u;
constexpr uint32_t kPs3IconSrn = 270u;

// Glyph slots of the stock panel, then of the records appended after it.
constexpr uint32_t kDimGlyph = 0u;
constexpr uint32_t kCursorGlyph = 12u;
constexpr uint32_t kIconGlyph = 13u;  // character 1
constexpr uint32_t kMarkerGlyph = 25u;
constexpr uint32_t kStockGlyphs = 35u;
constexpr uint32_t kExtraDimGlyph = kStockGlyphs;
constexpr uint32_t kExtraIconGlyph = kStockGlyphs + 2u;
constexpr uint32_t kExtraMarkerGlyph = kStockGlyphs + 4u;
constexpr uint32_t kExtendedGlyphs = kStockGlyphs + 6u;

constexpr uint32_t kUiRoot = 0x824400E4u;
constexpr uint32_t kObjects = 0x824CF500u;
constexpr uint32_t kLanguage = 0x8243D370u;  // 0 is Japanese
constexpr uint32_t kItemFlags = 0x82017634u;  // u32 per 100 byte item record
constexpr uint32_t kItemStride = 100u;
constexpr uint32_t kEquippedOffset = 28u;  // four u16 item ids in base stats

// The panel's state object, dword_824409E0.
constexpr uint32_t kOwnerItem = 0u;
constexpr uint32_t kOwnerList = 4u;
constexpr uint32_t kOwnerCount = 44u;
constexpr uint32_t kOwnerCursor = 88u;
constexpr uint32_t kOwnerBuilt = 96u;
constexpr uint32_t kOwnerPage = 100u;
constexpr uint32_t kOwnerEquippable = 104u;

constexpr uint32_t kScreenGlyphs = 76u;
constexpr uint32_t kScreenGlyphCount = 380u;
constexpr uint32_t kLayoutTableOffset = 709u;

constexpr uint32_t kRetail = eternalsonata::kRetailCharacterCount;
constexpr uint32_t kTwelve = eternalsonata::kPartyCharacterCount;

// The PS3's display order (0x469000): the 360's, then CRS and SRN.
constexpr uint32_t kOrder[kTwelve] = {1, 2, 3, 6, 10, 4, 5, 8, 7, 9, 11, 12};

constexpr uint32_t kGrey = 0xFF999999u;
constexpr uint32_t kWhite = 0xFFFFFFFFu;

// Icon placement from the panel's origin, and the cursor's offset from its
// icon. Five columns are the 360's; six are the PS3's moved by the same
// amount its five column layout differs from the 360's.
struct Grid {
  int cols;
  float x0, dx, y0, dy, cursor_x, cursor_y;
};
constexpr Grid kFiveWestern = {5, 30.0f, 70.0f, 20.0f, 58.0f, 16.8f, 16.0f};
constexpr Grid kFiveJapanese = {5, 35.0f, 70.0f, 25.0f, 65.0f, 21.0f, 20.0f};
constexpr Grid kSixWestern = {6, 23.0f, 60.0f, 20.0f, 58.0f, 16.0f, 15.0f};
constexpr Grid kSixJapanese = {6, 28.5f, 60.0f, 26.5f, 58.5f, 18.9f, 18.0f};

bool g_extended = false;
uint32_t g_extra[kTwelve - kRetail] = {};
uint32_t g_scratch = 0;
uint32_t g_list_copy = 0;

// Through the guest address translation: the panel's objects live in the
// physical aperture, where base + address reads zeros.
uint32_t ReadU32(uint8_t* base, uint32_t address) { return REX_LOAD_U32(address); }
uint8_t ReadU8(uint8_t* base, uint32_t address) { return REX_LOAD_U8(address); }
void WriteU32(uint8_t* base, uint32_t address, uint32_t value) { REX_STORE_U32(address, value); }

bool Joined(uint8_t* base, uint32_t c) {
  return ReadU32(base, eternalsonata::PartyArrayAddress(
                           eternalsonata::PartyArray::kPosition, c - 1)) != 0;
}

uint32_t JoinedCount(uint8_t* base) {
  uint32_t n = 0;
  for (uint32_t c = 1; c <= kTwelve; ++c)
    n += Joined(base, c) ? 1u : 0u;
  return n;
}

uint32_t ItemFlags(uint8_t* base, int32_t item) {
  return item >= 1 ? ReadU32(base, kItemFlags + kItemStride * (item - 1)) : 0u;
}

// Bit 2 + c: ps3_item_tables.cpp moved the PS3's CRS and SRN bits down to 13
// and 14, so the 360's formula covers all twelve.
uint32_t EquipBit(uint32_t c) { return 8u << (c - 1); }

int FindRecord(const std::vector<uint32_t>& words, uint32_t layer) {
  for (size_t i = 0; i + 2 < words.size(); ++i) {
    if (words[i] == kTypeLayer && words[i + 1] == layer && words[i + 2] == kTypeGlyph)
      return static_cast<int>(i + 2);
  }
  return -1;
}

bool GlyphBlock(const std::vector<uint32_t>& words, int at, uint32_t count) {
  if (at < 0 || at + kGlyphWords * count > words.size())
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (words[at + kGlyphWords * i] != kTypeGlyph)
      return false;
  }
  return true;
}

uint32_t Glyph(uint8_t* base, uint32_t screen, uint32_t slot) {
  return ReadU32(base, screen + kScreenGlyphs + 4u * slot);
}

uint32_t DimGlyph(uint32_t k) {
  return k < kRetail ? kDimGlyph + k : kExtraDimGlyph + k - kRetail;
}
uint32_t IconGlyph(uint32_t c) {
  return c <= kRetail ? kIconGlyph + c - 1 : kExtraIconGlyph + c - 1 - kRetail;
}
uint32_t MarkerGlyph(uint32_t k) {
  return k < kRetail ? kMarkerGlyph + k : kExtraMarkerGlyph + k - kRetail;
}

// The panel's screen while it holds the extended list, else 0.
uint32_t ActiveScreen(uint8_t* base, uint32_t owner) {
  if (!g_extended || !owner || !ReadU8(base, owner + kOwnerBuilt))
    return 0;
  const uint32_t ui = ReadU32(base, kUiRoot);
  const uint32_t page = ReadU32(base, owner + kOwnerPage);
  const uint32_t screen = ui ? ReadU32(base, ui + 4u * (page + kLayoutTableOffset)) : 0;
  if (!screen || screen == UINT32_MAX ||
      ReadU8(base, screen + kScreenGlyphCount) != kExtendedGlyphs)
    return 0;
  return screen;
}

// sub_821FA7F8 / sub_821FB808's list of who can wear the item, for twelve.
// The owner holds ten; the rest live here and the reader is redirected.
void RebuildEquipList(uint8_t* base, uint32_t owner) {
  const int32_t item = static_cast<int32_t>(ReadU32(base, owner + kOwnerItem));
  if (item < 1)
    return;
  const uint32_t flags = ItemFlags(base, item);
  uint32_t count = 0;
  for (uint32_t c : kOrder) {
    if (!Joined(base, c) || !(flags & EquipBit(c)))
      continue;
    if (count < kRetail)
      WriteU32(base, owner + kOwnerList + 4u * count, c);
    else
      g_extra[count - kRetail] = c;
    ++count;
  }
  for (uint32_t i = count; i < kRetail; ++i)
    WriteU32(base, owner + kOwnerList + 4u * i, 0);
  WriteU32(base, owner + kOwnerCount, count);
}

}  // namespace

namespace party_shop_menu {

std::uint32_t MaybeSwapShopList(std::uint8_t* base, std::uint32_t list) {
  const auto* found = std::find(std::begin(kShopLists), std::end(kShopLists), list);
  if (found == std::end(kShopLists))
    return 0;
  g_extended = false;
  if (!eternalsonata::IsPs3Target())
    return 0;
  const uint32_t icon_crs = eternalsonata::Ps3AppKeepImageId(kPs3IconCrs);
  const uint32_t icon_srn = eternalsonata::Ps3AppKeepImageId(kPs3IconSrn);
  if (!icon_crs || !icon_srn)
    return 0;

  std::vector<uint32_t> words;
  for (uint32_t i = 0; i < kMaxListWords; ++i) {
    const uint32_t word = ReadU32(base, list + 4u * i);
    if (word == kListEnd)
      break;
    words.push_back(word);
  }
  const size_t tail = words.size() - 3;
  if (words.size() >= kMaxListWords || words.size() < 3 || words[tail] != kTypeLayer ||
      words[tail + 1] != 0 || words[tail + 2] != kTypePop)
    return 0;
  const int dims = FindRecord(words, 0);
  const int cursor = FindRecord(words, 1);
  const int icons = FindRecord(words, 2);
  const int markers = FindRecord(words, 3);
  if (!GlyphBlock(words, dims, kRetail) || !GlyphBlock(words, cursor, 1) ||
      !GlyphBlock(words, icons, kRetail) || !GlyphBlock(words, markers, kRetail))
    return 0;

  const bool japanese = found == std::begin(kShopLists);
  const bool six = JoinedCount(base) > kRetail;
  const auto at = [](int block, uint32_t k) {
    return static_cast<size_t>(block) + kGlyphWords * k;
  };
  const int32_t dim_x0 = static_cast<int32_t>(words[at(dims, 0) + 2]);
  const int32_t dim_y0 = static_cast<int32_t>(words[at(dims, 0) + 3]);
  const int32_t dim_dx = static_cast<int32_t>(words[at(dims, 1) + 2]) - dim_x0;
  const int32_t dim_dy = static_cast<int32_t>(words[at(dims, 5) + 3]) - dim_y0;
  const int32_t mark_x0 = static_cast<int32_t>(words[at(markers, 0) + 2]);
  const int32_t mark_y0 = static_cast<int32_t>(words[at(markers, 0) + 3]);

  std::vector<uint32_t> dim_records, marker_records;
  for (uint32_t k = 0; k < kTwelve; ++k) {
    std::vector<uint32_t> dim(words.begin() + at(dims, std::min(k, kRetail - 1)),
                              words.begin() + at(dims, std::min(k, kRetail - 1)) + kGlyphWords);
    std::vector<uint32_t> mark(words.begin() + at(markers, std::min(k, kRetail - 1)),
                               words.begin() + at(markers, std::min(k, kRetail - 1)) + kGlyphWords);
    // sub_2159D8's positions, moved into the list's space.
    int32_t col, row, dx, dy, mark_y;
    if (six) {
      col = static_cast<int32_t>(k % 6);
      row = static_cast<int32_t>(k / 6);
      const int32_t ry = japanese ? static_cast<int32_t>(std::floor(58.5f * row)) : row * dim_dy;
      dx = (japanese ? -3 : -8) + 60 * col;
      dy = (japanese ? 4 : 0) + ry;
      mark_y = ry;
      if (japanese) {
        dim[4] = dim[5] = 900;
        mark[4] = mark[5] = 450;
      }
    } else {
      col = k < kRetail ? static_cast<int32_t>(k % 5) : 5;
      row = k < kRetail ? static_cast<int32_t>(k / 5) : static_cast<int32_t>(k - kRetail);
      dx = col * dim_dx;
      dy = row * dim_dy;
      mark_y = dy;
    }
    dim[2] = static_cast<uint32_t>(dim_x0 + dx);
    dim[3] = static_cast<uint32_t>(dim_y0 + dy);
    mark[2] = static_cast<uint32_t>(mark_x0 + dx);
    mark[3] = static_cast<uint32_t>(mark_y0 + mark_y);
    if (k < kRetail) {
      std::copy(dim.begin(), dim.end(), words.begin() + at(dims, k));
      std::copy(mark.begin(), mark.end(), words.begin() + at(markers, k));
    } else {
      dim_records.insert(dim_records.end(), dim.begin(), dim.end());
      marker_records.insert(marker_records.end(), mark.begin(), mark.end());
    }
  }
  // The PS3 shrinks the Japanese panel's icons and cursor to fit six columns.
  if (six && japanese) {
    for (uint32_t c = 0; c < kRetail; ++c)
      words[at(icons, c) + 4] = words[at(icons, c) + 5] = 900;
    words[at(cursor, 0) + 4] = words[at(cursor, 0) + 5] = 900;
  }
  std::vector<uint32_t> icon_records;
  for (uint32_t image : {icon_crs, icon_srn}) {
    icon_records.insert(icon_records.end(), words.begin() + at(icons, kRetail - 1),
                        words.begin() + at(icons, kRetail - 1) + kGlyphWords);
    icon_records[icon_records.size() - kGlyphWords + 1] = image;
  }

  std::vector<uint32_t> out(words.begin(), words.begin() + tail);
  out.insert(out.end(), {kTypeLayer, 0});
  out.insert(out.end(), dim_records.begin(), dim_records.end());
  out.insert(out.end(), {kTypeLayer, 2});
  out.insert(out.end(), icon_records.begin(), icon_records.end());
  out.insert(out.end(), {kTypeLayer, 3});
  out.insert(out.end(), marker_records.begin(), marker_records.end());
  out.insert(out.end(), words.begin() + tail, words.end());
  if (out.size() >= kMaxListWords)
    return 0;

  if (!g_list_copy) {
    auto* mem = rex::system::kernel_memory();
    g_list_copy = mem ? mem->SystemHeapAlloc(4u * (kMaxListWords + 1u), 0x20) : 0;
    if (!g_list_copy)
      return 0;
  }
  for (size_t i = 0; i < out.size(); ++i)
    WriteU32(base, g_list_copy + 4u * static_cast<uint32_t>(i), out[i]);
  WriteU32(base, g_list_copy + 4u * static_cast<uint32_t>(out.size()), kListEnd);
  g_extended = true;
  return g_list_copy;
}

}  // namespace party_shop_menu

// After the reader at 0x821FAFDC loads list entry `index`: entries past the
// owner's ten come from the sidecar.
extern "C++" void PartyShopListEntry(PPCRegister& entry, PPCRegister& index) {
  if (index.u32 >= kRetail && index.u32 < kTwelve)
    entry.u64 = g_extra[index.u32 - kRetail];
}

// sub_82179160(objects, id, argb, 0, 0): tints a sprite.
// sub_82178A88(objects, id, &xyz, 0, 0): places one.
// sub_8217BF28(&out, objects, id): reads one's position.
REX_EXTERN(sub_82179160);
REX_EXTERN(sub_82178A88);
REX_EXTERN(sub_8217BF28);
REX_EXTERN(__imp__sub_821FAF08);

namespace {

void Tint(PPCContext& ctx, uint8_t* base, uint32_t id, uint32_t argb) {
  PPCContext call = ctx;
  call.r3.u64 = kObjects;
  call.r4.u64 = id;
  call.r5.u64 = argb;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82179160(call, base);
}

void Place(PPCContext& ctx, uint8_t* base, uint32_t id, float x, float y) {
  WriteU32(base, g_scratch + 16, std::bit_cast<uint32_t>(x));
  WriteU32(base, g_scratch + 20, std::bit_cast<uint32_t>(y));
  WriteU32(base, g_scratch + 24, 0);
  PPCContext call = ctx;
  call.r3.u64 = kObjects;
  call.r4.u64 = id;
  call.r5.u64 = g_scratch + 16;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82178A88(call, base);
}

bool Wears(uint8_t* base, uint32_t c, int32_t item) {
  const uint32_t equipped =
      eternalsonata::PartyArrayAddress(eternalsonata::PartyArray::kStatsBase, c - 1) +
      kEquippedOffset;
  for (uint32_t i = 0; i < 4; ++i) {
    const int16_t id = static_cast<int16_t>(
        REX_LOAD_U16(equipped + 2u * i));
    if (id == static_cast<int16_t>(item))
      return true;
  }
  return false;
}

// The grid half of sub_821FA908 and sub_821FAF08, over twelve.
void RedrawGrid(PPCContext& ctx, uint8_t* base, uint32_t owner, uint32_t screen) {
  if (!g_scratch) {
    auto* mem = rex::system::kernel_memory();
    g_scratch = mem ? mem->SystemHeapAlloc(32, 0x10) : 0;
    if (!g_scratch)
      return;
  }
  PPCContext call = ctx;
  call.r3.u64 = g_scratch;
  call.r4.u64 = kObjects;
  call.r5.u64 = ReadU32(base, screen);
  sub_8217BF28(call, base);
  const float origin_x = std::bit_cast<float>(ReadU32(base, g_scratch));
  const float origin_y = std::bit_cast<float>(ReadU32(base, g_scratch + 4));

  const bool western = ReadU32(base, kLanguage) != 0;
  const bool six = JoinedCount(base) > kRetail;
  const Grid& grid = six ? (western ? kSixWestern : kSixJapanese)
                         : (western ? kFiveWestern : kFiveJapanese);
  const int32_t item = static_cast<int32_t>(ReadU32(base, owner + kOwnerItem));
  const uint32_t flags = ItemFlags(base, item);
  const bool equippable = ReadU8(base, owner + kOwnerEquippable) != 0;
  const uint32_t selected = ReadU32(base, owner + kOwnerCursor);
  const uint32_t cursor = Glyph(base, screen, kCursorGlyph);

  // Nothing to wear: the game hides the panel, so the grid goes with it.
  if (!equippable) {
    for (uint32_t c = 1; c <= kTwelve; ++c)
      Tint(ctx, base, Glyph(base, screen, IconGlyph(c)), 0);
    for (uint32_t i = 0; i < kTwelve; ++i) {
      Tint(ctx, base, Glyph(base, screen, DimGlyph(i)), 0);
      Tint(ctx, base, Glyph(base, screen, MarkerGlyph(i)), 0);
    }
    Tint(ctx, base, cursor, 0);
    return;
  }

  uint32_t k = 0, can_index = 0;
  bool highlighted = false;
  for (uint32_t c : kOrder) {
    if (!Joined(base, c))
      continue;
    const float x = origin_x + grid.x0 + grid.dx * static_cast<float>(k % grid.cols);
    const float y = origin_y + grid.y0 + grid.dy * static_cast<float>(k / grid.cols);
    const uint32_t icon = Glyph(base, screen, IconGlyph(c));
    const uint32_t dim = Glyph(base, screen, DimGlyph(k));
    Place(ctx, base, icon, x, y);
    if (flags & EquipBit(c)) {
      Tint(ctx, base, icon, kWhite);
      Tint(ctx, base, dim, kWhite);
      if (!highlighted && selected == can_index) {
        Place(ctx, base, cursor, x - grid.cursor_x, y - grid.cursor_y);
        Tint(ctx, base, cursor, kWhite);
        highlighted = true;
      }
      ++can_index;
    } else {
      Tint(ctx, base, icon, kGrey);
      Tint(ctx, base, dim, kGrey);
    }
    Tint(ctx, base, Glyph(base, screen, MarkerGlyph(k)), Wears(base, c, item) ? kWhite : 0);
    ++k;
  }
  if (!highlighted)
    Tint(ctx, base, cursor, 0);
  for (; k < kTwelve; ++k) {
    Tint(ctx, base, Glyph(base, screen, DimGlyph(k)), 0);
    Tint(ctx, base, Glyph(base, screen, MarkerGlyph(k)), 0);
  }
}

}  // namespace

// sub_821FAF08(owner, step): redraws the panel, stepping the preview by step.
// sub_821FA908 ends in it, so this also covers the build.
REX_HOOK_RAW(sub_821FAF08) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t screen = ActiveScreen(base, owner);
  if (screen)
    RebuildEquipList(base, owner);
  __imp__sub_821FAF08(ctx, base);
  if (!screen)
    return;
  const uint64_t result = ctx.r3.u64;
  RedrawGrid(ctx, base, owner, screen);
  ctx.r3.u64 = result;
}
