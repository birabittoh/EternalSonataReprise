// eternalsonata - ReXGlue Recompiled Project
//
// Host-side mirror of the guest Direct3D device.
//
// The game's D3D runtime is statically linked into default.xex, so there are
// no imports to hook: interception happens by overriding the recompiled
// functions themselves (see eternalsonata_config.toml, "Direct3D 9 (Xbox 360
// static lib)", where every entry point carries an `evidence` field).
//
// This layer only observes. The guest still runs its own device init and still
// writes PM4 packets, which is harmless with no GPU plugin loaded because the
// ring buffer those packets go into is never consumed. What this file does is
// track the API-level state a real backend needs -- bound shaders, shader
// constants, textures -- and check the device offsets that were recovered
// statically against what the running game actually does.
//
// Once the state here is trusted, a backend consumes it at draw time and the
// guest's packet writing becomes pure waste to be stubbed out.

#pragma once

#include <cstdint>

#include "native_renderer_state.h"

namespace eternalsonata {

// Guest device object layout. Every offset below is verified by decompilation;
// see the config's evidence fields for the per-function derivation.
namespace d3d {

// D3D__CreateDevice (0x82263568) allocates exactly this, 128 byte aligned.
inline constexpr uint32_t kDeviceSize = 22144;

// Float shader constant shadows, one vec4 per 16 bytes, 256 of each. The two
// are adjacent and the pair is bounded by the boolean constant shadow, which
// is an independent check on all three: 1792 + 256*16 == 5888, and
// 5888 + 256*16 == 9984.
inline constexpr uint32_t kVertexConstantShadow = 1792;
inline constexpr uint32_t kPixelConstantShadow = 5888;
inline constexpr uint32_t kBoolConstantShadow = 9984;
inline constexpr uint32_t kConstantRegisters = 256;

// The boolean and loop constant banks, which the emitted HLSL takes as one
// cbuffer (`uint4 xe_bools[2]; uint4 xe_loops[8];`). They are contiguous in the
// device and in that order, which is what lets a single copy serve both:
// 0x82265CB0 writes bool index i to 4*((i>>5)+2496), i.e. 9984, and 0x82265D70
// writes loop constant i to 4*(i+2504), i.e. 10016 == 9984 + 8*4. The vertex
// and pixel halves of each bank are the two halves of the same hardware
// register file (SQ_CF_BOOLEANS at 0x4900 is 8 dwords for 256 booleans), and
// the microcode's bool addresses are already global -- a pixel shader branches
// on b140 -- so no per-stage offset is applied here.
inline constexpr uint32_t kLoopConstantShadow = 10016;
inline constexpr uint32_t kBoolLoopConstantBytes = 8 * 4 + 32 * 4;

// Texture fetch constants, six dwords per sampler stage. SetTexture writes
// these inline and the three SetSamplerState_* setters patch fields inside
// them, which is why sampler state is not a register bank.
inline constexpr uint32_t kTextureFetchConstants = 1024;
inline constexpr uint32_t kTextureFetchStride = 24;
inline constexpr uint32_t kSamplerCount = 16;

// Bound objects, read by the draw time shader flush 0x82267E48.
inline constexpr uint32_t kVertexDeclaration = 11684;
inline constexpr uint32_t kBoundPixelShader = 12556;
inline constexpr uint32_t kBoundVertexShader = 12560;

// Predicated tiling, set up by BeginTiling (0x82261C20). This title renders
// 720p through EDRAM in horizontal bands, and this is the state that says so:
// the tile count, the per tile rectangles, and the extent that bounds them all.
//
// +13044 / +13048 are the maximum x2 and y2 over every tile, that is the **full
// logical render extent** (1280x720), as opposed to the EDRAM surface, which
// holds one band (1280x384). SetViewport (0x8225BA10) clamps to these rather
// than to the surface when tiling is active, which is the confirmation that
// they are screen space and not a band.
//
// The per tile origin at +12856/+12860 (each rounded down to 32) is what gets
// negated into PA_SC_WINDOW_OFFSET by 0x82258E28, shifting a band's geometry
// down into the EDRAM surface. None of that is reproduced here: the host renders
// the whole screen at once, so the window offset has nothing to offset into.
inline constexpr uint32_t kTileCount = 12612;
inline constexpr uint32_t kTilingExtentWidth = 13044;
inline constexpr uint32_t kTilingExtentHeight = 13048;

// Per stream fetch constant slots, a 16 byte array indexed by the declaration
// element's stream index. Stream 0 lands at vf95. Read by the vertex fetch
// patcher 0x82267218 and, together with the declaration serial, forms the
// variant cache key that 0x82267D08 probes with.
inline constexpr uint32_t kStreamFetchSlots = 12392;

// Vertex declaration object layout, from 0x82267218's own walk of it. The
// `type` field is not a D3DDECLTYPE enum index: on the 360 it is already the
// hardware fetch encoding, which is why the patcher only shifts it into place.
// The serial is assigned lazily by an lwarx/stwcx. increment of dword_82426750.
inline constexpr uint32_t kDeclSerial = 48;
inline constexpr uint32_t kDeclElementCount = 24;
inline constexpr uint32_t kDeclElements = 52;
inline constexpr uint32_t kDeclElementStride = 12;
inline constexpr uint32_t kDeclUsage = 9;
inline constexpr uint32_t kDeclUsageIndex = 10;

// Every D3D resource object (texture, vertex buffer, index buffer) carries its
// six GPU fetch constant dwords inline after a 28 byte header. Read off
// 0x82260C68, which takes its resolve destination as a texture object and
// pulls the destination format, base address and size straight out of
// destination+32, +36 and +28 without going through any accessor.
inline constexpr uint32_t kResourceFetchConstant = 28;

// Surface object layout, from the shared surface initialiser 0x8225DB30 that
// both CreateRenderTarget (0x8225E100) and the depth path run through. The
// object is 48 bytes and holds hardware register values, not API values, which
// is why the sizes below are stored with one subtracted and the format is a
// nibble: the guest is building RB_COLOR_INFO and friends at create time.
//
//   +24  (tile aligned width * 4 | msaa) << 16 | pitch, pitch in bits 0..13
//   +28  info word: EDRAM base tile in bits 0..11, format nibble at bit 16.
//        This is RB_COLOR_INFO for a colour surface and RB_DEPTH_INFO for a
//        depth one; 0x8225DB30 writes the base into the same field either way,
//        and the runtime confirms it (the 1280x720 pair comes back as colour at
//        tile 0 and depth at tile 720, adjacent and non overlapping).
//   +32  depth specific setup, hierarchical Z and stencil. Not a base tile:
//        it decodes as 1 for the depth surface, which is the low bit the
//        initialiser unconditionally ORs in.
//   +36  size: height-1 in bits 3..17, width-1 from bit 18
//   +40  the D3DFORMAT the surface was created with, unmodified
//   +44  the surface's EDRAM footprint in bytes, i.e. its tile count times 5120
//
// That last field is a size and not an offset, which is worth stating because
// it reads like one. CreateRenderTarget uses the same value as the count half
// of its own overflow check, `base + count > 0x800`, against the 2048 tiles of
// EDRAM; the base comes back separately from the allocator and is what lands in
// bits 0..11 of the info word.
inline constexpr uint32_t kSurfacePitchMsaa = 24;
inline constexpr uint32_t kSurfaceColorInfo = 28;
inline constexpr uint32_t kSurfaceDepthInfo = 32;
inline constexpr uint32_t kSurfaceSize = 36;
inline constexpr uint32_t kSurfaceFormat = 40;
inline constexpr uint32_t kSurfaceEdramBytes = 44;
inline constexpr uint32_t kEdramTileBytes = 5120;
inline constexpr uint32_t kEdramTiles = 2048;

// The bound surfaces, four colour targets and one depth stencil. SetRenderTarget
// (0x8225BDC0 behind the 0x8225C3C0 thunk) stores to 4*(index+3076), and the
// depth setter and Resolve both read +12320 for the depth slot.
inline constexpr uint32_t kColorSurfaces = 12304;
inline constexpr uint32_t kColorSurfaceCount = 4;
inline constexpr uint32_t kDepthSurface = 12320;

// The 256 entry shader tables the game binds out of, populated once at init by
// 0x82129260 from the static chunked blob at 0x8238EC50 in the xex. Entries are
// filled from index 1 upward, in the blob's own chunk order, which is what lets
// a bound shader object be resolved back to an offline extracted shader.
inline constexpr uint32_t kVertexShaderTable = 0x824BBEE8;
inline constexpr uint32_t kPixelShaderTable = 0x824BC2E8;
inline constexpr uint32_t kShaderTableEntries = 256;

}  // namespace d3d

// A decoded texture fetch constant. Field positions are xenia's
// xe_gpu_texture_fetch_t (../xenia-canary/src/xenia/gpu/xenos.h), which is
// reliable down to bit offsets and matches the field positions the three
// SetSamplerState_* setters patch.
struct TextureFetch {
  uint32_t type = 0;          // FetchConstantType; 2 is kTexture
  uint32_t format = 0;        // xenos::TextureFormat
  uint32_t endianness = 0;
  uint32_t base_address = 0;  // already shifted up by 12, and aperture-fixed
  uint32_t raw_base_address = 0;  // the same field before the fixup, for reads
  uint32_t width = 0;         // stored minus one, corrected here
  uint32_t height = 0;
  uint32_t pitch = 0;         // in pixels, stored >> 5, corrected here
  bool tiled = false;
  bool stacked = false;
};

// Decode sampler `stage`'s fetch constant out of the guest device.
TextureFetch DecodeTextureFetch(const uint32_t words[6]);

// Read sampler `stage`'s fetch constant out of the live device and decode it.
// False when the stage holds no texture. `base` is the guest memory base, which
// REX_LOAD_U32 needs in scope; the device's own guest address is taken from the
// mirror rather than passed in, because the pointer to hand at a draw is the
// *host* pointer to the device object and the two are not interchangeable.
// `sampler_out`, when given, receives the sampler half of the same six dwords,
// so a draw that wants both pays for one read rather than two. It is filled
// whenever the words were read at all, including when the fetch itself is not a
// usable texture, because the caller may still want to know.
bool GetBoundTextureFetch(uint8_t* base, uint32_t stage, TextureFetch& out,
                          GuestSamplerState* sampler_out = nullptr);

// A counter bumped whenever anything a texture binding is derived from changes:
// SetTexture, the three sampler setters, a resolve (which can replace the host
// image behind an address without the guest touching a fetch constant), and the
// frame boundary.
//
// The frame boundary is in there for correctness, not for tidiness. The texture
// mirror notices the guest rewriting a texture under an address it already
// holds by hashing the source once per texture per frame, so a draw path that
// skips the mirror on an unchanged generation must still visit it at least once
// per frame or the font atlas stops refreshing. That was a real bug; see the
// trap about sampled hashes in the handoff.
//
// The intended use is a cache of work derived from the fetch constants: hold
// the generation alongside the result and redo the work when it moves. It never
// wraps in practice and a wrap would only cost one stale frame anyway.
uint64_t TextureBindingGeneration();

// Guest address of the device object, or 0 before D3D__CreateDevice returns.
uint32_t D3DDeviceAddress();

// The full logical render extent BeginTiling last established, or 0x0 before it
// has ever run. This is what a host render target has to be sized to: the guest
// renders the whole screen through an EDRAM surface a fraction of its height and
// replays the command buffer once per band, which is a thing the host neither
// needs nor can do.
void D3DTilingExtent(uint32_t* width, uint32_t* height);

// One element of a vertex declaration. `type` is left raw deliberately: it is
// the hardware fetch encoding (data format in bits 0..5, signedness in 8..9,
// destination swizzle in 10..21), not something to be mapped through a
// D3DDECLTYPE table.
struct VertexElement {
  uint16_t stream = 0;
  uint16_t offset = 0;  // byte offset within the stream
  uint32_t type = 0;
  uint8_t usage = 0;        // D3DDECLUSAGE
  uint8_t usage_index = 0;
};

// The most elements any one declaration is expected to carry. The largest
// vertex input signature in the shader blob is vs_131 with 8, so this is
// generous; a declaration exceeding it is reported rather than truncated
// silently, because it would mean the layout above is misread.
inline constexpr uint32_t kMaxVertexElements = 24;

struct VertexDeclaration {
  uint32_t address = 0;
  uint32_t serial = 0;
  uint32_t element_count = 0;
  bool truncated = false;
  VertexElement elements[kMaxVertexElements];
};

// Decode the declaration object at `address` out of guest memory. Returns false
// if it does not look like a declaration, which is itself the check on the
// statically derived layout.
bool DecodeVertexDeclaration(uint8_t* base, uint32_t address, VertexDeclaration& out);

// A decoded surface object. Everything here comes back out of hardware register
// fields, so it is a readback of what the guest programmed rather than a copy
// of what it was asked for -- which is exactly what makes it a check on the
// layout above when compared against the CreateRenderTarget arguments.
struct Surface {
  uint32_t address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;         // tile aligned, in pixels
  uint32_t msaa = 0;          // D3DMULTISAMPLE_TYPE, 0..2
  uint32_t format = 0;        // the D3DFORMAT it was created with
  uint32_t hw_format = 0;    // the nibble programmed into the info word
  uint32_t edram_bytes = 0;  // footprint, not an offset
  uint32_t edram_tiles = 0;
  uint32_t base_tile = 0;    // where in EDRAM this surface starts
};

// Decode the surface object at `address`. Returns false when the result does
// not look like one, which is the check on the offsets rather than on the data.
bool DecodeSurface(uint8_t* base, uint32_t address, Surface& out);

// Decode a resource object's inline fetch constant, i.e. the texture itself
// rather than the sampler stage it happens to be bound to. Returns false when
// the fetch constant is not a texture.
bool DecodeResourceTexture(uint8_t* base, uint32_t address, TextureFetch& out);

// Guest address of the device object, or 0 before D3D__CreateDevice returns.
uint32_t D3DDeviceAddress();

// One line summary of what the mirror has seen so far. For the log and for
// eyeballing whether the offsets hold up on a real run.
void LogD3DMirrorSummary();

// What the draw path has seen: draw counts, primitive types, and the numbers
// that decide how big a statically compiled pipeline set has to be. See the
// comment above the draw hooks for why those numbers are the point.
void LogDrawMirrorSummary();

// What the surface path has seen: the render target set, the clear and resolve
// traffic, and how much of the sampled texture working set is produced by a
// resolve rather than loaded from disk. That last number is the one that says
// how much render-to-texture a backend has to model.
void LogSurfaceMirrorSummary();

}  // namespace eternalsonata
