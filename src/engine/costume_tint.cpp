// eternalsonata - Tinted costumes (docs/costumes.md §4).
//
// Only DXT colour blocks change, each encoded again on its own: alpha and
// layout stay, so a texture needs no untiling and keeps every size. A colour
// moves by as much as its hue and saturation say it belongs to a rule's
// colour; the hue shift and saturation and value scales that take
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
    // Brightening follows 1 - e^(-k v) through from and to instead: a scale
    // (dark navy to white is about 4) clipped the shading into flat white and
    // multiplied the texture's noise, and this rolls off before white.
    value_exposure_ = to.v > from.v && from.v > 1e-3f
                          ? -std::log(1 - std::min(to.v, 0.999f)) / from.v
                          : 0.f;
  }

  // `region` scales the change, for where in the texture the colour sits.
  void operator()(float rgb[3], float region) const {
    const Hsv hsv = ToHsv(rgb[0], rgb[1], rgb[2]);
    const float w = Weight(hsv) * region;
    if (w <= 0)
      return;
    float tinted[3];
    const float v = value_exposure_ > 0 ? 1 - std::exp(-value_exposure_ * hsv.v)
                                        : std::clamp(hsv.v * value_scale_, 0.f, 1.f);
    if (grey_)
      ToRgb({hue_, saturation_, v}, tinted);
    else
      ToRgb({hsv.h + hue_shift_, std::clamp(hsv.s * saturation_scale_, 0.f, 1.f), v}, tinted);
    for (int k = 0; k < 3; ++k)
      rgb[k] += (tinted[k] - rgb[k]) * w;
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
  float hue_, hue_shift_, saturation_, saturation_scale_, value_scale_, value_exposure_;
};

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t Le32(const uint8_t* p) {
  return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

enum class Dxt { kNone, kDxt1, kDxt3, kDxt5 };

uint16_t Pack565(const float rgb[3]) {
  constexpr long kMax[3] = {31, 63, 31};
  uint16_t out = 0;
  for (int k = 0; k < 3; ++k)
    out = static_cast<uint16_t>(out << (k == 1 ? 6 : 5)) |
          static_cast<uint16_t>(std::clamp(std::lround(rgb[k] * kMax[k]), 0l, kMax[k]));
  return out;
}

void Unpack565(uint16_t c, float rgb[3]) {
  rgb[0] = ((c >> 11) & 31) / 31.f;
  rgb[1] = ((c >> 5) & 63) / 63.f;
  rgb[2] = (c & 31) / 31.f;
}

// For each of a block's four colours, the index among `choices` (positions
// from e0 to e1 by index, negative for none) whose colour is nearest.
void Nearest(const float t[4][3], const float e0[3], const float e1[3], const float choices[4],
             int pick[4]) {
  for (int k = 0; k < 4; ++k) {
    float best = INFINITY;
    for (int i = 0; i < 4; ++i) {
      if (choices[i] < 0)
        continue;
      float d = 0;
      for (int j = 0; j < 3; ++j) {
        const float e = e0[j] + (e1[j] - e0[j]) * choices[i] - t[k][j];
        d += e * e;
      }
      if (d < best) {
        best = d;
        pick[k] = i;
      }
    }
  }
}

// Least squares endpoints for colours `t`, `count` of each, at `pos` along
// the line; false where those positions don't fix one.
bool FitLine(const float t[4][3], const float count[4], const float pos[4], float f0[3],
             float f1[3]) {
  float p = 0, q = 0, r = 0, x[3] = {}, y[3] = {}, total = 0;
  for (int k = 0; k < 4; ++k) {
    const float a = pos[k], n = count[k];
    p += n * (1 - a) * (1 - a);
    q += n * a * (1 - a);
    r += n * a * a;
    total += n;
    for (int j = 0; j < 3; ++j) {
      x[j] += n * (1 - a) * t[k][j];
      y[j] += n * a * t[k][j];
    }
  }
  const float det = p * r - q * q;
  total = std::max(total, 1.f);
  if (det <= 1e-6f * total * total)
    return false;
  for (int j = 0; j < 3; ++j) {
    f0[j] = std::clamp((r * x[j] - q * y[j]) / det, 0.f, 1.f);
    f1[j] = std::clamp((p * y[j] - q * x[j]) / det, 0.f, 1.f);
  }
  return true;
}

// Recolours the colour blocks of `size` bytes of DXT data, whose 16 bit
// words are big endian when `big`, each by its weight in `region` if given.
// A block's pixels are its four palette colours, so tinting those tints every
// pixel; the block is then encoded again, endpoints and indices both. Moving
// the endpoints alone left blocks that mix the tinted colour with another
// (an outline, a fold) blotchy and stair stepped. Block by block, so tiling
// and block order don't matter.
void TintBlocks(uint8_t* data, size_t size, Dxt format, bool big, const Tint& tint,
                const std::vector<float>* region = nullptr) {
  const auto load = [big](const uint8_t* p) -> uint16_t {
    return big ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]);
  };
  const auto store = [big](uint8_t* p, uint16_t v) {
    p[big ? 0 : 1] = uint8_t(v >> 8);
    p[big ? 1 : 0] = uint8_t(v);
  };
  constexpr float kFour[4] = {0, 1, 1 / 3.f, 2 / 3.f};
  constexpr float kThree[4] = {0, 1, 0.5f, -1};  // 3 is transparent
  const bool dxt1 = format == Dxt::kDxt1;
  const size_t stride = dxt1 ? 8 : 16;
  for (size_t at = 0; at + stride <= size; at += stride) {
    const float weight = region ? (*region)[at / stride] : 1;
    if (weight <= 0)
      continue;
    uint8_t* block = data + at + stride - 8;
    const uint16_t c0 = load(block), c1 = load(block + 2);
    const uint32_t idx = load(block + 4) | uint32_t(load(block + 6)) << 16;
    // DXT3/5 always decode four colours.
    const bool four = !dxt1 || c0 > c1;
    const float* old_pos = four ? kFour : kThree;
    float count[4] = {};
    for (int i = 0; i < 16; ++i)
      count[(idx >> (2 * i)) & 3] += 1;
    const bool transparent = !four && count[3] > 0;
    if (!four)
      count[3] = 0;
    float e0[3], e1[3], t[4][3];
    Unpack565(c0, e0);
    Unpack565(c1, e1);
    bool changed = false;
    for (int k = 0; k < 4; ++k) {
      float palette[3];
      for (int j = 0; j < 3; ++j)
        palette[j] = t[k][j] = e0[j] + (e1[j] - e0[j]) * std::max(old_pos[k], 0.f);
      tint(t[k], weight);
      for (int j = 0; j < 3; ++j)
        changed |= std::abs(t[k][j] - palette[j]) > 0.5f / 255;
    }
    if (!changed)
      continue;

    // Start from the principal axis of the tinted colours.
    float total = 0, mean[3] = {};
    for (int k = 0; k < 4; ++k) {
      total += count[k];
      for (int j = 0; j < 3; ++j)
        mean[j] += count[k] * t[k][j];
    }
    for (float& m : mean)
      m /= std::max(total, 1.f);
    float cov[3][3] = {};
    for (int k = 0; k < 4; ++k)
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
          cov[i][j] += count[k] * (t[k][i] - mean[i]) * (t[k][j] - mean[j]);
    float axis[3] = {1, 1, 1};
    for (int step = 0; step < 8; ++step) {
      float next[3] = {};
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
          next[i] += cov[i][j] * axis[j];
      const float norm = std::max(std::sqrt(next[0] * next[0] + next[1] * next[1] +
                                            next[2] * next[2]),
                                  1e-12f);
      for (int i = 0; i < 3; ++i)
        axis[i] = next[i] / norm;
    }
    float lo = INFINITY, hi = -INFINITY;
    for (int k = 0; k < 4; ++k) {
      if (count[k] <= 0)
        continue;
      const float proj = (t[k][0] - mean[0]) * axis[0] + (t[k][1] - mean[1]) * axis[1] +
                         (t[k][2] - mean[2]) * axis[2];
      lo = std::min(lo, proj);
      hi = std::max(hi, proj);
    }
    if (lo > hi)
      lo = hi = 0;
    float f0[3], f1[3];
    for (int j = 0; j < 3; ++j) {
      f0[j] = mean[j] + axis[j] * lo;
      f1[j] = mean[j] + axis[j] * hi;
    }

    // Choose indices against the line, refit it to them, twice.
    const float* choices = transparent ? kThree : kFour;
    int pick[4] = {};
    for (int round = 0; round < 2; ++round) {
      Nearest(t, f0, f1, choices, pick);
      const float pos[4] = {choices[pick[0]], choices[pick[1]], choices[pick[2]],
                            choices[pick[3]]};
      float g0[3], g1[3];
      if (FitLine(t, count, pos, g0, g1)) {
        std::copy(g0, g0 + 3, f0);
        std::copy(g1, g1 + 3, f1);
      }
    }
    uint16_t n0 = Pack565(f0), n1 = Pack565(f1);
    // c0 > c1 selects four colours, c0 <= c1 three and transparency.
    if (dxt1 && (transparent ? n0 > n1 : n0 < n1))
      std::swap(n0, n1);
    Unpack565(n0, f0);
    Unpack565(n1, f1);
    Nearest(t, f0, f1, choices, pick);
    // Equal endpoints decode as three colours, where 3 is transparent.
    if (dxt1 && !transparent && n0 == n1)
      std::fill(pick, pick + 4, 0);
    if (transparent)
      pick[3] = 3;
    uint32_t out = 0;
    for (int i = 0; i < 16; ++i)
      out |= uint32_t(pick[(idx >> (2 * i)) & 3]) << (2 * i);
    store(block, n0);
    store(block + 2, n1);
    store(block + 4, uint16_t(out));
    store(block + 6, uint16_t(out >> 16));
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

// How much of a span of a width by height portrait, in its full size's
// pixels, the region covers: 4 by 4 samples, a 4 by 4 block's pixel centres
// at the full size.
float Coverage(const eternalsonata::TintRegion& region, float x0, float y0, float x1, float y1,
               float width, float height) {
  constexpr int kSamples = 4;
  float sum = 0;
  for (int j = 0; j < kSamples; ++j) {
    const float y = y0 + (y1 - y0) * (j + 0.5f) / kSamples;
    for (int i = 0; i < kSamples; ++i) {
      const float x = x0 + (x1 - x0) * (i + 0.5f) / kSamples;
      const auto inside = [x, y](const eternalsonata::TintRegion::Rect& r) {
        return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
      };
      if ((!region.rects.empty() && std::none_of(region.rects.begin(), region.rects.end(), inside)) ||
          std::any_of(region.exclude.begin(), region.exclude.end(), inside))
        continue;
      if (!region.mask) {
        sum += 1;
        continue;
      }
      const auto mx = std::min(uint32_t(x / width * region.mask_width), region.mask_width - 1);
      const auto my = std::min(uint32_t(y / height * region.mask_height), region.mask_height - 1);
      sum += (*region.mask)[size_t(my) * region.mask_width + mx] / 255.f;
    }
  }
  return sum / (kSamples * kSamples);
}

// The region over every block of a width by height DXT mip chain, in the
// order the blocks are stored; blocks past the chain get none.
std::vector<float> RegionWeights(const eternalsonata::TintRegion& region, uint32_t width,
                                 uint32_t height, size_t blocks) {
  std::vector<float> weights;
  weights.reserve(blocks);
  for (uint32_t w = width, h = height; weights.size() < blocks;
       w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
    const float sx = float(width) / w, sy = float(height) / h;
    for (uint32_t by = 0; by < (h + 3) / 4; ++by)
      for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
        weights.push_back(Coverage(region, bx * 4 * sx, by * 4 * sy, std::min((bx + 1) * 4, w) * sx,
                                   std::min((by + 1) * 4, h) * sy, float(width), float(height)));
    if (w == 1 && h == 1)
      break;
  }
  weights.resize(blocks, 0.f);
  return weights;
}

// An NTEX: a PC DDS behind the chunk header, its mip levels one after the
// other, each a whole number of blocks; only `region` of it, if given.
bool TintNtex(uint8_t* chunk, size_t size, const Tint& tint,
              const eternalsonata::TintRegion* region = nullptr) {
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
  if (!region || region->empty()) {
    TintBlocks(chunk + kHeader, size - kHeader, format, false, tint);
    return true;
  }
  const uint32_t height = Le32(chunk + kDds + 12), width = Le32(chunk + kDds + 16);
  if (!width || !height)
    return false;
  const size_t stride = format == Dxt::kDxt1 ? 8 : 16;
  const auto weights = RegionWeights(*region, width, height, (size - kHeader) / stride);
  TintBlocks(chunk + kHeader, size - kHeader, format, false, tint, &weights);
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
    if (((rule.kinds >> kind) & 1) &&
        !TintNtex(ntex.data(), ntex.size(), Tint(rule), &rule.regions[kind]))
      return false;
  return true;
}

}  // namespace eternalsonata
