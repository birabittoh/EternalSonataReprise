#include "japanese_title.h"

#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace eternalsonata {
namespace {

using Bytes = std::vector<uint8_t>;

uint32_t Rd16(const Bytes& d, size_t o) {
  return uint32_t(d[o]) << 8 | d[o + 1];
}

uint32_t Rd32(const Bytes& d, size_t o) {
  return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}

void Wr32(Bytes& d, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    d[o + i] = uint8_t(v >> (24 - 8 * i));
}

struct Section {
  size_t offset = 0;
  size_t size = 0;
};

// A Mefc's sections by tag (docs/ps3-assets.md §3.4).
std::optional<Section> FindSection(const Bytes& d, size_t mefc, std::string_view tag) {
  if (mefc + 0x10 > d.size() || std::memcmp(d.data() + mefc, "Mefc", 4) != 0)
    return std::nullopt;
  const size_t end = mefc + 8 + Rd32(d, mefc + 4);
  const size_t dir = mefc + Rd16(d, mefc + 0x0C);
  if (end > d.size() || dir + 4 > end || d[dir] != 'C' || d[dir + 1] != 'K')
    return std::nullopt;
  const size_t entry_size = d[dir + 2];
  const size_t count = d[dir + 3];
  if (entry_size < 12 || dir + 4 + count * entry_size > end)
    return std::nullopt;
  for (size_t i = 0; i < count; ++i) {
    const size_t e = dir + 4 + i * entry_size;
    if (std::string_view(reinterpret_cast<const char*>(d.data() + e), 4) != tag)
      continue;
    const size_t at = mefc + Rd32(d, e + 8);
    // Sections run to the next one in the directory, or the Mefc's end.
    size_t next = end;
    for (size_t j = 0; j < count; ++j) {
      const size_t other = mefc + Rd32(d, dir + 4 + j * entry_size + 8);
      if (other > at && other < next)
        next = other;
    }
    if (at >= next)
      return std::nullopt;
    return Section{at, next - at};
  }
  return std::nullopt;
}

// A TRC rect is u, v, width, height and a pivot, as floats, every 0x28 bytes.
constexpr size_t kRectStride = 0x28;
// The Trusty Bell logo in the title effect's TEX0, rows 2..279, as the 360's
// rect 0 and both Japanese releases have it.
constexpr std::array<uint32_t, 6> kLogoRect = {0x00000000, 0x3B800000, 0x3F800000,
                                               0x3F0A8000, 0x3F000000, 0x3F033C12};
// The Japanese releases draw it this wide; PAL's Eternal Sonata logo, at
// 7.68 on the 360, is narrower.
constexpr uint32_t kLogoWidth = 0x41080000;  // 8.5

// An EFCT element: u32 size, then texture, rect, kind and a pad byte; the
// sprite's width is a float at +0x40.
constexpr size_t kElementRef = 4;
constexpr size_t kElementWidth = 0x40;

}  // namespace

Bytes BuildJapaneseTitle(const Bytes& title) {
  if (title.size() < 16 || std::memcmp(title.data(), "BMD ", 4) != 0)
    return {};
  // The title screen's effect is the fourth entry on both platforms.
  const uint32_t count = Rd32(title, 8);
  size_t effect = 0;
  for (uint32_t i = 1, seen = 0; i <= count && 12 + 4 * (i + 1) <= title.size(); ++i) {
    const uint32_t at = Rd32(title, 12 + 4 * i);
    if (at && ++seen == 4) {
      effect = at;
      break;
    }
  }
  const auto art = FindSection(title, effect, "TEX0");
  const auto rects = FindSection(title, effect, "TRC0");
  const auto efct = FindSection(title, effect, "EFCT");
  if (!effect || !art || !rects || !efct || rects->size < kRectStride || art->size < 0x30 ||
      std::memcmp(title.data() + efct->offset, "etbl", 4) != 0)
    return {};
  // TEX0 is an NTEX holding a DDS, whose height and width are little endian.
  auto le32 = [&](size_t o) {
    return uint32_t(title[o]) | uint32_t(title[o + 1]) << 8 | uint32_t(title[o + 2]) << 16 |
           uint32_t(title[o + 3]) << 24;
  };
  if (std::memcmp(title.data() + art->offset + 8, "DDS ", 4) != 0 ||
      le32(art->offset + 8 + 12) != 512 || le32(art->offset + 8 + 16) != 1024)
    return {};

  Bytes out = title;
  for (size_t i = 0; i < kLogoRect.size(); ++i)
    Wr32(out, rects->offset + 4 * i, kLogoRect[i]);

  // The element offsets (u16, from +12) run up to the first element.
  const size_t table = efct->offset + 12;
  const size_t first = Rd16(title, table);
  int logos = 0, japanese_logos = 0;
  for (size_t at = table; at < efct->offset + first && at + 2 <= efct->offset + efct->size;
       at += 2) {
    const size_t element = efct->offset + Rd16(title, at);
    if (element + kElementWidth + 4 > efct->offset + efct->size)
      continue;
    // PAL's logo is TEX7, rect 0, a sprite; the Trusty Bell one is TEX0's rect 0.
    const uint32_t ref = Rd32(title, element + kElementRef);
    // A PS3 JP title keeps its own logo in TEX7 at the Japanese width.
    if (ref == 0x00000100 ||
        (ref == 0x07000100 && Rd32(title, element + kElementWidth) == kLogoWidth))
      ++japanese_logos;
    if (ref != 0x07000100 || Rd32(title, element + kElementWidth) == kLogoWidth)
      continue;
    Wr32(out, element + kElementRef, 0x00000100);
    Wr32(out, element + kElementWidth, kLogoWidth);
    ++logos;
  }
  // A Japanese release's own title is the screen already.
  if (logos == 0 && japanese_logos == 1)
    return title;
  return logos == 1 ? out : Bytes{};
}

}  // namespace eternalsonata
