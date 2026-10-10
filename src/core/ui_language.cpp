#include "ui_language.h"

#include <atomic>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "ui_text.h"
#include "settings.h"

namespace eternalsonata {
namespace {

constexpr uint32_t kTextLanguage = 0x8243D370u;
constexpr uint32_t kJapanese = 0;
constexpr char kSlots[][5] = {"JPN ", "USA ", "GBR ", "FRA ", "ITA ", "DEU ", "ESP "};

// Covers the image's .data, which holds the executable's text blobs.
constexpr uint32_t kImageBegin = 0x82000000u;
constexpr uint32_t kImageEnd = 0x825E9600u;

std::atomic<uint32_t> g_generation{1};

rex::memory::Memory* Memory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

}  // namespace

int BtxLanguageFromCode(std::string_view code) {
  if (code == "ja") return 0;
  if (code == "en") return 1;
  if (code == "fr") return 3;
  if (code == "it") return 4;
  if (code == "de") return 5;
  if (code == "es") return 6;
  return -1;
}

namespace {

bool English(uint32_t language) {
  return language == 1 || language == 2;
}

uint32_t Resolve(uint32_t game) {
  const int ui = BtxLanguageFromCode(IntroLanguageCode());
  if (ui < 0 || game >= std::size(kSlots))
    return game;
  // A mod language rewrites the game's block in place, the executable's too.
  const uint32_t selected = UserLanguageId();
  if (selected < 1 || selected > 6)
    return game;
  // One font draws either Shift JIS or cp1252, and it follows the game.
  if ((uint32_t(ui) == kJapanese) != (game == kJapanese))
    return game;
  if (English(uint32_t(ui)) && English(game))
    return game;
  return uint32_t(ui);
}

uint32_t Load(const uint8_t* at) {
  return rex::memory::load_and_swap<uint32_t>(at);
}

}  // namespace

uint32_t UiTextLanguage(uint32_t game_language) {
  // Every text lookup asks, so the answer is kept per thread until a language
  // changes.
  thread_local uint32_t generation = 0;
  thread_local uint32_t game = ~0u;
  thread_local uint32_t ui = 0;
  const uint32_t current = g_generation.load(std::memory_order_acquire);
  if (generation != current || game != game_language) {
    generation = current;
    game = game_language;
    ui = Resolve(game_language);
  }
  return ui;
}

const char* BtxLanguageSlot(uint32_t language) {
  return kSlots[language < std::size(kSlots) ? language : 1];
}

uint32_t GameTextLanguage() {
  auto* memory = Memory();
  return memory ? Load(memory->TranslateVirtual<const uint8_t*>(kTextLanguage)) : 1;
}

uint32_t UiTextLanguage() {
  return UiTextLanguage(GameTextLanguage());
}

void InvalidateUiTextLanguage() {
  g_generation.fetch_add(1, std::memory_order_release);
}

bool IsUiTextBlob(uint32_t blob) {
  return blob >= kImageBegin && blob < kImageEnd;
}

bool LookupBtx(uint32_t blob, uint32_t sid, uint32_t language, uint32_t& text) {
  auto* memory = Memory();
  if (!memory || !blob || language >= std::size(kSlots))
    return false;
  const auto* header = memory->TranslateVirtual<const uint8_t*>(blob);
  if (std::memcmp(header, "BTX ", 4) != 0)
    return false;
  const uint32_t count = Load(header + 0x0C);
  uint32_t block = blob + Load(header + 4);
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    const auto* at = memory->TranslateVirtual<const uint8_t*>(block);
    if (std::memcmp(at, kSlots[language], 4) == 0) {
      const uint32_t entries = Load(at + 0x10);
      const auto* table = at + Load(at + 4);
      text = 0;
      for (uint32_t e = 0; e < entries; ++e) {
        if (Load(table + 8 * e) == sid) {
          text = block + Load(table + 8 * e + 4);
          break;
        }
      }
      return true;
    }
    block += Load(at + 8);
  }
  return false;
}

}  // namespace eternalsonata
