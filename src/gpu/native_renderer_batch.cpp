// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_batch.h.

#include "native_renderer_batch.h"

#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native_renderer_d3d.h"
#include "native_renderer_draw.h"
#include "native_renderer_pipeline.h"
#include "native_renderer_pipeline_internal.h"

REXCVAR_DEFINE_BOOL(sprite_batching, true, "Eternal Sonata",
                    "Merge runs of effect sprites into one draw each instead of one draw per quad");

namespace eternalsonata {
namespace {

// The sprite path: vs_015 (world c0..c3, view projection c4..c7, UV offset
// c47) with ps_003 (texture times pixel c0), as a 4 vertex triangle strip of
// float3 position + float2 texcoord.
constexpr int kSpriteVertexSlot = 15;
constexpr int kSpritePixelSlot = 3;
constexpr uint32_t kSpriteStride = 20;
constexpr uint32_t kFormat32x2Float = 37;
constexpr uint32_t kFormat32x3Float = 57;
constexpr uint32_t kFormat32x4Float = 38;
constexpr uint32_t kUsagePosition = 0;
constexpr uint32_t kUsageTexcoord = 5;
constexpr uint32_t kUsageColor = 10;
constexpr uint32_t kTriangleStrip = 6;
constexpr uint32_t kTriangleList = 4;

// The merged vertex, in host order: position, texcoord, the sprite's colour
// (pixel c0, read by the batch pixel shader from interpolator 1) and the index
// of its texture among the batch's (TEXCOORD1, up to kBatchTextures). See the
// pair derived in scripts/gen-guest-shaders.py.
constexpr uint32_t kBatchStride = 44;
constexpr uint32_t kBatchColorOffset = 20;
constexpr uint32_t kBatchIndexOffset = 36;
constexpr uint32_t kBatchTextures = 8;
constexpr uint64_t kBatchDeclarationIdentity = 0x5350524954454241ull;

// A merged draw's vertices live in a ring that is never reused within a frame,
// because the per frame stream cache keys uploads on the source pointer. A
// frame's worth of sprites is well under a megabyte; the ring wraps only after
// tens of frames.
constexpr size_t kRingBytes = 32u << 20;
constexpr uint32_t kMaxSprites = 8192;

std::vector<uint8_t> g_ring;
size_t g_ring_offset = 0;

struct Batch {
  bool pending = false;
  GuestDrawCall call;       // the first sprite's call, reused for the merged draw
  PipelineRequest request;  // the first sprite's request, as a triangle list
  VertexDeclaration declaration;
  uint8_t projection[80];   // c4..c7 and c47, guest order
  uint8_t textures[kBatchTextures][d3d::kTextureFetchStride];  // fetch constants
  uint32_t texture_count = 0;
  uint32_t sprites = 0;
  uint8_t* data = nullptr;
};

Batch g_batch;
std::vector<uint8_t> g_vertex_bank;

// Counters for the swap summary.
uint64_t g_sprites_absorbed = 0;
uint64_t g_sprites_rejected = 0;  // the sprite pair, but not a shape this merges
uint64_t g_batches = 0;
uint64_t g_flush_draw = 0;     // a draw that could not join
uint64_t g_flush_barrier = 0;  // a target, viewport, clear, resolve or frame edge
uint64_t g_flush_full = 0;
uint64_t g_batches_no_shaders = 0;
// Why a sprite could not join the pending batch, first difference wins.
uint64_t g_miss_pipeline = 0, g_miss_projection = 0, g_miss_state = 0, g_miss_texture = 0;

uint32_t Swap32(uint32_t value) {
  return (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
}

float GuestFloat(const uint8_t* at) {
  uint32_t word;
  std::memcpy(&word, at, 4);
  word = Swap32(word);
  float value;
  std::memcpy(&value, &word, 4);
  return value;
}

void PutGuestFloat(uint8_t* at, float value) {
  uint32_t word;
  std::memcpy(&word, &value, 4);
  word = Swap32(word);
  std::memcpy(at, &word, 4);
}

bool SameRenderState(const GuestRenderState& a, const GuestRenderState& b) {
  // The raw registers the decoded fields come from, plus the per draw values
  // the pipeline does not key on (alpha test, param gen, point size).
  return a.depth_control == b.depth_control && a.stencil_ref_mask == b.stencil_ref_mask &&
         a.mode_cntl == b.mode_cntl && a.blend_control == b.blend_control &&
         a.color_mask == b.color_mask && a.color_control == b.color_control &&
         a.alpha_ref == b.alpha_ref && a.param_gen_enabled == b.param_gen_enabled &&
         a.param_gen_pos == b.param_gen_pos && a.point_diameter_x == b.point_diameter_x &&
         a.point_diameter_y == b.point_diameter_y &&
         a.point_diameter_min == b.point_diameter_min &&
         a.point_diameter_max == b.point_diameter_max;
}

bool SpriteShape(const GuestDrawCall& call) {
  if (call.indexed || call.primitive_type != kTriangleStrip || call.count != 4)
    return false;
  const GuestDrawStream& stream = call.streams[0];
  if (stream.data == nullptr || stream.stride != kSpriteStride || stream.size < 4 * kSpriteStride)
    return false;
  const VertexDeclaration* decl = call.declaration;
  if (decl == nullptr || decl->element_count != 2)
    return false;
  const VertexElement& position = decl->elements[0];
  const VertexElement& texcoord = decl->elements[1];
  return position.stream == 0 && position.offset == 0 &&
         (position.type & 0x3Fu) == kFormat32x3Float && texcoord.stream == 0 &&
         texcoord.offset == 12 && (texcoord.type & 0x3Fu) == kFormat32x2Float;
}

VertexDeclaration BatchDeclaration() {
  VertexDeclaration decl;
  decl.identity = kBatchDeclarationIdentity;
  decl.element_count = 4;
  decl.elements[0] = VertexElement{0, 0, kFormat32x3Float, kUsagePosition, 0};
  decl.elements[1] = VertexElement{0, 12, kFormat32x2Float, kUsageTexcoord, 0};
  decl.elements[2] = VertexElement{0, kBatchColorOffset, kFormat32x4Float, kUsageColor, 0};
  decl.elements[3] = VertexElement{0, kBatchIndexOffset, kFormat32x2Float, kUsageTexcoord, 1};
  return decl;
}

void CaptureProjection(const uint8_t* bank, uint8_t* out) {
  std::memcpy(out, bank + 4 * 16, 64);
  std::memcpy(out + 64, bank + 47 * 16, 16);
}

void Flush(uint64_t* reason) {
  if (!g_batch.pending)
    return;
  g_batch.pending = false;
  ++*reason;
  ++g_batches;

  Batch& batch = g_batch;
  const uint8_t* device = batch.call.device;

  // The vertex bank the merged draw reads: the device's current shadow with the
  // batch's own projection put back and the world matrix made the identity,
  // since the vertices already carry it.
  g_vertex_bank.resize(d3d::kConstantRegisters * 16);
  std::memcpy(g_vertex_bank.data(), device + d3d::kVertexConstantShadow, g_vertex_bank.size());
  std::memset(g_vertex_bank.data(), 0, 64);
  for (uint32_t row = 0; row < 4; ++row)
    PutGuestFloat(g_vertex_bank.data() + row * 16 + row * 4, 1.0f);
  std::memcpy(g_vertex_bank.data() + 4 * 16, batch.projection, 64);
  std::memcpy(g_vertex_bank.data() + 47 * 16, batch.projection + 64, 16);

  // The texture mirror reads fetch constants out of the device at draw time,
  // so the batch's textures go into slots 0..7 for the duration of the draw.
  // Unused slots repeat texture 0 rather than keep whatever the guest left
  // there, which the batch pixel shader declares and would otherwise decode.
  uint8_t* fetch = const_cast<uint8_t*>(device) + d3d::kTextureFetchConstants;
  uint8_t saved[kBatchTextures][d3d::kTextureFetchStride];
  std::memcpy(saved, fetch, sizeof(saved));
  for (uint32_t slot = 0; slot < kBatchTextures; ++slot) {
    const uint32_t source = slot < batch.texture_count ? slot : 0;
    std::memcpy(fetch + slot * d3d::kTextureFetchStride, batch.textures[source],
                d3d::kTextureFetchStride);
  }

  GuestDrawCall call = batch.call;
  call.pipeline = AcquireGuestPipeline(batch.request);
  call.primitive_type = kTriangleList;
  call.count = batch.sprites * 6;
  call.streams[0].data = batch.data;
  call.streams[0].size = call.count * kBatchStride;
  call.streams[0].stride = kBatchStride;
  call.vertex_bank_override = g_vertex_bank.data();
  call.host_order_streams = true;
  if (call.pipeline != nullptr)
    IssueGuestDraw(call);
  else
    ++g_batches_no_shaders;

  std::memcpy(fetch, saved, sizeof(saved));
}

}  // namespace

bool SpriteBatchAbsorb(const GuestDrawCall& call, const PipelineRequest& request) {
  if (!REXCVAR_GET(sprite_batching))
    return false;
  if (request.vertex_slot != kSpriteVertexSlot || request.pixel_slot != kSpritePixelSlot)
    return false;
  if (!SpriteShape(call) || GuestPipelineTextureMask(call.pipeline) != 1u) {
    ++g_sprites_rejected;
    return false;
  }

  const uint8_t* bank = call.device + d3d::kVertexConstantShadow;
  float world[4][4];
  for (uint32_t row = 0; row < 4; ++row)
    for (uint32_t col = 0; col < 4; ++col)
      world[row][col] = GuestFloat(bank + row * 16 + col * 4);
  // The vertex shader takes w from the float3 fetch, which is 1, so the
  // transformed position can only stand in when the matrix leaves w at 1.
  if (world[3][0] != 0.0f || world[3][1] != 0.0f || world[3][2] != 0.0f ||
      world[3][3] != 1.0f) {
    ++g_sprites_rejected;
    return false;
  }

  uint8_t projection[80];
  CaptureProjection(bank, projection);
  const uint8_t* colour = call.device + d3d::kPixelConstantShadow;  // pixel c0
  const uint8_t* fetch = call.device + d3d::kTextureFetchConstants;  // slot 0

  bool joins = g_batch.pending && g_batch.sprites < kMaxSprites;
  uint32_t texture = 0;
  if (joins) {
    if (g_batch.call.pipeline != call.pipeline) {
      joins = false;
      ++g_miss_pipeline;
    } else if (std::memcmp(g_batch.projection, projection, sizeof(projection)) != 0) {
      joins = false;
      ++g_miss_projection;
    } else if (!SameRenderState(g_batch.call.state, call.state)) {
      joins = false;
      ++g_miss_state;
    } else {
      while (texture < g_batch.texture_count &&
             std::memcmp(g_batch.textures[texture], fetch, d3d::kTextureFetchStride) != 0)
        ++texture;
      if (texture == g_batch.texture_count) {
        if (texture < kBatchTextures) {
          std::memcpy(g_batch.textures[g_batch.texture_count++], fetch,
                      d3d::kTextureFetchStride);
        } else {
          joins = false;
          ++g_miss_texture;
        }
      }
    }
  }

  if (!joins) {
    Flush(g_batch.pending && g_batch.sprites >= kMaxSprites ? &g_flush_full : &g_flush_draw);
    if (g_ring.empty())
      g_ring.resize(kRingBytes);
    if (g_ring_offset + size_t(kMaxSprites) * 6 * kBatchStride > g_ring.size())
      g_ring_offset = 0;

    g_batch.pending = true;
    g_batch.call = call;
    g_batch.request = request;
    g_batch.request.primitive_type = kTriangleList;
    g_batch.request.sprite_batch = true;
    g_batch.request.strides[0] = kBatchStride;
    g_batch.declaration = BatchDeclaration();
    g_batch.call.declaration = &g_batch.declaration;
    g_batch.request.declaration = &g_batch.declaration;
    std::memcpy(g_batch.projection, projection, sizeof(projection));
    std::memcpy(g_batch.textures[0], fetch, d3d::kTextureFetchStride);
    g_batch.texture_count = 1;
    texture = 0;
    g_batch.sprites = 0;
    g_batch.data = g_ring.data() + g_ring_offset;
  }

  float rgba[4];
  for (uint32_t c = 0; c < 4; ++c)
    rgba[c] = GuestFloat(colour + 4 * c);
  const float index[2] = {float(texture), 0.0f};

  // Six list vertices from the four strip ones, keeping the strip's winding.
  static constexpr uint32_t kOrder[6] = {0, 1, 2, 2, 1, 3};
  const uint8_t* source = call.streams[0].data;
  uint8_t* out = g_batch.data + size_t(g_batch.sprites) * 6 * kBatchStride;
  for (uint32_t v = 0; v < 6; ++v) {
    const uint8_t* in = source + kOrder[v] * kSpriteStride;
    const float p[3] = {GuestFloat(in), GuestFloat(in + 4), GuestFloat(in + 8)};
    const float uv[2] = {GuestFloat(in + 12), GuestFloat(in + 16)};
    float position[3];
    for (uint32_t axis = 0; axis < 3; ++axis) {
      // The shader's dot(position, c_axis), with the guest's column stored
      // per register: p'.x = dot(p, c0) and so on.
      position[axis] = world[axis][0] * p[0] + world[axis][1] * p[1] +
                       world[axis][2] * p[2] + world[axis][3];
    }
    uint8_t* vertex = out + v * kBatchStride;
    std::memcpy(vertex, position, sizeof(position));
    std::memcpy(vertex + 12, uv, sizeof(uv));
    std::memcpy(vertex + kBatchColorOffset, rgba, sizeof(rgba));
    std::memcpy(vertex + kBatchIndexOffset, index, sizeof(index));
  }
  ++g_batch.sprites;
  ++g_sprites_absorbed;
  g_ring_offset =
      size_t(g_batch.data - g_ring.data()) + size_t(g_batch.sprites) * 6 * kBatchStride;
  return true;
}

void SpriteBatchFlush() { Flush(&g_flush_barrier); }

void SpriteBatchEndFrame() { Flush(&g_flush_barrier); }

void LogSpriteBatchSummary() {
  REXLOG_DEBUG(
      "native_renderer: sprite batching: {} sprites into {} draws, {} rejected | flushed by "
      "draw {} barrier {} full {} | split by pipeline {} projection {} state {} texture {} | "
      "batches without shaders {}",
      g_sprites_absorbed, g_batches, g_sprites_rejected, g_flush_draw, g_flush_barrier,
      g_flush_full, g_miss_pipeline, g_miss_projection, g_miss_state, g_miss_texture,
      g_batches_no_shaders);
}

}  // namespace eternalsonata
