// eternalsonata - ReXGlue Recompiled Project
//
// Driver for the shared pack reader; see guest_shaders.h and
// KryptonRecomp/guest_shader_pack.h.
//
// The pack symbol comes from a generated `.S` that `.incbin`s guest_shaders.bin
// into the executable's own read-only data (see write_pack_asm in
// scripts/gen-guest-shaders.py), so there is nothing to load from disk.

#include "guest_shaders.h"

#include "guest_shader_pack.h"

extern "C" {
extern const uint8_t kGuestShaderPackData[];
extern const uint8_t kGuestShaderPackDataEnd[];
}

namespace eternalsonata {
namespace {

// The renderer-owned entries past the slot tables, in gen-guest-shaders.py's
// EXTRA_NAMES order.
enum Extra : uint32_t { kPointSprite, kSpriteBatchVertex, kSpriteBatchPixel, kExtraCount };

// This title's pack: magic 'ESGS', version, and the flag/block layout it uses.
// Eternal Sonata carries a param_gen flag and a fixed 64 byte literal block,
// with no per-entry hash section, and three renderer-owned extras.
krypton::PackTraits MakeTraits() {
  krypton::PackTraits traits{};
  traits.magic = 0x53475345;  // 'ESGS', little endian
  traits.version = 9;
  traits.flag_point_size = 1 << 0;
  traits.flag_has_cube = 1 << 1;
  traits.flag_param_gen = 1 << 2;
  traits.flag_footprint = 1 << 3;
  traits.literal_block_bytes = 64;
  traits.literals_tail_aligned = false;
  traits.has_hash_section = false;
  traits.extra_entries = kExtraCount;
  return traits;
}

krypton::GuestShaderPack g_pack;

}  // namespace

bool LoadGuestShaders() {
  const size_t size = size_t(kGuestShaderPackDataEnd - kGuestShaderPackData);
  return g_pack.Load(kGuestShaderPackData, size, MakeTraits());
}

const GuestShader& GuestVertexShader(uint32_t slot) { return g_pack.Vertex(slot); }
const GuestShader& GuestPixelShader(uint32_t slot) { return g_pack.Pixel(slot); }

const GuestShader& GuestPointSpriteShader() { return g_pack.Extra(kPointSprite); }
const GuestShader& GuestSpriteBatchVertexShader() { return g_pack.Extra(kSpriteBatchVertex); }
const GuestShader& GuestSpriteBatchPixelShader() { return g_pack.Extra(kSpriteBatchPixel); }

uint32_t GuestVertexShaderCount() { return g_pack.VertexCount(); }
uint32_t GuestPixelShaderCount() { return g_pack.PixelCount(); }

}  // namespace eternalsonata
