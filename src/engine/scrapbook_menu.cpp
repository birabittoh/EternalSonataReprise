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
#include <string_view>
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
REX_EXTERN(sub_821796B8);
REX_EXTERN(sub_82179780);
REX_EXTERN(sub_82179848);

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
// active party (sub_821E3408), sub_211108 the cells and sub_1F0C80 the
// picture view.
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

// The picture panel, the size and kind the PS3 builds it at (sub_1F0C80),
// parked where the gallery parks its own track name panel.
constexpr std::int32_t kPanelX = 65;
constexpr std::int32_t kPanelY = 140;
constexpr std::int32_t kPanelHiddenY = 880;
constexpr std::uint32_t kPanelW = 1150;
constexpr std::uint32_t kPanelH = 543;
constexpr std::uint32_t kPictureX = 298;
constexpr std::uint32_t kPictureY = 34;
// Title and caption down the left of the picture. The caption is one text
// per line so the lines can sit on sub_1F0C80's rules: four, 60 apart, or
// seven, 46 apart, for FRA ITA DEU ESP. The rules sit 4 above the PS3's 250
// and 242, as our smaller text ends higher.
constexpr std::uint32_t kColumnX = 40;
constexpr std::uint32_t kColumnW = 230;
constexpr std::uint32_t kTitleY = 100;
constexpr std::uint32_t kTitleSize = 44;
constexpr std::uint32_t kCaptionY = 200;
constexpr std::uint32_t kCaptionLines = 7;
constexpr std::uint32_t kTypeRule = 600u;  // `600 x y width`, sub_821F0D90
struct CaptionLayout {
  std::uint32_t rules, step, first_rule, size;
};
constexpr CaptionLayout kShortCaptions = {4, 60, 246, 40};
constexpr CaptionLayout kLongCaptions = {7, 46, 238, 30};
// Italian's longer words crowd the column at those sizes.
constexpr std::uint32_t kItalian = 4;
constexpr std::uint32_t kItalianTitleSize = 34;
constexpr std::uint32_t kItalianCaptionSize = 24;
// Rule handles from +908, count at +972.
constexpr std::uint32_t kRuleBase = 908u;
constexpr std::uint32_t kRuleCount = 972u;

// The picture's frame: AppKeep image 303 (the PS3's store id 312), the one
// the Photos menu frames its thumbnails with, cut into nine pieces as
// sub_1F0C80 does: quarter texture corners, the middle half stretched. The
// PS3 starts the middle row at 40, two short of the corners, which leaves a
// line between it and the bottom row.
constexpr std::uint32_t kFrameImage = 303;
struct FramePiece {
  std::uint32_t x, y;
  float w, h, u0, v0, u1, v1;
};
constexpr std::array<FramePiece, 9> kFrame = {{
    {274, 10, 32, 32, 0.0f, 0.0f, 0.25f, 0.25f},
    {1108, 10, 32, 32, 0.75f, 0.0f, 1.0f, 0.25f},
    {274, 508, 32, 32, 0.0f, 0.75f, 0.25f, 1.0f},
    {1108, 508, 32, 32, 0.75f, 0.75f, 1.0f, 1.0f},
    {306, 10, 802, 32, 0.25f, 0.0f, 0.75f, 0.25f},
    {274, 42, 32, 466, 0.0f, 0.25f, 0.25f, 0.75f},
    {1108, 42, 32, 466, 0.75f, 0.25f, 1.0f, 0.75f},
    {306, 508, 802, 32, 0.25f, 0.75f, 0.75f, 1.0f},
    {306, 42, 802, 466, 0.25f, 0.25f, 0.75f, 0.75f},
}};
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
  std::array<std::uint32_t, kFrame.size()> frame{};
  std::array<std::uint32_t, kPictures> pictures{};
  std::uint32_t title = 0;
  std::array<std::uint32_t, kCaptionLines> caption{};
  std::array<std::uint32_t, kCaptionLines> rules{};
  // The confirm prompt, which the picture view does not show.
  std::uint32_t prompt_icon = 0;
  std::uint32_t prompt_text = 0;
  bool ready = false;
  bool laid_out = false;
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
std::uint32_t g_quad = 0;
std::uint32_t g_clip_name = 0;
std::uint32_t g_title_text = 0;
std::array<std::uint32_t, kCaptionLines> g_caption_text{};
// Where the confirm prompt lands in the screen's icon and text lists, taken
// while its label is looked up during the walk.
std::uint32_t g_prompt_icon_index = 0xFFFFFFFFu;
std::uint32_t g_prompt_text_index = 0xFFFFFFFFu;

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

// Without the space French sets before ! ? : and ;, which the narrow column
// would otherwise strand at the start of a line.
std::string Text(const std::uint8_t* base, std::uint32_t id) {
  std::string text = eternalsonata::Ps3ScrapbookText(Language(base), id);
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool before_mark = i + 1 < text.size() && std::string_view("!?:;").find(text[i + 1]) !=
                                                        std::string_view::npos;
    if (!(text[i] == ' ' && before_mark))
      out += text[i];
  }
  return out;
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

// Corner positions and UVs as {x, y} pairs, top left, top right, bottom left,
// bottom right; sub_822057D8 shapes the Photos menu's frames the same way.
void ShapeFrame(PPCContext& ctx, std::uint8_t* base) {
  if (!g_quad && !(g_quad = Alloc(64)))
    return;
  const auto store = [&](std::uint32_t at, std::initializer_list<float> values) {
    for (const float value : values) {
      REX_STORE_U32(at, std::bit_cast<std::uint32_t>(value));
      at += 4;
    }
  };
  for (std::size_t i = 0; i < kFrame.size(); ++i) {
    const FramePiece& piece = kFrame[i];
    store(g_quad, {0.0f, 0.0f, piece.w, 0.0f, 0.0f, piece.h, piece.w, piece.h});
    store(g_quad + 32, {piece.u0, piece.v0, piece.u1, piece.v0, piece.u0, piece.v1, piece.u1,
                        piece.v1});
    for (const auto& [set, arg] : {std::pair{&sub_821796B8, g_quad},
                                   std::pair{&sub_82179780, g_quad + 32},
                                   std::pair{&sub_82179848, 1u}}) {
      PPCContext call = ctx;
      call.r3.u64 = kObjects;
      call.r4.u64 = g_view.frame[i];
      call.r5.u64 = arg;
      call.r6.u64 = 0;
      call.r7.u64 = 0;
      call.r8.s64 = -1;
      set(call, base);
    }
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

// sub_821CF7B0 bakes a glyph cream (0xFFF2B8) when its text's style byte is
// 1, as every menu text's is, and white otherwise. The PS3 draws the picture
// view's text white. Text records hang off dword_82557568, linked at +932.
void DrawWhite(PPCContext& ctx, std::uint8_t* base, std::uint32_t id) {
  constexpr std::uint32_t kTextList = 0x82557568u;
  constexpr std::uint32_t kNextOffset = 932u;
  constexpr std::uint32_t kGlyphStyleOffset = 926u;
  const std::uint32_t handle = Resolve(ctx, base, id);
  for (std::uint32_t text = REX_LOAD_U32(kTextList); text && handle;
       text = REX_LOAD_U32(text + kNextOffset)) {
    if (REX_LOAD_U32(text) == handle) {
      REX_STORE_U8(text + kGlyphStyleOffset, 0);
      return;
    }
  }
}

float TextWidth(PPCContext& ctx, std::uint8_t* base, std::uint32_t id) {
  const std::uint32_t text = Resolve(ctx, base, id);
  if (!text)
    return 0.0f;
  PPCContext call = ctx;
  call.r3.u64 = kTextManager;
  call.r4.u64 = text;
  call.r5.u64 = 1;
  call.r6.u64 = 1;
  sub_821D4598(call, base);
  return static_cast<float>(call.r3.s32);
}

// The PS3 wraps its text to the column; ours is wrapped by measuring it a
// word at a time in text `id`.
std::vector<std::string> WrapLine(PPCContext& ctx, std::uint8_t* base, std::uint32_t id,
                                  std::uint32_t buffer, const std::string& text, float width) {
  std::vector<std::string> lines;
  std::string line;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t space = text.find(' ', at);
    const std::string word = text.substr(at, space == std::string::npos ? space : space - at);
    at = space == std::string::npos ? text.size() : space + 1;
    if (word.empty())
      continue;
    const std::string longer = line.empty() ? word : line + ' ' + word;
    SetText(ctx, base, id, buffer, longer);
    if (!line.empty() && TextWidth(ctx, base, id) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = longer;
    }
  }
  lines.push_back(line);
  return lines;
}

void SetWrappedText(PPCContext& ctx, std::uint8_t* base, std::uint32_t id, std::uint32_t buffer,
                    const std::string& text, float width) {
  std::string wrapped;
  for (const std::string& line : WrapLine(ctx, base, id, buffer, text, width))
    wrapped += (wrapped.empty() ? "" : "\n") + line;
  SetText(ctx, base, id, buffer, wrapped);
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t at = 0;
  while (true) {
    const std::size_t real = text.find('\n', at);
    const std::size_t escaped = text.find("\\n", at);
    const std::size_t end = std::min(real, escaped);
    lines.push_back(text.substr(at, end == std::string::npos ? end : end - at));
    if (end == std::string::npos)
      return lines;
    at = end + (end == real ? 1 : 2);
  }
}

// Only the lines the caption has get a rule, as sub_1F0C80 hides the rest.
// The French captions come as one long line plus the signature, so every line
// is wrapped to the column.
void SetCaption(PPCContext& ctx, std::uint8_t* base, const std::string& caption) {
  for (std::uint32_t i = 0; i < kCaptionLines; ++i) {
    if (!g_caption_text[i] && !(g_caption_text[i] = Alloc(400)))
      return;
  }
  std::vector<std::string> lines;
  for (const std::string& line : SplitLines(caption)) {
    for (std::string& wrapped : WrapLine(ctx, base, g_view.caption[0], g_caption_text[0], line,
                                         static_cast<float>(kColumnW)))
      lines.push_back(std::move(wrapped));
  }
  const std::uint32_t rules = Language(base) >= 3 && Language(base) <= 6
                                  ? kLongCaptions.rules
                                  : kShortCaptions.rules;
  for (std::uint32_t i = 0; i < kCaptionLines; ++i) {
    SetText(ctx, base, g_view.caption[i], g_caption_text[i], i < lines.size() ? lines[i] : "");
    const bool ruled = i < rules && i < lines.size();
    Tint(ctx, base, g_view.rules[i], ruled ? 0xFFFFFFFFu : 0u);
  }
}

void ShowConfirmPrompt(PPCContext& ctx, std::uint8_t* base, bool shown) {
  const float dx = shown ? -kAway : kAway;
  Shift(ctx, base, g_view.prompt_icon, dx, 0.0f);
  Shift(ctx, base, g_view.prompt_text, dx, 0.0f);
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

// The menu cursor (ui + 1596) draws its sprite from the object at +48. The
// PS3 hides it while a picture is up; tinted per frame, as the cursor's own
// animation may tint it back.
void ShowCursor(PPCContext& ctx, std::uint8_t* base, bool shown) {
  constexpr std::uint32_t kCursorOffset = 1596u;
  constexpr std::uint32_t kCursorSpriteOffset = 48u;
  const std::uint32_t ui = REX_LOAD_U32(kUiRootAddr);
  const std::uint32_t cursor = ui ? REX_LOAD_U32(ui + kCursorOffset) : 0;
  const std::uint32_t sprite = cursor ? REX_LOAD_U32(cursor + kCursorSpriteOffset) : 0xFFFFFFFFu;
  if (sprite != 0xFFFFFFFFu)
    Tint(ctx, base, sprite, shown ? 0xFFFFFFFFu : 0u);
}

// While a picture is up the cursor sits in a group of its own with one
// selectable, as sub_210A50 does on the PS3, so the cells behind cannot be
// reached. Confirm still answers with its tag, which closes the picture. The
// selectable takes the opened cell's place so the hidden cursor stays put.
void IsolateCursor(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu, int picture) {
  PPCContext call = ctx;
  call.r3.u64 = menu;
  sub_821FFC48(call, base);
  const std::uint32_t group = call.r3.u32;
  if (group == 0xFFFFFFFFu)
    return;
  call = ctx;
  call.r3.u64 = menu;
  call.r4.u64 = group;
  const auto column = static_cast<std::int32_t>(picture) % static_cast<std::int32_t>(kGridColumns);
  const auto row = static_cast<std::int32_t>(picture) / static_cast<std::int32_t>(kGridColumns);
  call.r5.s64 = kCellX + kCellPitchX * column + kSelectableDx;
  call.r6.s64 = kCellY + kCellPitchY * row + kSelectableDy;
  call.r7.u64 = 1;
  sub_821FFEF8(call, base);
  ShowCursor(ctx, base, false);
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
  ShowCursor(ctx, base, true);
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
  if (!g_title_text && !(g_title_text = Alloc(400)))
    return;
  for (std::uint32_t i = 0; i < kPictures; ++i)
    Tint(ctx, base, g_view.pictures[i], static_cast<int>(i) == picture ? 0xFFFFFFFFu : 0u);
  const auto n = static_cast<std::uint32_t>(picture);
  SetWrappedText(ctx, base, g_view.title, g_title_text, Text(base, n),
                 static_cast<float>(kColumnW));
  SetCaption(ctx, base, Text(base, kTextCaption + n));
  ShowConfirmPrompt(ctx, base, false);
  SetGridOffset(ctx, base, kAway);
  Shift(ctx, base, g_view.panel, 0.0f, static_cast<float>(kPanelY - kPanelHiddenY));
  PlayClip(ctx, base, picture);
  IsolateCursor(ctx, base, menu, picture);
  g_open = picture;
}

void ClosePicture(PPCContext& ctx, std::uint8_t* base, std::uint32_t menu) {
  if (g_open < 0)
    return;
  RestoreCursor(ctx, base, menu);
  Shift(ctx, base, g_view.panel, 0.0f, static_cast<float>(kPanelHiddenY - kPanelY));
  ShowConfirmPrompt(ctx, base, true);
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
    // sub_211108's title size per language. Its raised offsets for the
    // smaller sizes suit the PS3's font; ours stays centred in the box.
    const std::uint32_t language = Language(base);
    const std::uint32_t size = language == 4 ? 38u : language == 3 || language == 6 ? 40u : 48u;
    words[title + 3] = 11u + (48u - size) / 2u;
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

void AppendPicturePanel(const std::uint8_t* base, std::vector<std::uint32_t>& words) {
  const std::uint32_t pictures = eternalsonata::kPs3ScrapbookSlot + 1u;
  words.insert(words.end(), {kTypeFramedPanel, kPanelX,
                             static_cast<std::uint32_t>(kPanelHiddenY), kPanelW, kPanelH,
                             kGoldFrame, kOpenChildren, g_stock_panels + kPictures});
  // Of two icons the later one draws below, so the pictures come before the
  // frame whose middle they cover.
  for (std::uint32_t i = 0; i < kPictures; ++i)
    words.insert(words.end(), {kTypeIcon, pictures + i, kPictureX, kPictureY, 1000, 1000,
                               0xFFFFFFFFu});
  for (const FramePiece& piece : kFrame)
    words.insert(words.end(), {kTypeIcon, kFrameImage, piece.x, piece.y, 1000, 1000, 0xFFFFFFFFu});
  const bool italian = Language(base) == kItalian;
  words.insert(words.end(), {kTypeText, 0, kColumnX, kTitleY, kColumnW, 96, 0,
                             italian ? kItalianTitleSize : kTitleSize, 0xFFFFFFFFu, 1});
  const CaptionLayout& layout = Language(base) >= 3 && Language(base) <= 6 ? kLongCaptions
                                                                          : kShortCaptions;
  const std::uint32_t caption_size = italian ? kItalianCaptionSize : layout.size;
  for (std::uint32_t i = 0; i < kCaptionLines; ++i)
    words.insert(words.end(), {kTypeText, 0, kColumnX, kCaptionY + layout.step * i, kColumnW,
                               layout.step, 0, caption_size, 0xFFFFFFFFu, 1});
  for (std::uint32_t i = 0; i < kCaptionLines; ++i)
    words.insert(words.end(), {kTypeRule, kColumnX, layout.first_rule + layout.step * i, kColumnW});
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
  if (!g_view.laid_out) {
    CentreCellTitles(ctx, base);
    ShapeFrame(ctx, base);
    DrawWhite(ctx, base, g_view.title);
    for (const std::uint32_t line : g_view.caption)
      DrawWhite(ctx, base, line);
    g_view.laid_out = true;
  }
  if (g_open < 0)
    SetGridOffset(ctx, base, GridOffsetFor(base));
  else
    ShowCursor(ctx, base, false);
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
  if (sid == kSidPerform) {
    // Asked for by the label's own record, right after its glyph's.
    if (g_pending_build) {
      const std::uint32_t ui = REX_LOAD_U32(kUiRootAddr);
      const std::uint32_t screen = ui ? ScreenOf(base, ui) : 0;
      if (screen && REX_LOAD_U8(screen + kIconCount)) {
        g_prompt_icon_index = REX_LOAD_U8(screen + kIconCount) - 1u;
        g_prompt_text_index = REX_LOAD_U8(screen + kTextCount);
      }
    }
    return GuestString(base, kTextOpen, Text(base, kTextOpen));
  }
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
  AppendPicturePanel(base, words);

  if (!g_list_copy && !(g_list_copy = Alloc(4u * kListCapacity)))
    return 0;
  if (words.size() + 1 > kListCapacity)
    return 0;
  for (std::size_t i = 0; i < words.size(); ++i)
    REX_STORE_U32(g_list_copy + 4u * static_cast<std::uint32_t>(i), words[i]);
  REX_STORE_U32(g_list_copy + 4u * static_cast<std::uint32_t>(words.size()), kListEnd);
  g_prompt_icon_index = g_prompt_text_index = 0xFFFFFFFFu;
  g_pending_build = true;
  return g_list_copy;
}

// The appended records are the last eleven panels (ten cells and the picture
// panel), the last nineteen icons (the pictures, then the frame), the last
// texts and the last seven rules.
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
  const std::uint32_t rules = REX_LOAD_U8(screen + kRuleCount);
  const std::uint32_t picture_panel = g_stock_panels + kPictures;
  const std::uint32_t frame_icons = static_cast<std::uint32_t>(kFrame.size());
  // Ours end the lists: one text per cell, then the title and caption lines.
  const std::uint32_t view_texts = 1 + kCaptionLines;
  if (panels != picture_panel + 1 || icons < kPictures + frame_icons ||
      texts < kPictures + view_texts || rules < kCaptionLines ||
      g_prompt_icon_index >= icons || g_prompt_text_index >= texts) {
    REXLOG_WARN("[scrapbook] grid not built: {} panels, {} icons, {} texts, {} rules", panels,
                icons, texts, rules);
    return;
  }
  const auto icon = [&](std::uint32_t i) { return REX_LOAD_U32(screen + kIconBase + 4u * i); };
  const auto text = [&](std::uint32_t i) { return REX_LOAD_U32(screen + kTextBase + 4u * i); };
  g_view.screen = screen;
  for (std::uint32_t i = 0; i < kPictures; ++i) {
    g_view.cells[i] = REX_LOAD_U32(screen + 4u * (g_stock_panels + i));
    g_view.cell_texts[i] = text(texts - view_texts - kPictures + i);
    g_view.pictures[i] = icon(icons - frame_icons - kPictures + i);
  }
  for (std::uint32_t i = 0; i < frame_icons; ++i)
    g_view.frame[i] = icon(icons - frame_icons + i);
  g_view.panel = REX_LOAD_U32(screen + 4u * picture_panel);
  g_view.title = text(texts - view_texts);
  for (std::uint32_t i = 0; i < kCaptionLines; ++i) {
    g_view.caption[i] = text(texts - kCaptionLines + i);
    g_view.rules[i] = REX_LOAD_U32(screen + kRuleBase + 4u * (rules - kCaptionLines + i));
  }
  g_view.prompt_icon = icon(g_prompt_icon_index);
  g_view.prompt_text = text(g_prompt_text_index);
  g_view.grid_offset = kGridSlide;
  const auto built = [](const auto& ids) {
    return std::find(ids.begin(), ids.end(), 0xFFFFFFFFu) == ids.end();
  };
  g_view.ready = built(g_view.cells) && built(g_view.cell_texts) && built(g_view.frame) &&
                 built(g_view.pictures) && built(g_view.caption) && built(g_view.rules);
}

}  // namespace scrapbook_menu
