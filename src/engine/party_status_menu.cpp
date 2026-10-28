// eternalsonata: Characters 11 and 12 on the status page, and X changing
// the shown character's costume (docs/costumes.md).

#include "party_status_menu.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <iterator>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "character_roster.h"
#include "costume_system.h"
#include "eternalsonata_costume_api.h"
#include "eternalsonata_asset_container.h"
#include "generated/eternalsonata_init.h"
#include "party_system.h"
#include "ps3_appkeep.h"
#include "target.h"

// AppKeep image ids are the slot plus one.
extern "C++" void PartyStatusPortrait(PPCRegister &image,
                                      PPCRegister &character) {
  if (const uint32_t portrait = eternalsonata::CostumePortrait(
          static_cast<int>(character.u32), ETERNALSONATA_COSTUME_PORTRAIT_STATUS)) {
    image.u64 = portrait;
  } else if (character.u32 > 10 && character.u32 <= 12 && eternalsonata::IsPs3Target()) {
    image.u64 = eternalsonata::kPs3MenuPortraitSlot + character.u32 - 10;
  } else if (const int base = eternalsonata::IsModdedCharacter(static_cast<int>(character.u32))
                                  ? eternalsonata::CharacterBase(static_cast<int>(character.u32))
                                  : 0) {
    // word_8202C894[c - 1], the status art; past ten it reads the magic order.
    auto* memory = rex::Runtime::instance()->memory();
    image.u64 = rex::memory::load_and_swap<uint16_t>(
        memory->TranslateVirtual<uint8_t*>(0x8202C894u + 2u * static_cast<uint32_t>(base - 1)));
  }
}

extern "C++" void PartyStatusNameSid(PPCRegister &sid) {
  if (sid.u32 > 10 && sid.u32 <= 12)
    sid.u64 = eternalsonata::PartyNameSid(static_cast<int>(sid.u32), false);
}

extern "C++" void PartyStatusSelection(PPCRegister &manager) {
  auto *runtime = rex::Runtime::instance();
  auto *memory = runtime ? runtime->memory() : nullptr;
  if (!memory)
    return;
  const auto load_u32 = [&](uint32_t address) {
    return rex::memory::load_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t *>(address));
  };
  const auto load_u8 = [&](uint32_t address) {
    return *memory->TranslateVirtual<uint8_t *>(address);
  };

  const uint32_t character = load_u32(0x8243F360u);
  if (character <= 10 || character > 12)
    return;

  for (uint32_t control = load_u32(manager.u32 + 392); control;
       control = load_u32(control + 48)) {
    if (load_u32(control) != 0)
      continue;
    const uint8_t selected = load_u8(control + 44);
    const uint8_t count = load_u8(control + 12);
    if (selected >= count || selected > 5)
      *memory->TranslateVirtual<uint8_t *>(control + 44) = 0;
    return;
  }
}

namespace party_status_menu {
namespace {

// sub_821EC050 hands screen 2's shipped list to the interpreter uncopied, one
// per language (JPN USA GBR FRA ITA DEU ESP), so it is edited in a copy.
constexpr std::uint32_t kStatusLists[] = {
    0x82058268u, 0x8202DC78u, 0x82062058u, 0x82066F28u,
    0x8205D160u, 0x82070CD8u, 0x8206BE00u};
constexpr std::uint32_t kListEnd = 0x0000FFFFu;
constexpr std::uint32_t kRecordEnd = 0xFFFFFFFFu;
constexpr std::uint32_t kMaxListWords = 0x800u;
constexpr std::uint32_t kTypeGlyph = 100u;  // {100, image, x, y, sx, sy, -1}
constexpr std::uint32_t kTypeText = 200u;   // {200, sid, x, y, w, h, ...}

// AppKeep images are the slot plus one: 237 is B, 238 X, 240 and 242 LB / RB.
constexpr std::uint32_t kGlyphB = 237u;
constexpr std::uint32_t kGlyphX = 238u;
constexpr std::uint32_t kGlyphLb = 240u;
constexpr std::uint32_t kGlyphRb = 242u;
constexpr std::uint32_t kSwitchCharacterSid = 87u;
constexpr std::uint32_t kHidden = static_cast<std::uint32_t>(-10000);
constexpr std::uint32_t kPromptShift = 8u;

// Free below the party name ids. Answered only for the executable's blocks.
constexpr std::uint32_t kCostumeSid = 850u;
constexpr std::uint32_t kCostumeNameSid = 851u;

// {1, x, y, w, h, 2100, n} makes panel n the parent until 2101. n is the
// index the walk gives the new panel; the status list builds 13 of its own.
constexpr std::uint32_t kTypePanel = 1u;
constexpr std::uint32_t kNamePanel = 13u;
constexpr std::uint32_t kOpenPanel = 2100u;
constexpr std::uint32_t kClosePanel = 2101u;
constexpr std::uint32_t kNameWidth = 320u;
constexpr std::uint32_t kNamePad = 24u;
constexpr std::uint32_t kNameTop = 6u;
constexpr std::uint32_t kNameHeight = 48u;
constexpr std::uint32_t kNameSize = 36u;
constexpr std::uint32_t kNameParkedY = 2000u;
constexpr std::uint32_t kNameBytes = 128u;

constexpr std::uint32_t kLanguageIndexAddress = 0x8243D370u;

// Western ones in Latin-1, as the text records are single byte; Japanese is
// converted to Shift-JIS on use.
constexpr const char* kCostumeLabel[7] = {
    "コスチューム", "Costume", "Costume", "Costume",
    "Costume",      "Kost\xFCm", "Traje"};

std::uint32_t g_list_copy = 0;
std::uint32_t g_label = 0;
std::uint32_t g_name_text = 0;
bool g_prompt_built = false;
int g_prompt_shown = -1;

std::uint32_t ReadU32(const std::uint8_t* base, std::uint32_t address) {
  return rex::memory::load_and_swap<std::uint32_t>(base + address);
}

void WriteU32(std::uint8_t* base, std::uint32_t address, std::uint32_t value) {
  rex::memory::store_and_swap<std::uint32_t>(base + address, value);
}

// The guest copy of the shown costume name, empty until one is set.
std::uint32_t CostumeNameBuffer(std::uint8_t* base) {
  if (!g_name_text) {
    auto* mem = rex::system::kernel_memory();
    g_name_text = mem ? mem->SystemHeapAlloc(kNameBytes, 0x20) : 0;
    if (g_name_text)
      REX_STORE_U8(g_name_text, 0);
  }
  return g_name_text;
}

// Labels are UTF-8; the font reads Shift-JIS in Japanese and Latin-1 otherwise.
std::string CostumeNameText(std::uint8_t* base, const char* label) {
  const std::string utf8 = label ? label : "";
  if (ReadU32(base, kLanguageIndexAddress) == 0u) {
    std::string sjis;
    return eternalsonata::assets::EncodeShiftJis(utf8, sjis) ? sjis : utf8;
  }
  std::string latin;
  for (std::size_t i = 0; i < utf8.size(); ++i) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) {
      latin.push_back(static_cast<char>(c));
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      const unsigned cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[++i]) & 0x3Fu);
      latin.push_back(cp < 0x100 ? static_cast<char>(cp) : '?');
    } else {
      while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80)
        ++i;
      latin.push_back('?');
    }
  }
  return latin;
}

// Writes the name the label shows; empty hides it.
void SetCostumeName(std::uint8_t* base, const std::string& text) {
  const std::uint32_t buffer = CostumeNameBuffer(base);
  if (!buffer)
    return;
  const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(text.size(), kNameBytes - 1));
  for (std::uint32_t i = 0; i < n; ++i)
    REX_STORE_U8(buffer + i, static_cast<std::uint8_t>(text[i]));
  REX_STORE_U8(buffer + n, 0);
}

}  // namespace

// The LB / RB Switch Character prompt becomes X Costume. Its glyph and label
// take the row's own placement, read from the Back prompt, and the second
// shoulder glyph is parked off screen so object indices stay put.
std::uint32_t MaybeSwapStatusList(std::uint8_t* base, std::uint32_t list) {
  if (std::find(std::begin(kStatusLists), std::end(kStatusLists), list) ==
      std::end(kStatusLists)) {
    return 0;
  }
  g_prompt_built = false;
  g_prompt_shown = -1;
  if (const std::uint32_t name = CostumeNameBuffer(base))
    REX_STORE_U8(name, 0);
  std::vector<std::uint32_t> words;
  for (std::uint32_t i = 0; i < kMaxListWords; ++i) {
    const std::uint32_t word = ReadU32(base, list + 4u * i);
    if (word == kListEnd)
      break;
    words.push_back(word);
  }
  if (words.size() >= kMaxListWords)
    return 0;

  std::size_t prompt = 0, back = 0;
  for (std::size_t i = 0; i + 23 <= words.size(); ++i) {
    if (!prompt && words[i] == kTypeGlyph && words[i + 1] == kGlyphLb &&
        words[i + 6] == kRecordEnd && words[i + 7] == kTypeGlyph &&
        words[i + 8] == kGlyphRb && words[i + 13] == kRecordEnd &&
        words[i + 14] == kTypeText && words[i + 15] == kSwitchCharacterSid)
      prompt = i + 1;
    if (!back && words[i] == kTypeGlyph && words[i + 1] == kGlyphB &&
        words[i + 6] == kRecordEnd && words[i + 7] == kTypeText)
      back = i + 1;
  }
  if (!prompt || !back)
    return 0;
  const std::size_t p = prompt - 1, b = back - 1;

  const std::uint32_t label_dx = words[b + 9] - words[b + 2];
  words[p + 1] = kGlyphX;
  words[p + 2] -= kPromptShift;
  words[p + 3] = words[b + 3];
  words[p + 4] = words[b + 4];
  words[p + 5] = words[b + 5];
  words[p + 9] = kHidden;
  words[p + 15] = kCostumeSid;
  words[p + 16] = words[p + 2] + label_dx;
  words[p + 17] = words[b + 10];

  // The costume name: a panel of its own, parked until a change places it
  // under the portrait.
  words.insert(words.end(), {kTypePanel, 0, kNameParkedY, kNameWidth, kNameHeight, kOpenPanel,
                             kNamePanel});
  words.insert(words.end(), {kTypeText, kCostumeNameSid, kNamePad, kNameTop,
                             kNameWidth - 2u * kNamePad, kNameHeight, 0, kNameSize, 0xFFFFFFFFu,
                             1});
  words.push_back(kClosePanel);

  constexpr std::uint32_t kCopyWords = kMaxListWords + 32u;
  if (words.size() + 1u > kCopyWords)
    return 0;
  if (!g_list_copy) {
    auto* mem = rex::system::kernel_memory();
    g_list_copy = mem ? mem->SystemHeapAlloc(4u * kCopyWords, 0x20) : 0;
    if (!g_list_copy)
      return 0;
  }
  for (std::size_t i = 0; i < words.size(); ++i)
    WriteU32(base, g_list_copy + 4u * static_cast<std::uint32_t>(i), words[i]);
  WriteU32(base, g_list_copy + 4u * static_cast<std::uint32_t>(words.size()),
           kListEnd);
  g_prompt_built = true;
  return g_list_copy;
}

std::uint32_t CostumePromptText(std::uint8_t* base, std::uint32_t blob,
                                std::uint32_t sid) {
  if (blob < 0x82000000u || blob >= 0x82600000u)
    return 0;
  if (sid == kCostumeNameSid)
    return CostumeNameBuffer(base);
  if (sid != kCostumeSid)
    return 0;
  if (!g_label) {
    auto* mem = rex::system::kernel_memory();
    g_label = mem ? mem->SystemHeapAlloc(64, 0x20) : 0;
    if (!g_label)
      return 0;
  }
  const std::uint32_t language = ReadU32(base, kLanguageIndexAddress);
  std::string text = kCostumeLabel[language < 7u ? language : 1u];
  if (language == 0u) {
    std::string sjis;
    if (eternalsonata::assets::EncodeShiftJis(text, sjis))
      text = sjis;
  }
  for (std::uint32_t i = 0; i <= text.size() && i < 63u; ++i)
    REX_STORE_U8(g_label + i, i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
  REX_STORE_U8(g_label + 63u, 0);
  return g_label;
}

}  // namespace party_status_menu

// sub_82232C10(character): redraws the page for one character.
// sub_821425D8(sound manager, cue, 0, 0): a menu sound.
// sub_82179160(objects, handle, argb, 0, 0): tints a sprite.
// sub_821F6580(ui, slot): the text widget behind a screen slot.
// sub_821D3890(texts, widget, string, -1): sets a widget's text.
REX_EXTERN(sub_82232C10);
REX_EXTERN(sub_821425D8);
REX_EXTERN(sub_82179160);
REX_EXTERN(sub_821F6580);
REX_EXTERN(sub_821D3890);
// sub_821D4598(texts, widget, 1, 1): a widget's text width.
// sub_821D3D50(texts, widget, f1): its text scale.
// sub_8217BF28(&out, objects, handle) / sub_82178A88(objects, handle, &xyz):
// reads / sets an object's position.
// sub_821E8F58(dword_824400DC, image): an AppKeep image's texture, whose size
// sub_8217CF58 / sub_8217CFA8(objects, texture) answer.
REX_EXTERN(sub_821D4598);
REX_EXTERN(sub_821D3D50);
REX_EXTERN(sub_8217BF28);
REX_EXTERN(sub_82178A88);
REX_EXTERN(sub_821E8F58);
REX_EXTERN(sub_8217CF58);
REX_EXTERN(sub_8217CFA8);
REX_EXTERN(__imp__sub_82230658);
REX_EXTERN(__imp__sub_8222FFE8);

namespace {

constexpr uint32_t kMenuState = 0x824400E8u;
constexpr uint32_t kUiRoot = 0x824400E4u;
constexpr uint32_t kObjectManager = 0x824CF500u;
constexpr uint32_t kTextManager = 0x82555690u;
constexpr uint32_t kEmptyString = 0x82014040u;
constexpr uint32_t kXexTextBlock = 0x82031A00u;
constexpr uint32_t kShownCharacter = 0x8243F360u;
constexpr uint32_t kSoundManager = 0x8243D89Cu;
constexpr uint32_t kButtonX = 0x4000u;
constexpr uint32_t kCursorSound = 7u;

// Screen slots of the prompt: glyph 1 (glyph 0 is the list's first record) and
// text 0.
constexpr uint32_t kPromptGlyphSlot = 80u;
constexpr uint32_t kPromptTextSlot = 384u;

bool CanChangeCostume(int character) {
  const int next = eternalsonata::NextUnlockedCostume(character);
  return next >= 0 && next != eternalsonata::WornCostume(character);
}

// Shows the prompt only when X would do something for the shown character.
void RefreshCostumePrompt(PPCContext& ctx, uint8_t* base) {
  using namespace party_status_menu;
  if (!g_prompt_built)
    return;
  const uint32_t ui = REX_LOAD_U32(kUiRoot);
  const uint32_t screen = ui ? REX_LOAD_U32(ui + 2836u) : 0u;
  if (!screen)
    return;
  const int shown =
      CanChangeCostume(static_cast<int>(REX_LOAD_U32(kShownCharacter))) ? 1 : 0;
  if (shown == g_prompt_shown)
    return;
  g_prompt_shown = shown;

  PPCContext call = ctx;
  call.r3.u64 = kObjectManager;
  call.r4.u64 = REX_LOAD_U32(screen + kPromptGlyphSlot);
  call.r5.u64 = shown ? 0xFFFFFFFFu : 0u;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82179160(call, base);

  call = ctx;
  call.r3.u64 = ui;
  call.r4.u64 = REX_LOAD_U32(screen + kPromptTextSlot);
  sub_821F6580(call, base);
  const uint32_t widget = call.r3.u32;
  if (!widget)
    return;
  const uint32_t label = CostumePromptText(base, kXexTextBlock, kCostumeSid);
  call = ctx;
  call.r3.u64 = kTextManager;
  call.r4.u64 = widget;
  call.r5.u64 = shown && label ? label : kEmptyString;
  call.r6.s64 = -1;
  sub_821D3890(call, base);
}

// The costume name under the portrait: how long it stays, and the gap below
// the art. Screen object layout as scrapbook_menu.cpp reads it.
constexpr auto kNameShownFor = std::chrono::seconds(3);
constexpr float kNameGap = 4.0f;
constexpr float kNameLowestY = 600.0f;
constexpr float kNameParked = 2000.0f;
constexpr float kPortraitScale = 0.92f;  // sub_82232C10, all but Japanese
constexpr uint32_t kPortraitIcon = 124u;
constexpr uint32_t kPanelCount = 72u;
constexpr uint32_t kTextBase = 384u;
constexpr uint32_t kTextCount = 640u;
constexpr uint32_t kTextures = 0x824400DCu;
constexpr uint32_t kStatusArt = 0x8202C894u;
constexpr uint32_t kLanguage = 0x8243D370u;

std::chrono::steady_clock::time_point g_name_until{};
int g_name_character = 0;
uint32_t g_scratch = 0;

struct NameLabel {
  uint32_t panel = 0;
  uint32_t widget = 0;
};

// The appended panel and its text are the last ones the list walk made.
NameLabel FindNameLabel(PPCContext& ctx, uint8_t* base) {
  using namespace party_status_menu;
  const uint32_t ui = REX_LOAD_U32(kUiRoot);
  const uint32_t screen = ui ? REX_LOAD_U32(ui + 2836u) : 0u;
  if (!g_prompt_built || !screen)
    return {};
  const uint32_t panels = REX_LOAD_U8(screen + kPanelCount);
  const uint32_t texts = REX_LOAD_U8(screen + kTextCount);
  if (panels != kNamePanel + 1u || !texts)
    return {};
  PPCContext call = ctx;
  call.r3.u64 = ui;
  call.r4.u64 = REX_LOAD_U32(screen + kTextBase + 4u * (texts - 1u));
  sub_821F6580(call, base);
  return {REX_LOAD_U32(screen + 4u * (panels - 1u)), call.r3.u32};
}

void PlaceObject(PPCContext& ctx, uint8_t* base, uint32_t handle, float x, float y) {
  REX_STORE_U32(g_scratch + 16, std::bit_cast<uint32_t>(x));
  REX_STORE_U32(g_scratch + 20, std::bit_cast<uint32_t>(y));
  REX_STORE_U32(g_scratch + 24, 0);
  PPCContext call = ctx;
  call.r3.u64 = kObjectManager;
  call.r4.u64 = handle;
  call.r5.u64 = g_scratch + 16;
  call.r6.u64 = 0;
  call.r7.u64 = 0;
  call.r8.s64 = -1;
  sub_82178A88(call, base);
}

void SetNameText(PPCContext& ctx, uint8_t* base, uint32_t widget, uint32_t text) {
  PPCContext call = ctx;
  call.r3.u64 = kTextManager;
  call.r4.u64 = widget;
  call.r5.u64 = text;
  call.r6.s64 = -1;
  sub_821D3890(call, base);
}

void HideCostumeName(PPCContext& ctx, uint8_t* base) {
  g_name_character = 0;
  const NameLabel label = FindNameLabel(ctx, base);
  if (!label.widget || !g_scratch)
    return;
  party_status_menu::SetCostumeName(base, {});
  SetNameText(ctx, base, label.widget, kEmptyString);
  PlaceObject(ctx, base, label.panel, 0.0f, kNameParked);
}

// The portrait's art size, from the image sub_82232C10 gives it.
bool PortraitSize(PPCContext& ctx, uint8_t* base, int character, float& w, float& h) {
  PPCRegister image{}, who{};
  who.u64 = static_cast<uint32_t>(character);
  if (character >= 1 && character <= 10)
    image.u64 = REX_LOAD_U16(kStatusArt + 2u * static_cast<uint32_t>(character - 1));
  PartyStatusPortrait(image, who);
  if (!image.u32)
    return false;
  PPCContext call = ctx;
  call.r3.u64 = REX_LOAD_U32(kTextures);
  call.r4.u64 = image.u32;
  sub_821E8F58(call, base);
  const uint32_t texture = call.r3.u32;
  call = ctx;
  call.r3.u64 = kObjectManager;
  call.r4.u64 = texture;
  sub_8217CF58(call, base);
  w = static_cast<float>(call.r3.s32);
  call = ctx;
  call.r3.u64 = kObjectManager;
  call.r4.u64 = texture;
  sub_8217CFA8(call, base);
  h = static_cast<float>(call.r3.s32);
  return w > 0.0f && h > 0.0f;
}

// Centres the worn costume's name under the portrait for a few seconds.
void ShowCostumeName(PPCContext& ctx, uint8_t* base, int character) {
  using namespace party_status_menu;
  if (!g_scratch) {
    auto* mem = rex::system::kernel_memory();
    g_scratch = mem ? mem->SystemHeapAlloc(32, 0x10) : 0;
  }
  const NameLabel label = FindNameLabel(ctx, base);
  const uint32_t ui = REX_LOAD_U32(kUiRoot);
  const uint32_t screen = ui ? REX_LOAD_U32(ui + 2836u) : 0u;
  float art_w = 0.0f, art_h = 0.0f;
  if (!label.widget || !g_scratch || !screen ||
      !PortraitSize(ctx, base, character, art_w, art_h))
    return;

  SetCostumeName(base, CostumeNameText(base, eternalsonata::CostumeLabel(
                                                 character, eternalsonata::WornCostume(character))));
  SetNameText(ctx, base, label.widget, CostumeNameBuffer(base));
  PPCContext call = ctx;
  call.r3.u64 = kTextManager;
  call.r4.u64 = label.widget;
  call.r5.u64 = 1;
  call.r6.u64 = 1;
  sub_821D4598(call, base);
  const float text_w = static_cast<float>(call.r3.s32);
  const float max_w = static_cast<float>(kNameWidth - 2u * kNamePad);
  const float fit = text_w > max_w ? max_w / text_w : 1.0f;
  call = ctx;
  call.r3.u64 = kTextManager;
  call.r4.u64 = label.widget;
  call.f1.f64 = fit;
  sub_821D3D50(call, base);

  call = ctx;
  call.r3.u64 = g_scratch;
  call.r4.u64 = kObjectManager;
  call.r5.u64 = REX_LOAD_U32(screen + kPortraitIcon);
  sub_8217BF28(call, base);
  const float art_x = std::bit_cast<float>(REX_LOAD_U32(g_scratch));
  const float art_y = std::bit_cast<float>(REX_LOAD_U32(g_scratch + 4));
  const float scale = REX_LOAD_U32(kLanguage) ? kPortraitScale : 1.0f;
  const float x = art_x;
  const float y = std::min(art_y + art_h * scale + kNameGap, kNameLowestY);
  PlaceObject(ctx, base, label.panel, x, y);
  g_name_character = character;
  g_name_until = std::chrono::steady_clock::now() + kNameShownFor;
}

void TickCostumeName(PPCContext& ctx, uint8_t* base) {
  if (g_name_character &&
      (std::chrono::steady_clock::now() >= g_name_until ||
       static_cast<int>(REX_LOAD_U32(kShownCharacter)) != g_name_character))
    HideCostumeName(ctx, base);
}

}  // namespace

// sub_8222FFE8: builds the status page.
REX_HOOK_RAW(sub_8222FFE8) {
  __imp__sub_8222FFE8(ctx, base);
  g_name_character = 0;
  RefreshCostumePrompt(ctx, base);
}

// sub_82230658: the status page's input. Nothing there reads X.
REX_HOOK_RAW(sub_82230658) {
  const uint32_t menu = REX_LOAD_U32(kMenuState);
  const int character = static_cast<int>(REX_LOAD_U32(kShownCharacter));
  if (menu && static_cast<int32_t>(REX_LOAD_U32(menu + 444)) == -1 &&
      (REX_LOAD_U32(menu + 448) & kButtonX) && CanChangeCostume(character)) {
    if (eternalsonata::WearNextCostume(character) >= 0) {
      PPCContext call = ctx;
      call.r3.u64 = static_cast<uint32_t>(character);
      sub_82232C10(call, base);
      call = ctx;
      call.r3.u64 = REX_LOAD_U32(kSoundManager);
      call.r4.u64 = kCursorSound;
      call.r5.u64 = 0;
      call.r6.u64 = 0;
      sub_821425D8(call, base);
      ShowCostumeName(ctx, base, character);
    }
  }
  __imp__sub_82230658(ctx, base);
  RefreshCostumePrompt(ctx, base);
  TickCostumeName(ctx, base);
}
