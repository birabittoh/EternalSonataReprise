// eternalsonata - ReXGlue Recompiled Project
//
// The guest's shader inventory, translated and compiled ahead of time.
//
// Every shader this game can bind lives in one static chunked blob inside
// default.xex, walked once at init by sub_82129260 into the two 256 entry
// tables at dword_824BBEE8 and dword_824BC2E8. That closed set is what makes
// static compilation possible at all: scripts/gen-guest-shaders.py extracts the
// 260 containers, translates the microcode to HLSL (KryptonRecomp/xenos_hlsl.py) and
// compiles it with DXC, all at build time. Nothing here translates anything.
//
// The result, `guest_shaders.bin`, is linked straight into the executable's
// read-only data by a generated `.S` (see scripts/gen-guest-shaders.py's
// write_pack_asm), so there is nothing to deploy or go missing at runtime; this
// is the reader for the in-memory pack. Lookups are by *guest table slot*, the
// same index the device mirror resolves a bound shader object to, so binding is
// a direct index rather than a hash of anything.
//
// The pack format and the reader live in KryptonRecomp/guest_shader_pack.h,
// shared with the other ReXGlue titles; guest_shaders.cpp is the thin driver
// that fixes this title's PackTraits and pack symbol.

#pragma once

#include <cstddef>
#include <cstdint>

#include "guest_shader_pack.h"

namespace eternalsonata {

using GuestVertexInput = krypton::GuestVertexInput;
using GuestShader = krypton::GuestShader;

// Parse the pack linked into the executable. Safe to call more than once; only
// the first call does any work. Returns false and logs if the pack is malformed,
// in which case every lookup below returns an invalid shader.
bool LoadGuestShaders();

// Lookup by guest table slot, 1..255. An out-of-range slot, or one the game
// never populated, gives an entry whose `valid()` is false.
const GuestShader& GuestVertexShader(uint32_t slot);
const GuestShader& GuestPixelShader(uint32_t slot);

// The point sprite expansion geometry shader, which is the renderer's own and
// so has no guest table slot. A POINT_LIST draw needs it: the console rasterises
// each vertex as a quad sized by the vertex shader's e63 export and generates a
// sprite coordinate across it, and a host point list does neither. See
// KryptonRecomp/xenos_hlsl.py's point_sprite_gs.
const GuestShader& GuestPointSpriteShader();
// The sprite batcher's pair: vs_015 with a COLOR0 input carried on interpolator
// 1, and ps_003 modulating by it instead of pixel c0. See
// native_renderer_batch.cpp and the derivation in scripts/gen-guest-shaders.py.
const GuestShader& GuestSpriteBatchVertexShader();
const GuestShader& GuestSpriteBatchPixelShader();

// How many slots of each table the pack actually filled, for the startup log.
uint32_t GuestVertexShaderCount();
uint32_t GuestPixelShaderCount();

}  // namespace eternalsonata
