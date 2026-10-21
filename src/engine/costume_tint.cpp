// eternalsonata - Tinted costumes (docs/costumes.md §4).
//
// Only DXT colour endpoints change: the indices, alpha and layout stay, so a
// texture needs no decoding, untiling or re-encoding and keeps every size.
// An endpoint moves by as much as its hue and saturation say it belongs to
// a rule's colour; the hue shift and saturation and value scales that take
// `from` to `to` carry the colour's other shades along.

#include "costume_tint.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace {

constexpr float kHueFeather = 0.03f;
constexpr float kSaturationFeather = 0.1f;
// The minimum saturation a rule without one takes, as a share of from's.
constexpr float kSaturationShare = 0.65f;
// A from this grey has no hue to match or shift: its rule paints to's hue
// and saturation over whatever its saturation and value bounds catch.
constexpr float kGreySaturation = 0.05f;

struct Hsv {
  float h, s, v;
};

Hsv ToHsv(float r, float g, float b) {
  const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
  Hsv out{0, hi > 0 ? d / hi : 0, hi};
  if (d > 0) {
    float h = hi == r ? (g - b) / d : hi == g ? 2 + (b - r) / d : 4 + (r - g) / d;
    h /= 6;
    out.h = h - std::floor(h);
  }
  return out;
}

void ToRgb(Hsv c, float out[3]) {
  const float h6 = (c.h - std::floor(c.h)) * 6;
  const int i = static_cast<int>(h6) % 6;
  const float f = h6 - std::floor(h6);
  const float p = c.v * (1 - c.s), q = c.v * (1 - f * c.s), t = c.v * (1 - (1 - f) * c.s);
  const float table[6][3] = {{c.v, t, p}, {q, c.v, p}, {p, c.v, t},
                             {p, q, c.v}, {t, p, c.v}, {c.v, p, q}};
  std::memcpy(out, table[i], sizeof(table[i]));
}

Hsv FromRgb24(uint32_t rgb) {
  return ToHsv(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f);
}

class Tint {
 public:
  explicit Tint(const eternalsonata::TintRule& rule) {
    const Hsv from = FromRgb24(rule.from), to = FromRgb24(rule.to);
    hue_low_ = from.h - rule.hue_range;
    hue_high_ = from.h + rule.hue_range;
    grey_ = from.s < kGreySaturation;
    saturation_min_ = rule.min_saturation >= 0 ? rule.min_saturation
                      : grey_                  ? 0.f
                                               : from.s * kSaturationShare;
    saturation_max_ = rule.max_saturation;
    value_min_ = rule.min_value;
    value_max_ = rule.max_value;
    hue_ = to.h;
    hue_shift_ = to.h - from.h;
    saturation_ = to.s;
    saturation_scale_ = to.s / std::max(from.s, 1e-3f);
    value_scale_ = to.v / std::max(from.v, 1e-3f);
  }

  uint16_t operator()(uint16_t c) const {
    const float rgb[3] = {((c >> 11) & 31) / 31.f, ((c >> 5) & 63) / 63.f, (c & 31) / 31.f};
    const Hsv hsv = ToHsv(rgb[0], rgb[1], rgb[2]);
    const float w = Weight(hsv);
    if (w <= 0)
      return c;
    float tinted[3];
    const float v = std::clamp(hsv.v * value_scale_, 0.f, 1.f);
    if (grey_)
      ToRgb({hue_, saturation_, v}, tinted);
    else
      ToRgb({hsv.h + hue_shift_, std::clamp(hsv.s * saturation_scale_, 0.f, 1.f), v}, tinted);
    constexpr float kMax[3] = {31, 63, 31};
    uint16_t out = 0;
    for (int k = 0; k < 3; ++k) {
      const float mixed = rgb[k] + (tinted[k] - rgb[k]) * w;
      out = static_cast<uint16_t>(out << (k == 1 ? 6 : 5)) |
            static_cast<uint16_t>(std::clamp(std::lround(mixed * kMax[k]), 0l, long(kMax[k])));
    }
    return out;
  }

 private:
  // 1 inside [low, high], falling to 0 over `feather` outside it.
  static float Band(float x, float low, float high, float feather) {
    return std::clamp((x - low) / feather + 1, 0.f, 1.f) *
           std::clamp((high - x) / feather + 1, 0.f, 1.f);
  }

  float Weight(Hsv c) const {
    float wh = 1;
    if (!grey_) {
      // The hue's turn that starts at the band's low edge, less the feather.
      const float start = hue_low_ - kHueFeather;
      const float h = start + (c.h - start - std::floor(c.h - start));
      wh = Band(h, hue_low_, hue_high_, kHueFeather);
    }
    return wh * Band(c.s, saturation_min_, saturation_max_, kSaturationFeather) *
           Band(c.v, value_min_, value_max_, kSaturationFeather);
  }

  bool grey_;
  float hue_low_, hue_high_, saturation_min_, saturation_max_, value_min_, value_max_;
  float hue_, hue_shift_, saturation_, saturation_scale_, value_scale_;
};

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t Le32(const uint8_t* p) {
  return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

enum class Dxt { kNone, kDxt1, kDxt3, kDxt5 };

// Recolours the colour blocks of `size` bytes of DXT data, whose 16 bit
// words are big endian when `big`.
void TintBlocks(uint8_t* data, size_t size, Dxt format, bool big, const Tint& tint) {
  const auto load = [big](const uint8_t* p) -> uint16_t {
    return big ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]);
  };
  const auto store = [big](uint8_t* p, uint16_t v) {
    p[big ? 0 : 1] = uint8_t(v >> 8);
    p[big ? 1 : 0] = uint8_t(v);
  };
  const size_t stride = format == Dxt::kDxt1 ? 8 : 16;
  for (size_t at = 0; at + stride <= size; at += stride) {
    uint8_t* block = data + at + stride - 8;
    const uint16_t c0 = load(block), c1 = load(block + 2);
    uint16_t n0 = tint(c0), n1 = tint(c1);
    if (format == Dxt::kDxt1) {
      // c0 > c1 selects four colour mode, so keep the ordering: swap the
      // endpoints and the indices naming them when the tint inverted it.
      const bool four = c0 > c1;
      if (four ? n0 < n1 : n0 > n1) {
        std::swap(n0, n1);
        uint32_t idx = load(block + 4) | uint32_t(load(block + 6)) << 16, swapped = 0;
        for (int i = 0; i < 16; ++i) {
          const uint32_t bits = (idx >> (2 * i)) & 3;
          swapped |= (four || bits < 2 ? bits ^ 1 : bits) << (2 * i);
        }
        store(block + 4, uint16_t(swapped));
        store(block + 6, uint16_t(swapped >> 16));
      }
      // Part them in blue alone: a carry out of it would turn near white red.
      if (four && n0 == n1) {
        if ((n0 & 31) < 31)
          ++n0;
        else
          --n1;
      }
    }
    store(block, n0);
    store(block + 2, n1);
  }
}

// An NTX2 (the Xbox 360's): Xenos fetch constant format 18..20 are DXT1,
// DXT2/3 and DXT4/5. Its pixels start at the first 4 KB boundary after its
// header, counted from the model's start.
void TintNtx2(uint8_t* model, size_t model_size, size_t off, size_t size, const Tint& tint) {
  const uint8_t* p = model + off;
  const uint32_t header = Be32(p + 0x0C), pixels = Be32(p + 0x10), name_size = Be32(p + 0x1C);
  const size_t fetch = off + 0x18 + name_size + 0x18;
  if (fetch + 12 > off + size)
    return;
  const uint32_t word1 = Be32(model + fetch + 4);
  const Dxt format = (word1 & 0x3F) == 18   ? Dxt::kDxt1
                     : (word1 & 0x3F) == 19 ? Dxt::kDxt3
                     : (word1 & 0x3F) == 20 ? Dxt::kDxt5
                                            : Dxt::kNone;
  const size_t start = (off + 8 + header + 0xFFF) & ~size_t(0xFFF);
  if (format == Dxt::kNone || start + pixels > std::min(off + size, model_size))
    return;
  TintBlocks(model + start, pixels, format, ((word1 >> 6) & 3) == 1, tint);
}

// An NTEX: a PC DDS behind the chunk header, its mip levels one after the
// other, each a whole number of blocks.
bool TintNtex(uint8_t* chunk, size_t size, const Tint& tint) {
  constexpr size_t kDds = 8, kHeader = kDds + 128;
  if (size < kHeader || std::memcmp(chunk + kDds, "DDS ", 4) != 0)
    return false;
  const uint8_t* fourcc = chunk + kDds + 84;
  const Dxt format = std::memcmp(fourcc, "DXT1", 4) == 0   ? Dxt::kDxt1
                     : std::memcmp(fourcc, "DXT3", 4) == 0 ? Dxt::kDxt3
                     : std::memcmp(fourcc, "DXT5", 4) == 0 ? Dxt::kDxt5
                                                           : Dxt::kNone;
  if (format == Dxt::kNone)
    return false;
  TintBlocks(chunk + kHeader, size - kHeader, format, false, tint);
  return true;
}

bool Applies(const eternalsonata::TintRule& rule, int index, std::string_view name) {
  if (rule.names.empty() && rule.indices.empty())
    return true;
  return std::find(rule.indices.begin(), rule.indices.end(), index) != rule.indices.end() ||
         (!name.empty() && std::find(rule.names.begin(), rule.names.end(), name) != rule.names.end());
}

}  // namespace

namespace eternalsonata {

bool TintModel(const std::vector<TintRule>& rules, std::vector<uint8_t>& model) {
  if (model.size() < 8 || std::memcmp(model.data(), "NOBJ", 4) != 0)
    return false;
  const size_t end = std::min<size_t>(Be32(model.data() + 4), model.size());
  std::vector<Tint> tints(rules.begin(), rules.end());
  // Textures count in file order, as models.py list shows them. The PS3's
  // are NTEX chunks and carry no name.
  int index = 0;
  const auto visit = [&](size_t from, size_t to, auto& self) -> void {
    for (size_t off = from; off + 8 <= to;) {
      const uint32_t size = Be32(model.data() + off + 4);
      if (size < 8 || off + size > to)
        return;
      const uint8_t* tag = model.data() + off;
      if (std::memcmp(tag, "NMDL", 4) == 0) {
        // Its header is 64 bytes, 96 with flag 2.
        const bool long_header = (model[off + 33] & 2) != 0;
        self(off + (long_header ? 96 : 64), off + size, self);
      } else if (std::memcmp(tag, "NTX2", 4) == 0 && size > 0x4C) {
        const char* name = reinterpret_cast<const char*>(tag + 0x2C);
        const std::string_view texture(name, strnlen(name, 32));
        for (size_t r = 0; r < rules.size(); ++r)
          if (Applies(rules[r], index, texture))
            TintNtx2(model.data(), end, off, size, tints[r]);
        ++index;
      } else if (std::memcmp(tag, "NTEX", 4) == 0) {
        for (size_t r = 0; r < rules.size(); ++r)
          if (Applies(rules[r], index, {}))
            TintNtex(model.data() + off, size, tints[r]);
        ++index;
      }
      off += size;
    }
  };
  visit(8, end, visit);
  return true;
}

bool TintPortrait(const std::vector<TintRule>& rules, int kind, std::vector<uint8_t>& ntex) {
  if (ntex.size() < 8 || std::memcmp(ntex.data(), "NTEX", 4) != 0)
    return false;
  for (const TintRule& rule : rules)
    if (((rule.kinds >> kind) & 1) && !TintNtex(ntex.data(), ntex.size(), Tint(rule)))
      return false;
  return true;
}

}  // namespace eternalsonata
