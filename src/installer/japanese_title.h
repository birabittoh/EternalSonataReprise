#pragma once

// The Japanese title screen (title_jpn.bmd), which the title loads for
// Japanese text, made from a PAL title.bmd: PAL ships the Trusty Bell logo
// in the title effect's first texture without using it, so this only points
// the logo sprite at it. The labels and footer stay PAL's.

#include <cstdint>
#include <vector>

namespace eternalsonata {

// From a PAL layout title.bmd, the Xbox 360's or a converted PS3 one. Empty
// when the layout is not one of those.
std::vector<uint8_t> BuildJapaneseTitle(const std::vector<uint8_t>& title);

}  // namespace eternalsonata
