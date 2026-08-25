// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_draw.h.

#include "native_renderer_draw.h"

#include <cstring>
#include <memory>
#include <vector>

#include <rex/logging.h>

#include "native_renderer_frame.h"
#include "native_renderer_pipeline_internal.h"
#include "native_renderer_plume_internal.h"
#include "native_renderer_texture.h"

namespace eternalsonata {
namespace {

using namespace plume;

// Root constant buffer views have to be 256 byte aligned on D3D12, and nothing
// else here needs a coarser alignment, so one number covers every allocation.
constexpr uint32_t kUploadAlignment = 256;

// One arena block. Big enough that a frame of this title's traffic takes a
// handful, small enough that the first one is not a surprise: the opening
// screens run a few hundred draws a frame, each of which uploads its constants.
constexpr uint64_t kArenaBlockBytes = 8ull * 1024 * 1024;

// A refusal to upload a vertex buffer larger than this is a bug report, not a
// draw: the largest thing the guest binds is a mesh, and a stream that claims
// to be bigger than this has had its size field misread.
constexpr uint32_t kMaxStreamBytes = 16u * 1024 * 1024;

uint32_t Swap32(uint32_t value) {
  return (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
}
uint16_t Swap16(uint16_t value) { return uint16_t((value >> 8) | (value << 8)); }

uint32_t LoadDeviceU32(const uint8_t* device, uint32_t offset) {
  uint32_t raw;
  std::memcpy(&raw, device + offset, sizeof(raw));
  return Swap32(raw);
}

// Every reason a draw is dropped, counted and reported once. A dropped draw is
// missing geometry, so this is the first thing to read when the frame looks
// incomplete.
enum DropReason {
  kDropNoPipeline,
  kDropDeviceDown,
  kDropNoCommands,
  kDropNoTarget,
  kDropNoArena,
  kDropStreamMissing,
  kDropStreamTooBig,
  kDropVertexFormat,
  kDropIndicesMissing,
  kDropCount,
};
constexpr const char* kDropNames[kDropCount] = {
    "no host pipeline (see the pipeline summary for why)",
    "the backend is not up",
    "no command list to record into",
    "the guest has no render target bound",
    "the upload arena could not grow",
    "a declaration element names a stream the guest has not bound",
    "a bound stream claims an implausible size",
    "a declaration element is in a format the upload path cannot byte swap",
    "an indexed draw with no index buffer bound",
};
uint64_t g_drops[kDropCount] = {};
bool g_drop_reported[kDropCount] = {};

void Drop(DropReason reason, const char* detail) {
  ++g_drops[reason];
  if (g_drop_reported[reason])
    return;
  g_drop_reported[reason] = true;
  REXLOG_WARN("native_renderer: draw dropped, {}: {}", kDropNames[reason], detail);
}

uint64_t g_draws_issued = 0;
uint64_t g_draws_requested = 0;
uint64_t g_vertex_bytes = 0;
uint64_t g_index_bytes = 0;
uint64_t g_constant_bytes = 0;
uint64_t g_stream_cache_hits = 0;
uint64_t g_rect_draws = 0;
uint64_t g_rect_fallbacks = 0;
bool g_rect_fallback_reported = false;

// ---------------------------------------------------------------------------
// The upload arena.
//
// Single buffered on purpose, and safe only because the backend waits on its
// fence after every present: by the time the arena is reset, the frame that
// read those bytes has retired. This is the first thing that has to change when
// there are frames in flight.

struct ArenaBlock {
  std::unique_ptr<RenderBuffer> buffer;
  uint8_t* mapped = nullptr;
  uint64_t capacity = 0;
  uint64_t used = 0;
};

std::vector<ArenaBlock> g_arena;
uint32_t g_arena_block = 0;

struct Allocation {
  RenderBufferReference ref;
  uint8_t* cpu = nullptr;
  explicit operator bool() const { return cpu != nullptr; }
};

Allocation ArenaAllocate(RenderDevice* device, uint64_t bytes) {
  const uint64_t aligned = (bytes + kUploadAlignment - 1) / kUploadAlignment * kUploadAlignment;

  for (; g_arena_block < g_arena.size(); ++g_arena_block) {
    ArenaBlock& block = g_arena[g_arena_block];
    if (block.used + aligned <= block.capacity) {
      Allocation allocation;
      allocation.ref = block.buffer->at(block.used);
      allocation.cpu = block.mapped + block.used;
      block.used += aligned;
      return allocation;
    }
  }

  ArenaBlock block;
  block.capacity = aligned > kArenaBlockBytes ? aligned : kArenaBlockBytes;
  // One buffer serving as vertex, index and constant source. An upload heap
  // resource on D3D12 carries no usage restriction at all; Vulkan does, which
  // is why all three flags are declared rather than the one a given allocation
  // happens to be used for.
  block.buffer = device->createBuffer(RenderBufferDesc::UploadBuffer(
      block.capacity,
      RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT));
  if (!block.buffer)
    return {};
  block.mapped = static_cast<uint8_t*>(block.buffer->map());
  if (block.mapped == nullptr)
    return {};

  g_arena.push_back(std::move(block));
  g_arena_block = uint32_t(g_arena.size()) - 1;

  ArenaBlock& created = g_arena.back();
  Allocation allocation;
  allocation.ref = created.buffer->at(0);
  allocation.cpu = created.mapped;
  created.used = aligned;
  return allocation;
}

// ---------------------------------------------------------------------------
// Shared resources: the placeholder texture set and the zero stream.

std::unique_ptr<RenderTexture> g_white_texture;
std::unique_ptr<RenderSampler> g_sampler;
std::unique_ptr<RenderDescriptorSet> g_texture_set;
std::unique_ptr<RenderBuffer> g_null_stream;
bool g_resources_ready = false;
bool g_resources_failed = false;

bool EnsureResources(RenderDevice* device) {
  if (g_resources_ready)
    return true;
  if (g_resources_failed)
    return false;

  // A texture mirror does not exist yet, so every one of the sixteen slots gets
  // one opaque white texel. That is not a placeholder in the "wrong pixels"
  // sense: a shader that multiplies by a sampled texture reads 1.0, so the
  // geometry appears in its untextured colour rather than not at all, and the
  // difference between that and a real texture is obvious on sight.
  g_white_texture =
      device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  if (!g_white_texture) {
    g_resources_failed = true;
    return false;
  }

  const uint32_t white = 0xFFFFFFFFu;
  auto staging = device->createBuffer(RenderBufferDesc::UploadBuffer(kUploadAlignment));
  if (staging) {
    if (void* mapped = staging->map()) {
      std::memcpy(mapped, &white, sizeof(white));
      staging->unmap();
    }
    // A one-shot list, for the same reason the overlay's uploads use one: this
    // runs from inside the frame's own recording.
    RenderCommandQueue* queue = PlumeQueue();
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    commands->begin();
    commands->barriers(RenderBarrierStage::COPY,
                       RenderTextureBarrier(g_white_texture.get(), RenderTextureLayout::COPY_DEST));
    commands->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g_white_texture.get()),
        RenderTextureCopyLocation::PlacedFootprint(staging.get(), RenderFormat::R8G8B8A8_UNORM, 1,
                                                   1, 1, kUploadAlignment / 4));
    commands->barriers(RenderBarrierStage::GRAPHICS,
                       RenderTextureBarrier(g_white_texture.get(), RenderTextureLayout::SHADER_READ));
    commands->end();
    const RenderCommandList* submit = commands.get();
    queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());
  }

  // One sampler for every slot. The guest's real sampler state lives in the
  // texture fetch constants (see item 4 of the handoff), which the texture
  // mirror will read; until then linear and wrapping is the least surprising
  // default.
  RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = RenderFilter::LINEAR;
  sampler_desc.magFilter = RenderFilter::LINEAR;
  sampler_desc.mipmapMode = RenderMipmapMode::LINEAR;
  sampler_desc.addressU = RenderTextureAddressMode::WRAP;
  sampler_desc.addressV = RenderTextureAddressMode::WRAP;
  sampler_desc.addressW = RenderTextureAddressMode::WRAP;
  g_sampler = device->createSampler(sampler_desc);

  // The same shape the pipeline layout declares for set 0, because it has to be
  // the same shape: t0..t15 then s0..s15, and Plume numbers the descriptors
  // flat across the ranges in order, so the samplers start at kTextureSlots.
  const RenderDescriptorRange ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, kTextureSlots),
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 0, kTextureSlots),
  };
  g_texture_set = device->createDescriptorSet(RenderDescriptorSetDesc(ranges, 2));
  if (!g_sampler || !g_texture_set) {
    g_resources_failed = true;
    return false;
  }
  for (uint32_t i = 0; i < kTextureSlots; ++i) {
    g_texture_set->setTexture(i, g_white_texture.get(), RenderTextureLayout::SHADER_READ);
    g_texture_set->setSampler(kTextureSlots + i, g_sampler.get());
  }

  // The missing-attribute stream: a vertex shader input the declaration has no
  // element for. The hardware synthesises a constant; a zero buffer with a
  // three component format is the same thing, since the fourth component of an
  // absent input reads as 1.0 either way.
  g_null_stream = device->createBuffer(
      RenderBufferDesc::UploadBuffer(kUploadAlignment, RenderBufferFlag::VERTEX));
  if (!g_null_stream) {
    g_resources_failed = true;
    return false;
  }
  if (void* mapped = g_null_stream->map()) {
    std::memset(mapped, 0, kUploadAlignment);
    g_null_stream->unmap();
  }

  g_resources_ready = true;
  REXLOG_INFO("native_renderer: guest draw path up, with the texture mirror behind it");
  return true;
}

// ---------------------------------------------------------------------------
// Byte swapping, which is where the console's endianness is paid for.

// Components and their width for the vertex formats the pipeline cache accepts.
// The width is what decides the swap: a stream's fetch constant carries one
// endian mode, and what that mode means is "swap each component", which is why
// a packed format like k_2_10_10_10 swaps as one 32 bit word and k_16_16 as two
// 16 bit ones rather than as a single dword.
bool VertexFormatSwap(uint32_t type, uint32_t* component_bytes, uint32_t* component_count) {
  switch (type & 0x3Fu) {
    case 6:   // k_8_8_8_8, one packed word
    case 7:   // k_2_10_10_10
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10
    case 33:  // k_32
    case 36:  // k_32_FLOAT
      *component_bytes = 4;
      *component_count = 1;
      return true;
    case 25:  // k_16_16
    case 31:  // k_16_16_FLOAT
      *component_bytes = 2;
      *component_count = 2;
      return true;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
      *component_bytes = 2;
      *component_count = 4;
      return true;
    case 34:  // k_32_32
    case 37:  // k_32_32_FLOAT
      *component_bytes = 4;
      *component_count = 2;
      return true;
    case 57:  // k_32_32_32_FLOAT
      *component_bytes = 4;
      *component_count = 3;
      return true;
    case 35:  // k_32_32_32_32
    case 38:  // k_32_32_32_32_FLOAT
      *component_bytes = 4;
      *component_count = 4;
      return true;
    default:
      return false;
  }
}

bool VertexFormatIsFloat(uint32_t type) {
  switch (type & 0x3Fu) {
    case 36:
    case 37:
    case 38:
    case 57:
      return true;
    default:
      return false;
  }
}

// One element of the vertex, reduced to what the copy needs.
struct SwapField {
  uint32_t offset = 0;
  uint32_t component_bytes = 0;
  uint32_t component_count = 0;
};

struct SwapPlan {
  SwapField fields[kMaxVertexElements];
  uint32_t count = 0;
  int32_t position_field = -1;  // index into `fields`, if POSITION0 is float
};

bool BuildSwapPlan(const VertexDeclaration& decl, uint32_t stream, SwapPlan* plan) {
  const uint32_t element_count =
      decl.element_count < kMaxVertexElements ? decl.element_count : kMaxVertexElements;
  for (uint32_t i = 0; i < element_count; ++i) {
    const VertexElement& element = decl.elements[i];
    if (element.stream != stream)
      continue;
    SwapField field;
    field.offset = element.offset;
    if (!VertexFormatSwap(element.type, &field.component_bytes, &field.component_count))
      return false;
    if (element.usage == 0 && element.usage_index == 0 && VertexFormatIsFloat(element.type))
      plan->position_field = int32_t(plan->count);
    plan->fields[plan->count++] = field;
    if (plan->count == kMaxVertexElements)
      break;
  }
  return true;
}

void SwapVertex(uint8_t* vertex, const SwapPlan& plan) {
  for (uint32_t f = 0; f < plan.count; ++f) {
    const SwapField& field = plan.fields[f];
    uint8_t* at = vertex + field.offset;
    if (field.component_bytes == 4) {
      for (uint32_t c = 0; c < field.component_count; ++c) {
        uint32_t value;
        std::memcpy(&value, at + 4 * c, 4);
        value = Swap32(value);
        std::memcpy(at + 4 * c, &value, 4);
      }
    } else {
      for (uint32_t c = 0; c < field.component_count; ++c) {
        uint16_t value;
        std::memcpy(&value, at + 2 * c, 2);
        value = Swap16(value);
        std::memcpy(at + 2 * c, &value, 2);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// RECTANGLE_LIST, the 360's three vertex quad.
//
// The fourth corner is implied: it is the mirror of the vertex opposite the
// longest edge, across that edge. Which of the three edges is the diagonal is
// not fixed, so it is measured, the same way xenia's geometry shader does it;
// when the position is not a float format there is nothing to measure with and
// the common case (the 1-2 edge) is assumed.
//
// Attributes of the synthesised vertex follow the same rule as the position for
// anything float, and are copied from a corner of the diagonal otherwise. A
// packed colour cannot be averaged without unpacking it, and UI quads are flat
// coloured in practice.

void SynthesiseRectVertex(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out,
                          uint32_t stride, const SwapPlan& plan, const VertexDeclaration& decl,
                          uint32_t stream) {
  // out = b + c - a, per float component; everything else comes from b.
  std::memcpy(out, b, stride);

  const uint32_t element_count =
      decl.element_count < kMaxVertexElements ? decl.element_count : kMaxVertexElements;
  uint32_t field = 0;
  for (uint32_t i = 0; i < element_count && field < plan.count; ++i) {
    const VertexElement& element = decl.elements[i];
    if (element.stream != stream)
      continue;
    const SwapField& swap = plan.fields[field++];
    if (!VertexFormatIsFloat(element.type))
      continue;
    for (uint32_t component = 0; component < swap.component_count; ++component) {
      float va, vb, vc;
      std::memcpy(&va, a + swap.offset + 4 * component, 4);
      std::memcpy(&vb, b + swap.offset + 4 * component, 4);
      std::memcpy(&vc, c + swap.offset + 4 * component, 4);
      const float value = vb + vc - va;
      std::memcpy(out + swap.offset + 4 * component, &value, 4);
    }
  }
}

// Which pair of the three vertices spans the diagonal. Returns the two indices
// on it; the third is the one to mirror.
void RectDiagonal(const uint8_t* vertices, uint32_t stride, const SwapPlan& plan, uint32_t* first,
                  uint32_t* second, uint32_t* opposite) {
  *first = 1;
  *second = 2;
  *opposite = 0;
  if (plan.position_field < 0)
    return;

  const SwapField& position = plan.fields[plan.position_field];
  float xyz[3][3] = {};
  for (uint32_t v = 0; v < 3; ++v) {
    for (uint32_t c = 0; c < 3 && c < position.component_count; ++c)
      std::memcpy(&xyz[v][c], vertices + v * stride + position.offset + 4 * c, 4);
  }

  auto distance = [&](uint32_t i, uint32_t j) {
    float sum = 0.0f;
    for (uint32_t c = 0; c < 3; ++c) {
      const float d = xyz[i][c] - xyz[j][c];
      sum += d * d;
    }
    return sum;
  };

  const float d12 = distance(1, 2);
  const float d20 = distance(2, 0);
  const float d01 = distance(0, 1);
  if (d20 > d12 && d20 >= d01) {
    *first = 2;
    *second = 0;
    *opposite = 1;
  } else if (d01 > d12 && d01 > d20) {
    *first = 0;
    *second = 1;
    *opposite = 2;
  }
}

// ---------------------------------------------------------------------------
// Stream and constant upload, both with a "has this changed" check in front,
// because the same buffer is bound across a run of draws and the constant banks
// change far less often than they are read.

struct StreamCacheEntry {
  const uint8_t* source = nullptr;
  uint32_t bytes = 0;
  uint32_t stride = 0;
  uint32_t primitive = 0;
  RenderBufferReference ref;
  uint32_t size = 0;
};
std::vector<StreamCacheEntry> g_stream_cache;

struct ConstantCacheEntry {
  std::vector<uint8_t> raw;  // the guest bytes, unswapped, as last uploaded
  RenderBufferReference ref;
  bool valid = false;
};
ConstantCacheEntry g_constant_cache[3];

// Copy a constant bank out of the device, swapping, and reuse the previous
// upload when the guest bytes have not changed. The comparison is against the
// raw guest memory rather than against our own idea of what was set, so a write
// that did not come through the two constant setters cannot go unnoticed.
bool UploadConstantBank(RenderDevice* device, const uint8_t* source, uint32_t bytes,
                        ConstantCacheEntry& cache, RenderBufferReference* out) {
  if (cache.valid && cache.raw.size() == bytes &&
      std::memcmp(cache.raw.data(), source, bytes) == 0) {
    *out = cache.ref;
    return true;
  }

  const Allocation allocation = ArenaAllocate(device, bytes);
  if (!allocation)
    return false;

  const uint32_t dwords = bytes / 4;
  const uint8_t* in = source;
  uint8_t* out_bytes = allocation.cpu;
  for (uint32_t i = 0; i < dwords; ++i) {
    uint32_t value;
    std::memcpy(&value, in + 4 * i, 4);
    value = Swap32(value);
    std::memcpy(out_bytes + 4 * i, &value, 4);
  }

  cache.raw.assign(source, source + bytes);
  cache.ref = allocation.ref;
  cache.valid = true;
  g_constant_bytes += bytes;
  *out = allocation.ref;
  return true;
}

// The guest's last viewport, as SetViewport passed it.
struct Viewport {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float min_z = 0.0f, max_z = 1.0f;
  bool set = false;
};
Viewport g_viewport;

}  // namespace

void DrawSetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float min_z,
                     float max_z) {
  g_viewport = {x, y, width, height, min_z, max_z, true};
}

bool IssueGuestDraw(const GuestDrawCall& call) {
  ++g_draws_requested;

  if (call.pipeline == nullptr) {
    Drop(kDropNoPipeline, "the pipeline cache refused this draw");
    return false;
  }
  if (call.declaration == nullptr || call.count == 0)
    return false;

  RenderDevice* device = PlumeDevice();
  if (device == nullptr) {
    Drop(kDropDeviceDown, "the guest is drawing before the backend came up");
    return false;
  }
  if (!EnsureResources(device))
    return false;

  RenderCommandList* commands = PlumeGuestCommands();
  if (commands == nullptr) {
    Drop(kDropNoCommands, "the backend is up but handed back no list");
    return false;
  }

  uint32_t target_width = 0;
  uint32_t target_height = 0;
  if (FrameBindDrawTargets(commands, &target_width, &target_height) == nullptr) {
    Drop(kDropNoTarget, "no colour or depth surface is bound, so there is nowhere to draw");
    return false;
  }

  const bool rect_list = call.primitive_type == 8;

  // The vertex streams. Each host slot is filled from the guest stream the
  // pipeline recorded for it, or from the zero buffer for the missing-attribute
  // slot.
  uint32_t slot_count = 0;
  const RenderInputSlot* slots = GuestPipelineInputSlots(call.pipeline, &slot_count);
  const uint32_t* slot_streams = GuestPipelineSlotStreams(call.pipeline, nullptr);

  RenderVertexBufferView views[kMaxPipelineStreams + 1];
  uint32_t draw_count = call.count;
  bool expanded = false;

  for (uint32_t slot = 0; slot < slot_count; ++slot) {
    const uint32_t stream_index = slot_streams[slot];
    if (stream_index == kNullInputSlot) {
      views[slot] = RenderVertexBufferView(g_null_stream->at(0), kUploadAlignment);
      continue;
    }

    const GuestDrawStream& stream = call.streams[stream_index];
    if (stream.data == nullptr || stream.stride == 0) {
      Drop(kDropStreamMissing, "the pipeline reads a stream SetStreamSource never bound");
      return false;
    }
    if (stream.size > kMaxStreamBytes) {
      Drop(kDropStreamTooBig, "the resource size field at +28 is not what it is believed to be");
      return false;
    }

    SwapPlan plan;
    if (!BuildSwapPlan(*call.declaration, stream_index, &plan)) {
      Drop(kDropVertexFormat, "the pipeline accepted a format the swap table does not know");
      return false;
    }

    // An indexed draw indexes the whole bound buffer, so the whole bound buffer
    // is what has to be there; a non-indexed one reads exactly what it draws.
    // The rectangle expansion turns each triple into two triangles, which is
    // where the vertex count changes.
    const bool expand = rect_list && !call.indexed && slot_count == 1 && call.count >= 3;
    uint32_t source_vertices = call.indexed ? stream.size / stream.stride : call.count;
    if (source_vertices == 0)
      continue;
    if (!call.indexed && source_vertices * stream.stride > stream.size)
      source_vertices = stream.size / stream.stride;

    const uint32_t triples = expand ? source_vertices / 3 : 0;
    const uint32_t out_vertices = expand ? triples * 6 : source_vertices;
    const uint64_t out_bytes = uint64_t(out_vertices) * stream.stride;
    if (out_bytes == 0)
      continue;

    // Reuse the upload when the same bytes were already uploaded this frame.
    // A run of draws out of one mesh buffer is the common shape here, and the
    // swap is the expensive part.
    StreamCacheEntry* cached = nullptr;
    for (auto& entry : g_stream_cache) {
      if (entry.source == stream.data && entry.bytes == uint32_t(out_bytes) &&
          entry.stride == stream.stride && entry.primitive == (expand ? 8u : 0u)) {
        cached = &entry;
        break;
      }
    }

    if (cached != nullptr) {
      ++g_stream_cache_hits;
      views[slot] = RenderVertexBufferView(cached->ref, cached->size);
    } else {
      const Allocation allocation = ArenaAllocate(device, out_bytes);
      if (!allocation) {
        Drop(kDropNoArena, "an upload buffer could not be created");
        return false;
      }

      if (expand) {
        uint8_t scratch[4][256];
        const uint32_t stride = stream.stride;
        const bool small_enough = stride <= sizeof(scratch[0]);
        for (uint32_t triple = 0; triple < triples && small_enough; ++triple) {
          const uint8_t* source = stream.data + size_t(triple) * 3 * stride;
          for (uint32_t v = 0; v < 3; ++v) {
            std::memcpy(scratch[v], source + size_t(v) * stride, stride);
            SwapVertex(scratch[v], plan);
          }
          uint32_t first = 1, second = 2, opposite = 0;
          RectDiagonal(&scratch[0][0], sizeof(scratch[0]), plan, &first, &second, &opposite);
          SynthesiseRectVertex(scratch[opposite], scratch[first], scratch[second], scratch[3],
                               stride, plan, *call.declaration, stream_index);

          uint8_t* out = allocation.cpu + size_t(triple) * 6 * stride;
          const uint32_t order[6] = {0, 1, 2, first, second, 3};
          for (uint32_t v = 0; v < 6; ++v)
            std::memcpy(out + size_t(v) * stride, scratch[order[v]], stride);
        }
        if (!small_enough) {
          // A rect list vertex wider than the scratch. Nothing in this title has
          // one, and falling back to the unexpanded triples keeps the draw
          // rather than losing it.
          ++g_rect_fallbacks;
          std::memcpy(allocation.cpu, stream.data, size_t(triples) * 3 * stride);
          for (uint32_t v = 0; v < triples * 3; ++v)
            SwapVertex(allocation.cpu + size_t(v) * stride, plan);
        }
      } else {
        std::memcpy(allocation.cpu, stream.data, size_t(out_bytes));
        for (uint32_t v = 0; v < out_vertices; ++v)
          SwapVertex(allocation.cpu + size_t(v) * stream.stride, plan);
      }

      g_vertex_bytes += out_bytes;
      views[slot] = RenderVertexBufferView(allocation.ref, uint32_t(out_bytes));

      StreamCacheEntry entry;
      entry.source = stream.data;
      entry.bytes = uint32_t(out_bytes);
      entry.stride = stream.stride;
      entry.primitive = expand ? 8u : 0u;
      entry.ref = allocation.ref;
      entry.size = uint32_t(out_bytes);
      g_stream_cache.push_back(entry);
    }

    if (expand) {
      draw_count = out_vertices;
      expanded = true;
    }
  }

  if (rect_list) {
    ++g_rect_draws;
    if (!expanded && !g_rect_fallback_reported) {
      g_rect_fallback_reported = true;
      REXLOG_WARN(
          "native_renderer: a RECTANGLE_LIST draw could not be expanded (indexed, or across more "
          "than one stream), so each quad is drawn as the one triangle the guest supplied");
    }
  }

  // Indices.
  RenderIndexBufferView index_view;
  if (call.indexed) {
    if (call.indices == nullptr) {
      Drop(kDropIndicesMissing, "SetIndices was never called, or its buffer decodes as null");
      return false;
    }
    const uint32_t index_bytes = call.count * (call.index_32bit ? 4u : 2u);
    const Allocation allocation = ArenaAllocate(device, index_bytes);
    if (!allocation) {
      Drop(kDropNoArena, "an index upload could not be allocated");
      return false;
    }
    if (call.index_32bit) {
      for (uint32_t i = 0; i < call.count; ++i) {
        uint32_t value;
        std::memcpy(&value, call.indices + 4 * i, 4);
        value = Swap32(value);
        std::memcpy(allocation.cpu + 4 * i, &value, 4);
      }
    } else {
      for (uint32_t i = 0; i < call.count; ++i) {
        uint16_t value;
        std::memcpy(&value, call.indices + 2 * i, 2);
        value = Swap16(value);
        std::memcpy(allocation.cpu + 2 * i, &value, 2);
      }
    }
    g_index_bytes += index_bytes;
    index_view = RenderIndexBufferView(allocation.ref, index_bytes,
                                       call.index_32bit ? RenderFormat::R32_UINT
                                                        : RenderFormat::R16_UINT);
  }

  // The constant banks. Four root descriptors in the order the layout declares
  // them, with the bool and loop bank shared by both stages because the guest's
  // is one register file with the vertex half first.
  RenderBufferReference vertex_floats, pixel_floats, bool_loops;
  const bool constants_ok =
      UploadConstantBank(device, call.device + d3d::kVertexConstantShadow,
                         d3d::kConstantRegisters * 16, g_constant_cache[0], &vertex_floats) &&
      UploadConstantBank(device, call.device + d3d::kPixelConstantShadow,
                         d3d::kConstantRegisters * 16, g_constant_cache[1], &pixel_floats) &&
      UploadConstantBank(device, call.device + d3d::kBoolConstantShadow,
                         d3d::kBoolLoopConstantBytes, g_constant_cache[2], &bool_loops);
  if (!constants_ok) {
    Drop(kDropNoArena, "a constant bank could not be uploaded");
    return false;
  }

  // The viewport, clamped to the target. The guest renders 720p through EDRAM
  // in bands, so its viewport can be taller than the surface it is bound to;
  // the band's own origin is what the resolve carries, not what this does.
  float x = 0.0f, y = 0.0f;
  float width = float(target_width), height = float(target_height);
  if (g_viewport.set) {
    x = float(g_viewport.x);
    y = float(g_viewport.y);
    width = float(g_viewport.width);
    height = float(g_viewport.height);
    if (x + width > float(target_width))
      width = float(target_width) - x;
    if (y + height > float(target_height))
      height = float(target_height) - y;
  }
  if (width <= 0.0f || height <= 0.0f) {
    x = y = 0.0f;
    width = float(target_width);
    height = float(target_height);
  }
  commands->setViewports(RenderViewport(x, y, width, height, g_viewport.min_z, g_viewport.max_z));
  commands->setScissors(
      RenderRect(int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height)));

  commands->setGraphicsPipelineLayout(GuestPipelineLayout());
  commands->setPipeline(GuestPipelineObject(call.pipeline));
  commands->setGraphicsRootDescriptor(vertex_floats, kRootVertexFloatConstants);
  commands->setGraphicsRootDescriptor(bool_loops, kRootVertexBoolConstants);
  commands->setGraphicsRootDescriptor(pixel_floats, kRootPixelFloatConstants);
  commands->setGraphicsRootDescriptor(bool_loops, kRootPixelBoolConstants);

  // The texture mirror.
  //
  // Only the slots the bound pixel shader declares are decoded. The other
  // stages still hold whatever fetch constant was last written there, and the
  // guest never fetches from them precisely because its shader does not name
  // them; treating those as textures means decoding a resource that may have
  // been freed, which is a read through an unmapped address rather than a
  // wrong picture.
  //
  // Every slot is still *written*, declared or not, because the descriptor set
  // is shared across draws and a texture left in a slot by the previous one
  // would otherwise stay bound. An undeclared slot, and one whose texture the
  // mirror could not produce, gets the white placeholder, so a draw the mirror
  // cannot serve appears in flat colour rather than not at all.
  const uint32_t texture_mask = GuestPipelineTextureMask(call.pipeline);
  for (uint32_t stage = 0; stage < kTextureSlots; ++stage) {
    RenderTexture* texture = nullptr;
    TextureFetch fetch;
    if ((texture_mask & (1u << stage)) != 0 &&
        GetBoundTextureFetch(call.memory_base, stage, fetch))
      texture = static_cast<RenderTexture*>(TextureMirrorLookup(call.memory_base, fetch));
    g_texture_set->setTexture(stage, texture != nullptr ? texture : g_white_texture.get(),
                              RenderTextureLayout::SHADER_READ);
  }

  commands->setGraphicsDescriptorSet(g_texture_set.get(), kTextureDescriptorSet);
  commands->setVertexBuffers(0, views, slot_count, slots);

  if (call.indexed) {
    commands->setIndexBuffer(&index_view);
    commands->drawIndexedInstanced(call.count, 1, 0, call.base_vertex, 0);
  } else {
    commands->drawInstanced(draw_count, 1, 0, 0);
  }

  ++g_draws_issued;
  return true;
}

void ResetGuestDrawArena() {
  for (ArenaBlock& block : g_arena)
    block.used = 0;
  g_arena_block = 0;
  g_stream_cache.clear();
  for (ConstantCacheEntry& cache : g_constant_cache)
    cache.valid = false;
}

void LogGuestDrawSummary() {
  uint64_t dropped = 0;
  for (uint64_t count : g_drops)
    dropped += count;

  REXLOG_INFO(
      "native_renderer: draws issued={} requested={} dropped={} | uploaded {} KiB vertices, {} "
      "KiB indices, {} KiB constants | stream cache hits={} | arena {} block(s) | rect lists={} "
      "(unexpanded {})",
      g_draws_issued, g_draws_requested, dropped, g_vertex_bytes / 1024, g_index_bytes / 1024,
      g_constant_bytes / 1024, g_stream_cache_hits, g_arena.size(), g_rect_draws,
      g_rect_fallbacks);

  for (uint32_t i = 0; i < kDropCount; ++i) {
    if (g_drops[i] != 0)
      REXLOG_INFO("native_renderer:   dropped {}x: {}", g_drops[i], kDropNames[i]);
  }
}

void ShutdownGuestDraws() {
  for (ArenaBlock& block : g_arena) {
    if (block.mapped != nullptr)
      block.buffer->unmap();
  }
  g_arena.clear();
  g_arena_block = 0;
  g_stream_cache.clear();
  for (ConstantCacheEntry& cache : g_constant_cache) {
    cache.valid = false;
    cache.raw.clear();
  }
  g_texture_set.reset();
  g_sampler.reset();
  g_white_texture.reset();
  g_null_stream.reset();
  g_resources_ready = false;
  g_resources_failed = false;
}

}  // namespace eternalsonata
