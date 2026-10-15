// eternalsonata - the PS3's Scrapbook camp page (docs/ps3-scrapbook.md).
//
// Ten captioned pictures, PS3 data only. option_strip.cpp gives the entry
// Item Set's place in the strip with tag 13; this file opens the Music gallery
// for it, as achievements_menu.cpp does for tag 11, since the PS3 page has the
// same frame: a header, a list of titled rows and a panel at (65, 140).
//
// The list holds one tab of ten rows. Confirming a row plays the picture's
// clip through the gallery's own stream start and slides the list away for a
// panel appended to the display list: the ten pictures (campdata
// characterphoto.bmd, in AppKeep slots from kPs3ScrapbookSlot) with only the
// chosen one tinted visible, its title and its caption. Any button closes it.

#include "scrapbook_menu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
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

namespace scrapbook_menu {
namespace {

constexpr std::uint32_t kPictures = eternalsonata::kPs3ScrapbookCount;

// Ps3ScrapbookText ids past the pictures' titles and captions.
constexpr std::uint32_t kTextCaption = 10;
constexpr std::uint32_t kTextHeading = 21;
constexpr std::uint32_t kTextOpen = 22;
constexpr std::uint32_t kTabSlot = 23;  // GuestString slots for the three tabs

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

// The gallery's rows: the count it scrolls against, how far it is scrolled,
// and the state byte its tick switches on (3 takes input).
constexpr std::uint32_t kRowCountAddr = 0x82440128u;
constexpr std::uint32_t kScrollAddr = 0x8243F368u;
constexpr std::uint32_t kGalleryStateAddr = 0x8243F3C2u;
constexpr std::uint8_t kGalleryInput = 3;

// The menu's selection result (a row 1..4, -4 for cancel) and button word,
// and the cooldown the gallery arms after a choice.
constexpr std::uint32_t kResultOffset = 444u;
constexpr std::uint32_t kButtonsOffset = 448u;
constexpr std::uint32_t kCooldownFlagOffset = 425u;
constexpr std::uint32_t kCooldownOffset = 428u;
constexpr std::uint32_t kButtonTabLeft = 0x100u;
constexpr std::uint32_t kButtonTabRight = 0x200u;
constexpr std::uint32_t kButtonStop = 0x8000u;

// What sub_82226CC8 slides away on exit: the description and list panels,
// the five row containers and the three cursor glows.
constexpr std::uint32_t kListPanels[] = {4u, 8u};
constexpr std::uint32_t kRowContainers = 1672u;
constexpr std::uint32_t kRowContainerCount = 5u;
constexpr std::uint32_t kGlows[] = {104u, 108u, 112u};
constexpr float kAway = 1000.0f;

// The tab strip's second and third widgets.
constexpr std::uint32_t kHiddenTabs[] = {1548u, 1552u};

// Screen object layout: panel handles from +0 (count at +72), icon handles
// from +76 (count at +380), text handles from +384 (count at +640).
constexpr std::uint32_t kPanelCount = 72u;
constexpr std::uint32_t kIconBase = 76u;
constexpr std::uint32_t kIconCount = 380u;
constexpr std::uint32_t kTextBase = 384u;
constexpr std::uint32_t kTextCount = 640u;

// Text the gallery asks for while the page is up.
constexpr std::uint32_t kMenuBlob = 0x82031A00u;
constexpr std::uint32_t kTitleBlob = 0x82053E10u;
constexpr std::uint32_t kSidPerform = 158u;
constexpr std::uint32_t kSidTabFirst = 160u;

// The picture panel, the size the PS3 builds it at (sub_1F0C80), parked where
// the gallery parks its own list before it slides in.
constexpr std::int32_t kPanelX = 65;
constexpr std::int32_t kPanelY = 140;
constexpr std::int32_t kPanelHiddenY = 880;
constexpr std::uint32_t kPanelW = 1150;
constexpr std::uint32_t kPanelH = 543;
constexpr std::uint32_t kPanelIndex = 3;
constexpr std::int32_t kPictureX = 24;
constexpr std::int32_t kPictureY = 40;
constexpr std::int32_t kColumnX = kPictureX + 816 + 24;
constexpr std::uint32_t kColumnW = kPanelW - kColumnX - 24;

constexpr std::uint32_t kListEnd = 0x0000FFFFu;
constexpr std::uint32_t kMaxListWords = 0x400u;
constexpr std::uint32_t kOffscreen = static_cast<std::uint32_t>(-10000);

bool g_active = false;
thread_local int t_row = -1;

struct View {
  std::uint32_t screen = 0;
  std::uint32_t panel = 0;
  std::array<std::uint32_t, kPictures> pictures{};
  std::uint32_t title = 0;
  std::uint32_t caption = 0;
  bool ready = false;
};
View g_view;
bool g_pending_build = false;
int g_open = -1;

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

void Shift(PPCContext& ctx, std::uint8_t* base, std::uint32_t id, float dy) {
  if (!id || id == 0xFFFFFFFFu)
    return;
  PPCContext call = ctx;
  call.r3.u64 = g_scratch;
  call.r4.u64 = kObjects;
  call.r5.u64 = id;
  sub_8217BF28(call, base);
  const float x = std::bit_cast<float>(REX_LOAD_U32(g_scratch));
  const float y = std::bit_cast<float>(REX_LOAD_U32(g_scratch + 4));
  Place(ctx, base, id, x, y + dy);
}

// Moves what the exit animation moves, by `dy`, so the picture panel has the
// screen to itself.
void ShiftList(PPCContext& ctx, std::uint8_t* base, float dy) {
  const std::uint32_t screen = g_view.screen;
  for (const std::uint32_t at : kListPanels)
    Shift(ctx, base, REX_LOAD_U32(screen + at), dy);
  for (std::uint32_t i = 0; i < kRowContainerCount; ++i) {
    const std::uint32_t id = REX_LOAD_U32(screen + kRowContainers + 4u * i);
    if (id == 0xFFFFFFFFu)
      continue;
    if (const std::uint32_t row = Resolve(ctx, base, id))
      Shift(ctx, base, REX_LOAD_U32(row + 16u), dy);
  }
  for (const std::uint32_t at : kGlows)
    Shift(ctx, base, REX_LOAD_U32(screen + at), dy);
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

void OpenPicture(PPCContext& ctx, std::uint8_t* base, int picture) {
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
  ShiftList(ctx, base, kAway);
  Place(ctx, base, g_view.panel, kPanelX, kPanelY);
  PlayClip(ctx, base, picture);
  g_open = picture;
}

void ClosePicture(PPCContext& ctx, std::uint8_t* base) {
  if (g_open < 0)
    return;
  Place(ctx, base, g_view.panel, kPanelX, kPanelHiddenY);
  ShiftList(ctx, base, -kAway);
  g_open = -1;
}

bool IsMusicList(std::uint32_t list) { return achievements_menu::IsMusicScreenList(list); }

// The prompt pair `icon` then `label`: a seven word icon record directly
// followed by a ten word text record. Moved off screen, not removed, so every
// later record keeps its object index.
void HidePrompt(std::vector<std::uint32_t>& words, std::uint32_t icon, std::uint32_t label) {
  for (std::size_t i = 0; i + 17 <= words.size(); ++i) {
    if (words[i] == 100u && words[i + 1] == icon && words[i + 7] == 200u &&
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

std::uint32_t RowCount() { return kPictures; }

void FixRowCount(std::uint8_t* base) {
  rex::memory::store_and_swap<std::uint32_t>(base + kRowCountAddr, kPictures);
}

void BeginRow(int index) { t_row = index; }
void EndRow() { t_row = -1; }

void Tick(PPCContext& ctx, std::uint8_t* base) {
  for (const std::uint32_t tab : kHiddenTabs)
    achievements_menu::HideTabFrame(ctx, base, tab);
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
    // The list is away; the cursor may move behind the picture, nothing else.
    if (result != -1 || (buttons & kButtonStop)) {
      ClosePicture(ctx, base);
      PlaySound(ctx, base, kSoundCancel);
      Cooldown(base, menu, 15);
    }
    return;
  }
  if (result >= 1 && result <= 4 && REX_LOAD_U8(kGalleryStateAddr) == kGalleryInput) {
    const auto row = static_cast<int>(REX_LOAD_U32(kScrollAddr)) + result - 1;
    if (row >= 0 && row < static_cast<int>(kPictures)) {
      OpenPicture(ctx, base, row);
      Cooldown(base, menu, 30);
      return;
    }
  }
  // One tab, and nothing to stop.
  REX_STORE_U32(menu + kButtonsOffset, buttons & ~(kButtonTabLeft | kButtonTabRight | kButtonStop));
  __imp__sub_82226858(ctx, base);
  REX_STORE_U32(menu + kButtonsOffset, buttons);
}

std::uint32_t TextOverride(std::uint8_t* base, std::uint32_t blob, std::uint32_t sid) {
  if (!g_active)
    return 0;
  if (t_row >= 0 && blob == kTitleBlob)
    return GuestString(base, static_cast<std::uint32_t>(t_row), Text(base, static_cast<std::uint32_t>(t_row)));
  if (blob != kMenuBlob)
    return 0;
  if (sid == kSidPerform)
    return GuestString(base, kTextOpen, Text(base, kTextOpen));
  if (sid >= kSidTabFirst && sid < kSidTabFirst + 3)
    return GuestString(base, kTabSlot + (sid - kSidTabFirst),
                       sid == kSidTabFirst ? EntryLabel(base) : std::string());
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
  HideRecord(words, 100u, 240u);  // LB, RB and Change Tab
  HideRecord(words, 100u, 242u);
  HideRecord(words, 200u, 64u);
  HideRecord(words, 200u, 125u);  // the Japanese D-pad and its label
  HideRecord(words, 700u, 148u);

  const std::uint32_t pictures = eternalsonata::kPs3ScrapbookSlot + 1u;
  const std::initializer_list<std::uint32_t> panel = {
      1, kPanelX, static_cast<std::uint32_t>(kPanelHiddenY), kPanelW, kPanelH, 2100, kPanelIndex};
  words.insert(words.end(), panel);
  for (std::uint32_t i = 0; i < kPictures; ++i)
    words.insert(words.end(), {100, pictures + i, kPictureX, kPictureY, 1000, 1000,
                               0xFFFFFFFFu});
  words.insert(words.end(), {200, 0, kColumnX, kPictureY, kColumnW, 96, 0, 40, 0xFFFFFFFFu, 1});
  words.insert(words.end(), {200, 0, kColumnX, kPictureY + 110, kColumnW, 340, 0, 34,
                             0xFFFFFFFFu, 1});
  words.push_back(2101);

  const std::uint32_t needed = 4u * (static_cast<std::uint32_t>(words.size()) + 1u);
  if (!g_list_copy && !(g_list_copy = Alloc(4u * (kMaxListWords + 64u))))
    return 0;
  if (needed > 4u * (kMaxListWords + 64u))
    return 0;
  for (std::size_t i = 0; i < words.size(); ++i)
    REX_STORE_U32(g_list_copy + 4u * static_cast<std::uint32_t>(i), words[i]);
  REX_STORE_U32(g_list_copy + 4u * static_cast<std::uint32_t>(words.size()), kListEnd);
  g_pending_build = true;
  return g_list_copy;
}

// The appended records are the last panel, the last ten icons and the last two
// texts the walk made.
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
  if (panels <= kPanelIndex || icons < kPictures || texts < 2) {
    REXLOG_WARN("[scrapbook] picture panel not built: {} panels, {} icons, {} texts", panels,
                icons, texts);
    return;
  }
  g_view.screen = screen;
  g_view.panel = REX_LOAD_U32(screen + 4u * (panels - 1));
  for (std::uint32_t i = 0; i < kPictures; ++i)
    g_view.pictures[i] = REX_LOAD_U32(screen + kIconBase + 4u * (icons - kPictures + i));
  g_view.title = REX_LOAD_U32(screen + kTextBase + 4u * (texts - 2));
  g_view.caption = REX_LOAD_U32(screen + kTextBase + 4u * (texts - 1));
  g_view.ready = std::find(g_view.pictures.begin(), g_view.pictures.end(), 0xFFFFFFFFu) ==
                 g_view.pictures.end();
}

}  // namespace scrapbook_menu
