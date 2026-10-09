#pragma once

// The interface language: the BTX block the executable's own text is drawn
// in, apart from the game language in dword_8243D370, which the story text,
// the font and the layouts keep following.

#include <cstdint>
#include <string_view>

namespace eternalsonata {

// The dword_8243D370 index (JPN USA GBR FRA ITA DEU ESP) for executable side
// text, given the game's. It is the game's own while a mod language is
// selected, or when the two sit on different sides of the Japanese font split.
uint32_t UiTextLanguage(uint32_t game_language);
uint32_t UiTextLanguage();

// A res/lang code as a dword_8243D370 index, or -1.
int BtxLanguageFromCode(std::string_view code);

// The BTX fourcc for a dword_8243D370 index.
const char* BtxLanguageSlot(uint32_t language);

// dword_8243D370 itself.
uint32_t GameTextLanguage();

// Call after either language changes.
void InvalidateUiTextLanguage();

// Whether `blob` is text whose language follows the interface: one in the
// guest image. Ask before a PS3 block replaces an image one.
bool IsUiTextBlob(uint32_t blob);

// sub_8223B780 for language `language`, bounded by the blob's language count.
// False when the blob has no such block, so the caller can fall back to the
// stock lookup; `text` is then left alone.
bool LookupBtx(uint32_t blob, uint32_t sid, uint32_t language, uint32_t& text);

}  // namespace eternalsonata
