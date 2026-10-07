// eternalsonata: Characters 11 and 12 on the status page, and X changing
// the shown character's costume (docs/costumes.md).

#include "party_status_menu.h"

#include <algorithm>
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
#include "ps3_natives.h"
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

constexpr std::uint32_t kLanguageIndexAddress = 0x8243D370u;

// Western ones in Latin-1, as the text records are single byte; Japanese is
// converted to Shift-JIS on use.
constexpr const char* kCostumeLabel[7] = {
    "コスチューム", "Costume", "Costume", "Costume",
    "Costume",      "Kost\xFCm", "Traje"};

std::uint32_t g_list_copy = 0;
std::uint32_t g_label = 0;
bool g_prompt_built = false;
int g_prompt_shown = -1;

std::uint32_t ReadU32(const std::uint8_t* base, std::uint32_t address) {
  return rex::memory::load_and_swap<std::uint32_t>(base + address);
}

void WriteU32(std::uint8_t* base, std::uint32_t address, std::uint32_t value) {
  rex::memory::store_and_swap<std::uint32_t>(base + address, value);
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

  if (!g_list_copy) {
    auto* mem = rex::system::kernel_memory();
    g_list_copy = mem ? mem->SystemHeapAlloc(4u * (kMaxListWords + 1u), 0x20) : 0;
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
  if (sid != kCostumeSid || blob < 0x82000000u || blob >= 0x82600000u)
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
  if (eternalsonata::IsPs3Target() && eternalsonata::Ps3CostumesBlocked())
    return false;
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

}  // namespace

// sub_8222FFE8: builds the status page.
REX_HOOK_RAW(sub_8222FFE8) {
  __imp__sub_8222FFE8(ctx, base);
  RefreshCostumePrompt(ctx, base);
}

// sub_82230658: the status page's input. Nothing there reads X.
REX_HOOK_RAW(sub_82230658) {
  const uint32_t menu = REX_LOAD_U32(kMenuState);
  const int character = static_cast<int>(REX_LOAD_U32(kShownCharacter));
  if (menu && static_cast<int32_t>(REX_LOAD_U32(menu + 444)) == -1 &&
      (REX_LOAD_U32(menu + 448) & kButtonX) && CanChangeCostume(character)) {
    const int next = eternalsonata::NextUnlockedCostume(character);
    if (eternalsonata::WearCostume(character, next) >= 0) {
      PPCContext call = ctx;
      call.r3.u64 = static_cast<uint32_t>(character);
      sub_82232C10(call, base);
      call = ctx;
      call.r3.u64 = REX_LOAD_U32(kSoundManager);
      call.r4.u64 = kCursorSound;
      call.r5.u64 = 0;
      call.r6.u64 = 0;
      sub_821425D8(call, base);
    }
  }
  __imp__sub_82230658(ctx, base);
  RefreshCostumePrompt(ctx, base);
}
