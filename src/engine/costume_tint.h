// eternalsonata - Tinted costumes: an existing costume's model and portraits
// with some colours replaced, made at run time so a mod ships only the
// colours. See docs/costumes.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eternalsonata {

// One colour replaced: endpoints near `from` in hue, and saturated enough,
// move towards `to`, and the colour's other shades with them.
struct TintRule {
  uint32_t from = 0, to = 0;
  float hue_range = 20.f / 360.f;  // turns either side of from's hue
  float min_saturation = -1;       // below 0: a share of from's own
  float max_saturation = 1;
  float min_value = 0, max_value = 1;
  // Model textures it applies to, by NTX2 name or by order among the
  // model's textures; every texture when both are empty.
  std::vector<std::string> names;
  std::vector<int> indices;
  // Portrait kinds it applies to, bit per ETERNALSONATA_COSTUME_PORTRAIT_*.
  uint32_t kinds = ~0u;
};

// Applies the rules to a NOBJ's textures in place. False when it is not one.
bool TintModel(const std::vector<TintRule>& rules, std::vector<uint8_t>& model);

// Applies the rules for `kind` to an NTEX portrait chunk (a DDS behind an 8
// byte header) in place, every mip level. False when it is not one.
bool TintPortrait(const std::vector<TintRule>& rules, int kind, std::vector<uint8_t>& ntex);

}  // namespace eternalsonata
