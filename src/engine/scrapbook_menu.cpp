// eternalsonata - the PS3's Scrapbook camp page (docs/ps3-scrapbook.md).
//
// Ten captioned pictures, PS3 data only. option_strip.cpp gives the entry
// Item Set's place in the strip with tag 13; this file opens the Music gallery
// for it, as achievements_menu.cpp does for tag 11, and keeps only its header:
// the list and track name panels stay parked and no rows are built. In their
// place the page is the PS3's grid (sub_211108): ten framed cells in two
// columns of five, each titled and each its own selectable.
//
// Confirming a cell plays the picture's clip through the gallery's own stream
// start and moves the grid away for a panel appended to the display list: the
// ten pictures (campdata characterphoto.bmd, in AppKeep slots from
// kPs3ScrapbookSlot) with only the chosen one tinted visible, its title and its
// caption. Any button closes it.

#include "scrapbook_menu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

#include "achievements_menu.h"
#include "ps3_appkeep.h"
#include "ps3_item_tables.h"
#include "ps3_natives.h"
#include "target.h"

REX_EXTERN(__imp__sub_82226858);
REX_EXTERN(sub_8217BF28);
REX_EXTERN(sub_82178A88);
REX_EXTERN(sub_82179160);
REX_EXTERN(sub_821D4598);
REX_EXTERN(sub_821D3D50);

namespace scrapbook_menu {
namespace {

constexpr std::uint32_t kPictures = eternalsonata::kPs3ScrapbookCount;

// Ps3ScrapbookText ids past the pictures' titles and captions.
constexpr std::uint32_t kTextCaption = 10;
constexpr std::uint32_t kTextHeading = 21;
constexpr std::uint32_t kTextOpen = 22;

// The clip each picture plays, u32 table 0x468F84 in the EBOOT.
constexpr std::array<std::uint8_t, kPictures> kClips = {19, 21, 23, 22, 27, 25, 40, 38, 20, 53};

// The status menu's screen state, as achievements_menu.cpp names it.
constexpr std::uint32_t kScreenIdAddr = 0x8243F364u;
constexpr std::uint32_t kStateAddr = 0x8243F3B8u;
constexpr std::uint32_t kHistoryAddr = 0x8243F378u;
constexpr std::uint32_t kHistoryDepthAddr = 0x8243E8A3u;
constexpr std::uint32_t kHistoryMax = 0x10u;
constexpr std::uint32_t kMusicScreenId = 0xAu;

constexpr std::uint32_t kLanguageAddr = 0x8243D370u;
constexpr std::uint32_t kUiRootAddr = 0x824400E4u;
constexpr std::uint32_t kMenuAddr = 0x824400E8u;
constexpr std::uint32_t kObjects = 0x824CF500u;
constexpr std::uint32_t kTextManager = 0x82555690u;
constexpr std::uint32_t kSoundManagerAddr = 0x8243D89Cu;
constexpr std::uint32_t kSoundCancel = 3u;

// The gallery's state byte (2 slides in, 3 takes input, 4 slides out) and the
// countdown those states run on, at ui + 1636: 75 down for the slide in, 85
// down for the slide out.
constexpr std::uint32_t kGalleryStateAddr = 0x8243F3C2u;
constexpr std::uint8_t kGallerySlideIn = 2;
constexpr std::uint8_t kGalleryInput = 3;
constexpr std::uint8_t kGallerySlideOut = 4;
constexpr std::uint32_t kTimerOffset = 1636u;
constexpr float kSlideInFrames = 75.0f;
constexpr float kSlideOutFrames = 85.0f;

// The menu's selection result (a tag, -4 for cancel) and button word, and the
// cooldown the gallery arms after a choice.
constexpr std::uint32_t kResultOffset = 444u;
constexpr std::uint32_t kButtonsOffset = 448u;
constexpr std::int32_t kResultCancel = -4;
constexpr std::uint32_t kCooldownFlagOffset = 425u;
constexpr std::uint32_t kCooldownOffset = 428u;
// The group the cursor moves in, and the flag that puts it on the group's
// first selectable (both as sub_821FFC48 and sub_821FFEF8 use them).
constexpr std::uint32_t kActiveGroupOffset = 396u;
constexpr std::uint32_t kCursorInitOffset = 424u;
// The menu's linked list of groups: id at +0, selectable table at +8, next at +48.
constexpr std::uint32_t kGroupListOffset = 392u;
constexpr std::uint32_t kGroupTableOffset = 8u;
constexpr std::uint32_t kGroupNextOffset = 48u;
constexpr std::uint32_t kButtonTabLeft = 0x100u;
constexpr std::uint32_t kButtonTabRight = 0x200u;
constexpr std::uint32_t kButtonStop = 0x8000u;

// Music list records the page keeps off screen: the track name panel and the
// list panel, each held at the y its record parks it at.
constexpr std::uint32_t kNamePanel = 1u;
constexpr std::uint32_t kNamePanelParkedY = 880u;
constexpr std::uint32_t kListPanel = 2u;
constexpr std::uint32_t kListPanelParkedY = 1030u;
constexpr std::uint32_t kTypeSlide = 2u;
constexpr std::uint32_t kTypePanel = 1u;
// `9 x y w h kind`: a panel of a given frame kind (sub_821E8FE8 takes it as
// 3 * fill + border). Kind 10 is the gold framed one the camp grid gives the
// active party (sub_821E3408) and sub_211108 gives the cells.
constexpr std::uint32_t kTypeFramedPanel = 9u;
constexpr std::uint32_t kGoldFrame = 10u;
constexpr std::uint32_t kTypeIcon = 100u;
constexpr std::uint32_t kTypeText = 200u;
constexpr std::uint32_t kOpenChildren = 2100u;
constexpr std::uint32_t kCloseChildren = 2101u;
constexpr std::uint32_t kOpenSelection = 1500u;
constexpr std::uint32_t kCloseSelection = 1501u;
constexpr std::uint32_t kSelectable = 1502u;

// Header records: the title, and the confirm and cancel prompts.
constexpr std::uint32_t kSidTitle = 196u;
constexpr std::uint32_t kSidPerform = 158u;
constexpr std::uint32_t kSidBack = 53u;
constexpr std::uint32_t kIconConfirm = 236u;
constexpr std::uint32_t kIconCancel = 237u;
// sub_211108's spacing: the glyph 55 left of its label, the label ending 18
// short of the next glyph.
constexpr std::int32_t kPromptGlyphGap = 55;
constexpr std::int32_t kPromptLabelGap = 18;
// What one character of a 40 point prompt measures on average.
constexpr float kPromptCharWidth = 16.0f;

// The grid, sub_211108's cells: 430 x 75 gold framed panels on an 430 x 80 pitch,
// titled in a 390 x 48 box at 40 points. The cursor's selectables sit 50 right
// of and 20 below a cell's corner, the spot the cursor's own -50 lands left of.
constexpr std::int32_t kCellX = 210;
constexpr std::int32_t kCellY = 190;
constexpr std::uint32_t kCellW = 430;
constexpr std::uint32_t kCellH = 75;
constexpr std::int32_t kCellPitchX = 430;
constexpr std::int32_t kCellPitchY = 80;
constexpr std::int32_t kCellTextX = 20;
constexpr std::int32_t kCellTextY = 14;
constexpr std::uint32_t kCellTextW = 390;
constexpr std::uint32_t kCellTextH = 48;
constexpr std::uint32_t kCellTextSize = 40;
constexpr std::int32_t kSelectableDx = 50;
constexpr std::int32_t kSelectableDy = 20;
constexpr std::uint32_t kGridColumns = 2;
// Where the cells wait off screen, the distance the stock list slides.
constexpr float kGridSlide = 740.0f;
// Text ids the cell titles ask for; none of them is in the Music blob.
constexpr std::uint32_t kSidCellFirst = 0x7100u;

// Text the gallery asks for while the page is up.
constexpr std::uint32_t kMenuBlob = 0x82031A00u;

// The picture panel, the size the PS3 builds it at (sub_1F0C80), parked where
// the gallery parks its own track name panel.
constexpr std::int32_t kPanelX = 65;
constexpr std::int32_t kPanelY = 140;
constexpr std::int32_t kPanelHiddenY = 880;
constexpr std::uint32_t kPanelW = 1150;
constexpr std::uint32_t kPanelH = 543;
constexpr std::int32_t kPictureX = 24;
constexpr std::int32_t kPictureY = 40;
constexpr std::int32_t kColumnX = kPictureX + 816 + 24;
constexpr std::uint32_t kColumnW = kPanelW - kColumnX - 24;
constexpr float kAway = 1000.0f;


// Screen object layout: panel handles from +0 (count at +72), icon handles
// from +76 (count at +380), text handles from +384 (count at +640).
constexpr std::uint32_t kPanelCount = 72u;
constexpr std::uint32_t kIconBase = 76u;
constexpr std::uint32_t kIconCount = 380u;
constexpr std::uint32_t kTextBase = 384u;
constexpr std::uint32_t kTextCount = 640u;

constexpr std::uint32_t kListEnd = 0x0000FFFFu;
constexpr std::uint32_t kMaxListWords = 0x400u;
constexpr std::uint32_t kListCapacity = kMaxListWords + 256u;
constexpr std::uint32_t kOffscreen = static_cast<std::uint32_t>(-10000);

bool g_active = false;

struct View {
  std::uint32_t screen = 0;
  std::array<std::uint32_t, kPictures> cells{};
  std::array<std::uint32_t, kPictures> cell_texts{};
  std::uint32_t panel = 0;
  std::array<std::uint32_t, kPictures> pictures{};
  std::uint32_t title = 0;
  std::uint32_t caption = 0;
  bool ready = false;
  bool centred = false;
  // How far the cells currently sit below their place.
  float grid_offset = 0.0f;
};
View g_view;
bool g_pending_build = false;
// Panels the Music list builds before ours (header, track name, list), so
// the cells are panels g_stock_panels.. and the picture panel follows them.
std::uint32_t g_stock_panels = 0;
int g_open = -1;
std::uint32_t g_list_group = 0;
std::uint32_t g_picture_group = 0xFFFFFFFFu;

std::uint32_t g_list_copy = 0;
std::uint32_t g_scratch = 0;
std::uint32_t g_clip_name = 0;
std::uint32_t g_title_text = 0;
std::uint32_t g_caption_text = 0;

// Guest copies of what the lookups return, one per id, rewritten per call so
// a language change needs no restart.
std::array<std::uint32_t, 32> g_strings{};

std::uint32_t Alloc(std::uint32_t size) {
  auto* mem = rex::system::kernel_memory();
  return mem ? mem->SystemHeapAlloc(size, 0x20) : 0;
}

std::uint32_t Language(const std::uint8_t* base) {
  return rex::memory::load_and_swap<std::uint32_t>(base + kLanguageAddr);
}

std::string Text(const std::uint8_t* base, std::uint32_t id) {
  return eternalsonata::Ps3ScrapbookText(Language(base), id);
}

// sub_211108 sets the hints at 36 points for FRA ITA DEU ESP.
std::uint32_t PromptSize(const std::uint8_t* base) {
  const std::uint32_t language = Language(base);
  return language >= 3 && language <= 6 ? 36u : 40u;
}

// Into a buffer of `capacity` bytes, cut short rather than overrun.
void WriteGuestString(std::uint8_t* base, std::uint32_t address, const std::string& text,
                      std::uint32_t capacity) {
  const std::uint32_t length = std::min<std::uint32_t>(static_cast<std::uint32_t>(text.size()),
                                                       capacity - 1);
  for (std::uint32_t i = 0; i < length; ++i)
    REX_STORE_U8(address + i, static_cast<std::uint8_t>(text[i]));
  REX_STORE_U8(address + length, 0);
}

std::uint32_t GuestString(std::uint8_t* base, std::uint32_t slot, const std::string& text) {
  constexpr std::uint32_t kCapacity = 128;
  if (slot >= g_strings.size())
    return 0;
  if (!g_strings[slot] && !(g_strings[slot] = Alloc(kCapacity)))
    return 0;
  WriteGuestString(base, g_strings[slot], text, kCapacity);
  return g_strings[slot];
}

std::string StripMarkup(const std::string& text) {
  std::size_t at = 0;
  while (at < text.size() && text[at] == '<') {
    const std::size_t close = text.find('>', at);
    if (close == std::string::npos)
      break;
    at = close + 1;
  }
  return text.substr(at);
}

std::uint32_t ScreenOf(std::uint8_t* base, std::uint32_t root) {
  return REX_LOAD_U32(root + 4u * (REX_LOAD_U8(root + 2833u) + 709u));
}

std::uint32_t Resolve(PPCContext& ctx, std::uint8_t* base, std::uint32_t id) {
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kUiRootAddr);
  call.r4.u64 = id;
  sub_821F6580(call, base);
  return call.r3.u32;
}

void Tint(PPCContext& ctx, std::uint8_t* base, std::uint32_t id, std::uint32_t argb) {
  PPCContext call = ctx;
  call.r3.u64 = kObjects;
  call.r4.u64 = id;
  call.r5.u64 = argb;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82179160(call, base);
}

void Place(PPCContext& ctx, std::uint8_t* base, std::uint32_t id, float x, float y) {
  REX_STORE_U32(g_scratch + 16, std::bit_cast<std::uint32_t>(x));
  REX_STORE_U32(g_scratch + 20, std::bit_cast<std::uint32_t>(y));
  REX_STORE_U32(g_scratch + 24, 0);
  PPCContext call = ctx;
  call.r3.u64 = kObjects;
  call.r4.u64 = id;
  call.r5.u64 = g_scratch + 16;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82178A88(call, base);
}

// Relative, so it holds whatever space the object's position is kept in.
void Shift(PPCContext& ctx, std::uint8_t* base, std::uint32_t id, float dx, float dy) {
  if (!id || id == 0xFFFFFFFFu)
    return;
  PPCContext call = ctx;
  call.r3.u64 = g_scratch;
  call.r4.u64 = kObjects;
  call.r5.u64 = id;
  sub_8217BF28(call, base);
  const float x = std::bit_cast<float>(REX_LOAD_U32(g_scratch));
  const float y = std::bit_cast<float>(REX_LOAD_U32(g_scratch + 4));
  Place(ctx, base, id, x + dx, y + dy);
}

void SetGridOffset(PPCContext& ctx, std::uint8_t* base, float offset) {
  const float dy = offset - g_view.grid_offset;
  if (dy == 0.0f)
    return;
  for (const std::uint32_t cell : g_view.cells)
    Shift(ctx, base, cell, 0.0f, dy);
  g_view.grid_offset = offset;
}

// Each title in the middle of its cell, scaled down if it does not fit. Text
// records have no centring of their own, so this waits for the widths.
void CentreCellTitles(PPCContext& ctx, std::uint8_t* base) {
  for (const std::uint32_t id : g_view.cell_texts) {
    const std::uint32_t text = Resolve(ctx, base, id);
    if (!text)
      continue;
    PPCContext call = ctx;
    call.r3.u64 = kTextManager;
    call.r4.u64 = text;
    call.r5.u64 = 1;
    call.r6.u64 = 1;
    sub_821D4598(call, base);
    const float width = static_cast<float>(call.r3.s32);
    if (width <= 0.0f)
      continue;
    const float room = static_cast<float>(kCellTextW);
    const float fit = width > room ? room / width : 1.0f;
    if (fit < 1.0f) {
      call = ctx;
      call.r3.u64 = kTextManager;
      call.r4.u64 = text;
      call.f1.f64 = fit;
      sub_821D3D50(call, base);
    }
    Shift(ctx, base, id, (room - width * fit) * 0.5f, 0.0f);
  }
}

void SetText(PPCContext& ctx, std::uint8_t* base, std::uint32_t id, std::uint32_t buffer,
             const std::string& text) {
  constexpr std::uint32_t kCapacity = 400;  // the setter's own limit
  const std::uint32_t object = Resolve(ctx, base, id);
  if (!object || !buffer)
    return;
  WriteGuestString(base, buffer, text, kCapacity);
  PPCContext call = ctx;
  call.r3.u64 = kTextManager;
  call.r4.u64 = object;
  call.r5.u64 = buffer;
  call.r6.s64 = -1;
  sub_821D3890(call, base);
}

void PlayClip(PPCContext& ctx, std::uint8_t* base, int picture) {
  if (!g_clip_name && !(g_clip_name = Alloc(32)))
    return;
  char name[32];
  std::snprintf(name, sizeof(name), "MP1%02d.cxs", kClips[static_cast<std::size_t>(picture)]);
  WriteGuestString(base, g_clip_name, name, sizeof(name));
  PPCContext call = ctx;
  call.r3.u64 = 0x824251E8u;  // what the gallery passes; the callee ignores it
  call.r4.u64 = g_clip_name;
  sub_821F77F0(call, base);
}

void PlaySound(PPCContext& ctx, std::uint8_t* base, std::uint32_t id) {
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kSoundManagerAddr);
  call.r4.u64 = id;
  call.r5.u64 = 0;
  call.r6.u64 = 0;
  sub_821425D8(call, base);
}

void Cooldown(std::uint8_t* base, std::uint32_t menu, std::uint32_t frames) {
  REX_STORE_U8(menu + kCooldownFlagOffset, 0);
  REX_STORE_U32(menu + kCooldownOffset, frames);
}

// While a picture is up the cursor sits in a group of its own with one
// selectable, as sub_210A50 does on the PS3, so the cells behind cannot be
// reached. Confirm still answers with its tag, which closes the picture.
void IsolateCursor(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu) {
  PPCContext call = ctx;
  call.r3.u64 = menu;
  sub_821FFC48(call, base);
  const std::uint32_t group = call.r3.u32;
  if (group == 0xFFFFFFFFu)
    return;
  call = ctx;
  call.r3.u64 = menu;
  call.r4.u64 = group;
  call.r5.u64 = kOffscreen;
  call.r6.u64 = kOffscreen;
  call.r7.u64 = 1;
  sub_821FFEF8(call, base);
  g_list_group = REX_LOAD_U32(menu + kActiveGroupOffset);
  g_picture_group = group;
  REX_STORE_U32(menu + kActiveGroupOffset, group);
  REX_STORE_U8(menu + kCursorInitOffset, 1);
}

// sub_821FFB20 frees a group's selectables but not its 192 byte table at +8,
// so every picture opened would leak one until sub_821FFC48's allocation fails.
void FreeGroupTable(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu, std::uint32_t id) {
  for (std::uint32_t group = REX_LOAD_U32(menu + kGroupListOffset); group;
       group = REX_LOAD_U32(group + kGroupNextOffset)) {
    if (REX_LOAD_U32(group) != id)
      continue;
    if (const std::uint32_t table = REX_LOAD_U32(group + kGroupTableOffset)) {
      PPCContext call = ctx;
      call.r3.u64 = table;
      sub_82113FD0(call, base);
      REX_STORE_U32(group + kGroupTableOffset, 0);
    }
    return;
  }
}

void RestoreCursor(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu) {
  if (g_picture_group == 0xFFFFFFFFu)
    return;
  REX_STORE_U32(menu + kActiveGroupOffset, g_list_group);
  FreeGroupTable(ctx, base, menu, g_picture_group);
  PPCContext call = ctx;
  call.r3.u64 = menu;
  call.r4.u64 = g_picture_group;
  sub_821FFB20(call, base);
  g_picture_group = 0xFFFFFFFFu;
}

void OpenPicture(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu, int picture) {
  if (!g_view.ready)
    return;
  if (!g_title_text)
    g_title_text = Alloc(400);
  if (!g_caption_text)
    g_caption_text = Alloc(400);
  for (std::uint32_t i = 0; i < kPictures; ++i)
    Tint(ctx, base, g_view.pictures[i], static_cast<int>(i) == picture ? 0xFFFFFFFFu : 0u);
  const auto n = static_cast<std::uint32_t>(picture);
  SetText(ctx, base, g_view.title, g_title_text, Text(base, n));
  SetText(ctx, base, g_view.caption, g_caption_text, Text(base, kTextCaption + n));
  SetGridOffset(ctx, base, kAway);
  Shift(ctx, base, g_view.panel, 0.0f, static_cast<float>(kPanelY - kPanelHiddenY));
  PlayClip(ctx, base, picture);
  IsolateCursor(ctx, base, menu);
  g_open = picture;
}

void ClosePicture(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu) {
  if (g_open < 0)
    return;
  RestoreCursor(ctx, base, menu);
  Shift(ctx, base, g_view.panel, 0.0f, static_cast<float>(kPanelHiddenY - kPanelY));
  SetGridOffset(ctx, base, 0.0f);
  g_open = -1;
}

bool IsMusicList(std::uint32_t list) { return achievements_menu::IsMusicScreenList(list); }

// The prompt pair `icon` then `label`: a seven word icon record directly
// followed by a ten word text record. Moved off screen, not removed, so every
// later record keeps its object index.
void HidePrompt(std::vector<std::uint32_t>& words, std::uint32_t icon, std::uint32_t label) {
  for (std::size_t i = 0; i + 17 <= words.size(); ++i) {
    if (words[i] == kTypeIcon && words[i + 1] == icon && words[i + 7] == kTypeText &&
        words[i + 8] == label) {
      words[i + 2] = kOffscreen;
      words[i + 9] = kOffscreen;
      return;
    }
  }
}

void HideRecord(std::vector<std::uint32_t>& words, std::uint32_t type, std::uint32_t id) {
  for (std::size_t i = 0; i + 3 <= words.size(); ++i) {
    if (words[i] == type && words[i + 1] == id) {
      words[i + 2] = kOffscreen;
      return;
    }
  }
}

// The record `type id x y ...`, or the list's size.
std::size_t FindRecord(const std::vector<std::uint32_t>& words, std::uint32_t type,
                       std::uint32_t id) {
  for (std::size_t i = 0; i + 4 <= words.size(); ++i) {
    if (words[i] == type && words[i + 1] == id)
      return i;
  }
  return words.size();
}

// Leaves panel `panel` where its record parks it, below the screen, by giving
// its slide in that same spot as the target.
void KeepPanelParked(std::vector<std::uint32_t>& words, std::uint32_t panel, std::uint32_t y) {
  for (std::size_t i = 1; i + 4 <= words.size(); ++i) {
    if (words[i - 1] == kCloseChildren && words[i] == kTypeSlide && words[i + 1] == panel) {
      words[i + 3] = y;
      return;
    }
  }
}

// The PS3 header: a smaller title, and Display drawn right up against Back
// where the Music screen spreads its three prompts out.
void ReshapeHeader(const std::uint8_t* base, std::vector<std::uint32_t>& words) {
  const std::size_t title = FindRecord(words, kTypeText, kSidTitle);
  if (title + 10 <= words.size()) {
    // sub_211108's title offset and size per language.
    const std::uint32_t language = Language(base);
    const auto [dy, size] = language == 4                    ? std::pair{-2, 38u}
                            : language == 3 || language == 6 ? std::pair{-4, 40u}
                                                             : std::pair{11, 48u};
    words[title + 3] = static_cast<std::uint32_t>(dy);
    words[title + 7] = size;
  }

  const std::uint32_t size = PromptSize(base);
  const std::size_t back = FindRecord(words, kTypeText, kSidBack);
  if (back + 10 <= words.size())
    words[back + 7] = size;
  const std::size_t back_icon = FindRecord(words, kTypeIcon, kIconCancel);
  const std::size_t icon = FindRecord(words, kTypeIcon, kIconConfirm);
  const std::size_t label = FindRecord(words, kTypeText, kSidPerform);
  if (back_icon == words.size() || icon == words.size() || label + 10 > words.size())
    return;
  const float width = kPromptCharWidth * static_cast<float>(size) / 40.0f *
                      static_cast<float>(Text(base, kTextOpen).size());
  const std::int32_t label_x = static_cast<std::int32_t>(words[back_icon + 2]) -
                               kPromptLabelGap - static_cast<std::int32_t>(width);
  words[label + 2] = static_cast<std::uint32_t>(label_x);
  words[label + 7] = size;
  words[icon + 2] = static_cast<std::uint32_t>(label_x - kPromptGlyphGap);
}

// The Music list's four row selectables give way to the ten cells', in
// reading order, so the cursor's same row / same column search walks the grid.
void ReplaceSelectables(std::vector<std::uint32_t>& words) {
  std::size_t open = words.size();
  for (std::size_t i = 0; i + 2 <= words.size(); ++i) {
    if (words[i] == kOpenSelection) {
      open = i;
      break;
    }
  }
  if (open == words.size())
    return;
  std::size_t close = open + 2;
  while (close + 4 <= words.size() && words[close] == kSelectable)
    close += 4;
  if (close >= words.size() || words[close] != kCloseSelection)
    return;
  std::vector<std::uint32_t> cells;
  for (std::uint32_t i = 0; i < kPictures; ++i) {
    const auto column = static_cast<std::int32_t>(i % kGridColumns);
    const auto row = static_cast<std::int32_t>(i / kGridColumns);
    cells.insert(cells.end(),
                 {kSelectable,
                  static_cast<std::uint32_t>(kCellX + kCellPitchX * column + kSelectableDx),
                  static_cast<std::uint32_t>(kCellY + kCellPitchY * row + kSelectableDy), i + 1});
  }
  words.erase(words.begin() + static_cast<std::ptrdiff_t>(open + 2),
              words.begin() + static_cast<std::ptrdiff_t>(close));
  words.insert(words.begin() + static_cast<std::ptrdiff_t>(open + 2), cells.begin(), cells.end());
}

// A panel record, `1 x y w h` or `9 x y w h kind`, opening its children with
// `2100 n`.
std::uint32_t CountPanels(const std::vector<std::uint32_t>& words) {
  std::uint32_t count = 0;
  for (std::size_t i = 6; i + 1 < words.size(); ++i) {
    const bool panel = words[i - 5] == kTypePanel || words[i - 6] == kTypeFramedPanel;
    if (words[i] == kOpenChildren && panel && words[i + 1] < 32u)
      count = std::max(count, words[i + 1] + 1);
  }
  return count;
}

// The cells start below the screen; Tick brings them up with the header.
void AppendCells(std::vector<std::uint32_t>& words) {
  for (std::uint32_t i = 0; i < kPictures; ++i) {
    const auto column = static_cast<std::int32_t>(i % kGridColumns);
    const auto row = static_cast<std::int32_t>(i / kGridColumns);
    const auto x = static_cast<std::uint32_t>(kCellX + kCellPitchX * column);
    const auto y = static_cast<std::uint32_t>(kCellY + kCellPitchY * row +
                                              static_cast<std::int32_t>(kGridSlide));
    words.insert(words.end(), {kTypeFramedPanel, x, y, kCellW, kCellH, kGoldFrame,
                               kOpenChildren, g_stock_panels + i});
    words.insert(words.end(), {kTypeText, kSidCellFirst + i,
                               static_cast<std::uint32_t>(kCellTextX),
                               static_cast<std::uint32_t>(kCellTextY), kCellTextW, kCellTextH, 0,
                               kCellTextSize, 0xFFFFFFFFu, 1});
    words.push_back(kCloseChildren);
  }
}

void AppendPicturePanel(std::vector<std::uint32_t>& words) {
  const std::uint32_t pictures = eternalsonata::kPs3ScrapbookSlot + 1u;
  words.insert(words.end(), {kTypePanel, kPanelX, static_cast<std::uint32_t>(kPanelHiddenY),
                             kPanelW, kPanelH, kOpenChildren, g_stock_panels + kPictures});
  for (std::uint32_t i = 0; i < kPictures; ++i)
    words.insert(words.end(), {kTypeIcon, pictures + i, kPictureX, kPictureY, 1000, 1000,
                               0xFFFFFFFFu});
  words.insert(words.end(),
               {kTypeText, 0, kColumnX, kPictureY, kColumnW, 96, 0, 40, 0xFFFFFFFFu, 1});
  words.insert(words.end(), {kTypeText, 0, kColumnX, kPictureY + 110, kColumnW, 340, 0, 34,
                             0xFFFFFFFFu, 1});
  words.push_back(kCloseChildren);
}

// Below the screen while the gallery slides in or out, eased as the header is.
float GridOffsetFor(std::uint8_t* base) {
  const std::uint32_t ui = REX_LOAD_U32(kUiRootAddr);
  const float timer = ui ? static_cast<float>(static_cast<std::int32_t>(
                               REX_LOAD_U32(ui + kTimerOffset)))
                         : 0.0f;
  switch (REX_LOAD_U8(kGalleryStateAddr)) {
    case kGallerySlideIn: {
      const float left = std::clamp(timer / kSlideInFrames, 0.0f, 1.0f);
      return kGridSlide * left * left;
    }
    case kGalleryInput:
      return 0.0f;
    case kGallerySlideOut: {
      const float done = std::clamp(1.0f - timer / kSlideOutFrames, 0.0f, 1.0f);
      return kGridSlide * done * done;
    }
    default:
      return REX_LOAD_U8(kGalleryStateAddr) > kGallerySlideOut ? kGridSlide : g_view.grid_offset;
  }
}

}  // namespace

bool StripShowsScrapbook() {
  return eternalsonata::IsPs3Target() && eternalsonata::Ps3ScrapbookUnlocked() &&
         eternalsonata::Ps3ScrapbookAvailable();
}

std::string EntryLabel(const std::uint8_t* base) { return StripMarkup(Text(base, kTextHeading)); }

bool Active() { return g_active; }

bool Dispatch(std::uint8_t* base, std::uint32_t tag) {
  if (tag != kTag) {
    g_active = false;
    return false;
  }
  // Without pictures the tag falls to the switch's default arm, a no-op.
  if (!eternalsonata::LoadPs3Scrapbook() || (!g_scratch && !(g_scratch = Alloc(32)))) {
    g_active = false;
    return false;
  }
  g_active = true;
  g_open = -1;
  g_picture_group = 0xFFFFFFFFu;
  g_view = {};
  // loc_82236F08 with the Music gallery's id, as achievements_menu.cpp does.
  const std::uint8_t depth = base[kHistoryDepthAddr];
  if (depth < kHistoryMax) {
    rex::memory::store_and_swap<std::uint32_t>(base + kHistoryAddr + 4u * depth, 0u);
    base[kHistoryDepthAddr] = static_cast<std::uint8_t>(depth + 1);
  }
  rex::memory::store_and_swap<std::uint32_t>(base + kScreenIdAddr, kMusicScreenId);
  base[kStateAddr] = 0u;
  return true;
}

void Tick(PPCContext& ctx, std::uint8_t* base) {
  if (!g_view.ready)
    return;
  if (!g_view.centred) {
    CentreCellTitles(ctx, base);
    g_view.centred = true;
  }
  if (g_open < 0)
    SetGridOffset(ctx, base, GridOffsetFor(base));
}

void Input(PPCContext& ctx, std::uint8_t* base) {
  const std::uint32_t menu = REX_LOAD_U32(kMenuAddr);
  if (!menu) {
    __imp__sub_82226858(ctx, base);
    return;
  }
  const auto result = static_cast<std::int32_t>(REX_LOAD_U32(menu + kResultOffset));
  const std::uint32_t buttons = REX_LOAD_U32(menu + kButtonsOffset);
  if (g_open >= 0) {
    // The grid is away and the cursor is held by the picture's own group.
    if (result != -1 || (buttons & kButtonStop)) {
      ClosePicture(ctx, base, menu);
      PlaySound(ctx, base, kSoundCancel);
      Cooldown(base, menu, 15);
    }
    return;
  }
  if (result >= 1 && result <= static_cast<std::int32_t>(kPictures) &&
      REX_LOAD_U8(kGalleryStateAddr) == kGalleryInput) {
    OpenPicture(ctx, base, menu, result - 1);
    Cooldown(base, menu, 30);
    return;
  }
  // The gallery only ever sees a cancel: its scrolling, tab steps and Y stop
  // all act on the rows this page never builds.
  if (result != kResultCancel)
    return;
  REX_STORE_U32(menu + kButtonsOffset, buttons & ~(kButtonTabLeft | kButtonTabRight | kButtonStop));
  __imp__sub_82226858(ctx, base);
  REX_STORE_U32(menu + kButtonsOffset, buttons);
}

std::uint32_t TextOverride(std::uint8_t* base, std::uint32_t blob, std::uint32_t sid) {
  if (!g_active || blob != kMenuBlob)
    return 0;
  if (sid == kSidPerform)
    return GuestString(base, kTextOpen, Text(base, kTextOpen));
  if (sid >= kSidCellFirst && sid < kSidCellFirst + kPictures) {
    const std::uint32_t picture = sid - kSidCellFirst;
    return GuestString(base, picture, Text(base, picture));
  }
  return 0;
}

std::uint32_t MaybeSwapScreenList(std::uint8_t* base, std::uint32_t list) {
  if (!g_active || !IsMusicList(list))
    return 0;
  std::vector<std::uint32_t> words;
  for (std::uint32_t i = 0; i < kMaxListWords; ++i) {
    const std::uint32_t word = REX_LOAD_U32(list + 4u * i);
    if (word == kListEnd)
      break;
    words.push_back(word);
  }
  if (words.size() >= kMaxListWords)
    return 0;

  HidePrompt(words, 239u, 159u);  // Y, Stop
  HideRecord(words, kTypeIcon, 240u);  // LB, RB and Change Tab
  HideRecord(words, kTypeIcon, 242u);
  HideRecord(words, kTypeText, 64u);
  HideRecord(words, kTypeText, 125u);  // the Japanese D-pad and its label
  HideRecord(words, 700u, 148u);
  // The PS3 page has no track name line and no list.
  KeepPanelParked(words, kNamePanel, kNamePanelParkedY);
  KeepPanelParked(words, kListPanel, kListPanelParkedY);
  g_stock_panels = CountPanels(words);
  ReshapeHeader(base, words);
  ReplaceSelectables(words);
  AppendCells(words);
  AppendPicturePanel(words);

  if (!g_list_copy && !(g_list_copy = Alloc(4u * kListCapacity)))
    return 0;
  if (words.size() + 1 > kListCapacity)
    return 0;
  for (std::size_t i = 0; i < words.size(); ++i)
    REX_STORE_U32(g_list_copy + 4u * static_cast<std::uint32_t>(i), words[i]);
  REX_STORE_U32(g_list_copy + 4u * static_cast<std::uint32_t>(words.size()), kListEnd);
  g_pending_build = true;
  return g_list_copy;
}

// The appended records are the last eleven panels (ten cells and the picture
// panel), the last ten icons, and the last twelve texts (one per cell, then
// the picture's title and caption).
void ScreenBuilt(std::uint8_t* base, std::uint32_t root) {
  if (!g_pending_build)
    return;
  g_pending_build = false;
  g_view = {};
  const std::uint32_t screen = ScreenOf(base, root);
  if (!screen)
    return;
  const std::uint32_t panels = REX_LOAD_U8(screen + kPanelCount);
  const std::uint32_t icons = REX_LOAD_U8(screen + kIconCount);
  const std::uint32_t texts = REX_LOAD_U8(screen + kTextCount);
  const std::uint32_t picture_panel = g_stock_panels + kPictures;
  if (panels != picture_panel + 1 || icons < kPictures || texts < kPictures + 2) {
    REXLOG_WARN("[scrapbook] grid not built: {} panels, {} icons, {} texts", panels, icons,
                texts);
    return;
  }
  g_view.screen = screen;
  for (std::uint32_t i = 0; i < kPictures; ++i) {
    g_view.cells[i] = REX_LOAD_U32(screen + 4u * (g_stock_panels + i));
    g_view.cell_texts[i] = REX_LOAD_U32(screen + kTextBase + 4u * (texts - kPictures - 2 + i));
    g_view.pictures[i] = REX_LOAD_U32(screen + kIconBase + 4u * (icons - kPictures + i));
  }
  g_view.panel = REX_LOAD_U32(screen + 4u * picture_panel);
  g_view.title = REX_LOAD_U32(screen + kTextBase + 4u * (texts - 2));
  g_view.caption = REX_LOAD_U32(screen + kTextBase + 4u * (texts - 1));
  g_view.grid_offset = kGridSlide;
  const auto built = [](const auto& ids) {
    return std::find(ids.begin(), ids.end(), 0xFFFFFFFFu) == ids.end();
  };
  g_view.ready = built(g_view.cells) && built(g_view.cell_texts) && built(g_view.pictures);
}

}  // namespace scrapbook_menu
