// eternalsonata - ReXGlue Recompiled Project
//
// The guest's shader inventory, translated and compiled ahead of time.
//
// Every shader this game can bind lives in one static chunked blob inside
// default.xex, walked once at init by sub_82129260 into the two 256 entry
// tables at dword_824BBEE8 and dword_824BC2E8. That closed set is what makes
// static compilation possible at all: scripts/gen-guest-shaders.py extracts the
// 260 containers, translates the microcode to HLSL (scripts/xenos_hlsl.py) and
// compiles it with DXC, all at build time. Nothing here translates anything.
//
// The result is `guest_shaders.bin`, deployed next to the exe, and this is the
// reader for it. Lookups are by *guest table slot*, the same index the device
// mirror resolves a bound shader object to, so binding is a direct index rather
// than a hash of anything.
//
// No Plume types here: the pack is bytes, and the caller turns a blob into a
// RenderShader.

#pragma once

#include <cstddef>
#include <cstdint>

namespace eternalsonata {

// D3DDECLUSAGE plus its usage index, one per vertex shader input. This is the
// container's own vertex fetch patch table, which is what sub_82267218 matches
// against the bound vertex declaration at draw time; a host input layout is
// built from it instead, which is why vfetch never has to be translated.
struct GuestVertexInput {
  uint8_t usage;
  uint8_t usage_index;
};

// One shader. `dxil` and `spirv` point into the pack's blob section and are
// null when that format was not built for this platform.
struct GuestShader {
  const void* dxil = nullptr;
  uint32_t dxil_size = 0;
  const void* spirv = nullptr;
  uint32_t spirv_size = 0;

  // Pixel shaders: which texture/sampler register slots the shader declares.
  // Slots run 0..15 across this title.
  uint32_t texture_mask = 0;

  // Vertex shaders only.
  const GuestVertexInput* inputs = nullptr;
  uint32_t input_count = 0;

  // Interpolator semantic keys, sorted. Both shader types declare their
  // interpolators as TEXCOORD<key>, so the host linker performs the register
  // renumbering sub_822679A8 does at draw time. D3D12 requires a pixel shader's
  // inputs to be a subset of the vertex shader's outputs, so a pipeline builder
  // should check one set against the other before creating a pipeline: the
  // hardware would have synthesised a constant for an unmatched input, and the
  // host will simply refuse to link.
  const uint8_t* interpolator_keys = nullptr;
  uint32_t interpolator_key_count = 0;

  // The compiler's literal constant pool: 64 bytes, four host order float4s
  // that belong in constant registers 252, 253, 254 and 255 of this shader's
  // own bank. Never null; a shader that carries no pool points at zeros.
  //
  // The guest writes these through neither constant setter, so they are not in
  // the register shadows and have to be overlaid on top of the bank a draw
  // uploads. 209 of the 260 shaders carry one, and they are not decorative:
  // nearly every pixel shader spells saturate as `min(x, c253.w)` and clamps
  // its exported alpha against `c255.x`, so a zero pool makes alpha zero and
  // the fixed function alpha test then discards the pixel. See the note above
  // PREFIX_CANDIDATES in scripts/xenos_ucode.py.
  const uint8_t* literals = nullptr;
  bool has_literals = false;

  bool exports_point_size = false;
  bool has_cube_texture = false;

  bool valid() const { return dxil_size != 0 || spirv_size != 0; }
};

// Read the pack. `path` is the pack file; passing nothing looks for
// `guest_shaders.bin` next to the executable. Safe to call more than once; only
// the first call does any work. Returns false and logs if the pack is missing or
// malformed, in which case every lookup below returns an invalid shader.
bool LoadGuestShaders(const char* path = nullptr);

// Lookup by guest table slot, 1..255. An out-of-range slot, or one the game
// never populated, gives an entry whose `valid()` is false.
const GuestShader& GuestVertexShader(uint32_t slot);
const GuestShader& GuestPixelShader(uint32_t slot);

// The point sprite expansion geometry shader, which is the renderer's own and
// so has no guest table slot. A POINT_LIST draw needs it: the console rasterises
// each vertex as a quad sized by the vertex shader's e63 export and generates a
// sprite coordinate across it, and a host point list does neither. See
// scripts/xenos_hlsl.py's point_sprite_gs.
const GuestShader& GuestPointSpriteShader();

// How many slots of each table the pack actually filled, for the startup log.
uint32_t GuestVertexShaderCount();
uint32_t GuestPixelShaderCount();

}  // namespace eternalsonata
