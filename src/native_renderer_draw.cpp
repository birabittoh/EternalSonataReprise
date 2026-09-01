// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_draw.h.

#include "native_renderer_draw.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include "native_renderer_frame.h"
#include "native_renderer_pipeline_internal.h"
#include "native_renderer_plume.h"
#include "native_renderer_plume_internal.h"
#include "native_renderer_profile.h"
#include "native_renderer_shader_debug.h"
#include "native_renderer_texture.h"

namespace eternalsonata {

// The profiler's storage lives here because this is the translation unit that
// enters most of its zones. See native_renderer_profile.h.
uint64_t g_profile_ns[kPhaseCount] = {};
uint64_t g_profile_hits[kPhaseCount] = {};
uint64_t g_gpu_frame_ns = 0;
uint64_t g_gpu_frame_count = 0;
std::chrono::steady_clock::time_point g_profile_window_start = std::chrono::steady_clock::now();

namespace {

// In enum order. A name out of step with the enum silently mislabels every
// number below it, which is worth one comment to prevent.
const char* const kPhaseNames[kPhaseCount] = {
    "present",
    "  fence wait",
    "draw",
    "  vertex upload",
    "  index upload",
    "  constants",
    "  texture bind",
    "    fetch decode",
    "    mirror lookup",
    "    acquire sampler",
    "      texture hash",
    "      texture upload",
    "      aperture walk",
    "  descriptor sets",
    "  bind targets",
    "  projection probe",
    "  stream setup",
    "    swap plan",
    "    water hash",
    "  submit",
    "decl decode",
    "readback publish",
};

// Phases that nest inside another phase. Their time is already inside their
// parent's, so they must not be subtracted from the wall clock a second time
// when working out what is unaccounted for.
bool PhaseIsNested(uint32_t phase) {
  switch (phase) {
    case kPhasePresent:
    case kPhaseDraw:
    case kPhaseDeclDecode:
    // Top level since frames went in flight: the wait moved off the end of the
    // present and onto the front of the next frame's first draw, where it is
    // waiting for the frame kFramesInFlight back rather than for the one just
    // submitted. It is no longer inside any other phase, so leaving it nested
    // would charge its time to `other`.
    case kPhaseFenceWait:
      return false;
    default:
      return true;
  }
}

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
  kDropShaderDisabled,
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
    "a bound shader is switched off in the shader debugger",
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

// QUAD_LIST draws, split by which expansion they took. Both are correct; the
// split is here because only one of the two had a way to be seen on screen
// when it was written, and a zero in either column says the other is untested.
uint64_t g_quad_draws = 0;
uint64_t g_quad_indexed_draws = 0;

// Draws that arrive with the fixed function alpha test on, which the pixel
// shader has to do instead. Zero would mean this title never uses it.
uint64_t g_alpha_test_draws = 0;
bool g_rect_fallback_reported = false;

// ---------------------------------------------------------------------------
// The upload arena.
//
// One arena per frame slot, which is what makes frames in flight possible: the
// GPU reads a draw's vertices, indices and constants straight out of these
// blocks, so a frame's bytes cannot be handed out again until that frame has
// retired. With one arena per slot the wait is for the frame kFramesInFlight
// back, which by then is long done, instead of for the frame just submitted.
//
// `Arena()` is the current slot's blocks for the whole of a frame's recording;
// BeginGuestDrawFrame moves it.

struct ArenaBlock {
  std::unique_ptr<RenderBuffer> buffer;
  uint8_t* mapped = nullptr;
  uint64_t capacity = 0;
  uint64_t used = 0;
};

struct FrameArena {
  std::vector<ArenaBlock> blocks;
  // Sets created because a cache was full. They were handed to draws in this
  // slot's command list, so they live exactly as long as it does.
  std::vector<std::unique_ptr<RenderDescriptorSet>> transient_sets;
};

FrameArena g_arenas[kFramesInFlight];
uint32_t g_arena_slot = 0;
uint32_t g_arena_block = 0;

std::vector<ArenaBlock>& Arena() { return g_arenas[g_arena_slot].blocks; }

// The last draw's texture and sampler descriptor sets, held against the shader
// slot mask, the content epoch and a hash of the fetch constants they were
// built from. See its use in IssueGuestDraw for why a single entry is enough
// and why the epoch has to include the frame boundary.
struct BindingCache {
  uint64_t epoch = 0;
  uint64_t signature = 0;
  uint32_t mask = 0;
  RenderDescriptorSet* texture_set = nullptr;
  RenderDescriptorSet* sampler_set = nullptr;
  bool valid = false;
};
BindingCache g_binding_cache;
uint64_t g_binding_cache_hits = 0;
uint64_t g_binding_cache_misses = 0;

// Staging for the endian swap, so it never happens in the arena itself. See the
// note at its use: the arena is write-combined and reads out of it are what a
// swap in place would be made of. Kept across draws to avoid reallocating; it
// settles at the largest stream the title binds.
std::vector<uint8_t> g_swap_scratch;

struct Allocation {
  RenderBufferReference ref;
  uint8_t* cpu = nullptr;
  explicit operator bool() const { return cpu != nullptr; }
};

Allocation ArenaAllocate(RenderDevice* device, uint64_t bytes) {
  const uint64_t aligned = (bytes + kUploadAlignment - 1) / kUploadAlignment * kUploadAlignment;

  std::vector<ArenaBlock>& arena = Arena();
  for (; g_arena_block < arena.size(); ++g_arena_block) {
    ArenaBlock& block = arena[g_arena_block];
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

  arena.push_back(std::move(block));
  g_arena_block = uint32_t(arena.size()) - 1;

  ArenaBlock& created = arena.back();
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

  // The fallback sampler, for a slot the pixel shader does not declare and for
  // one whose fetch constant could not be read. Every declared slot gets a
  // sampler built from the guest's own fetch constant instead; see
  // AcquireSampler.
  RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = RenderFilter::LINEAR;
  sampler_desc.magFilter = RenderFilter::LINEAR;
  sampler_desc.mipmapMode = RenderMipmapMode::LINEAR;
  sampler_desc.addressU = RenderTextureAddressMode::WRAP;
  sampler_desc.addressV = RenderTextureAddressMode::WRAP;
  sampler_desc.addressW = RenderTextureAddressMode::WRAP;
  g_sampler = device->createSampler(sampler_desc);

  if (!g_sampler) {
    g_resources_failed = true;
    return false;
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
// Descriptor sets, one per distinct set of bindings.
//
// This has to be a set per binding combination and cannot be one set rewritten
// per draw, which is what it was and what made every draw in a frame sample the
// same textures. A descriptor set is a range of a GPU visible heap, and
// `setGraphicsDescriptorSet` records a pointer to it, not a copy of it: the GPU
// reads the contents when the command list *executes*, which here is at the
// present, long after every draw in the frame was recorded. Rewriting the set
// between draws therefore does not rebind anything, it retroactively changes
// what every draw already recorded reads, and the whole frame ends up with the
// bindings of the last draw. The symptom is a late drawn effect's texture
// appearing on everything.
//
// The overlay never had the bug because it keeps a descriptor set per texture.
// The guest side cannot do that -- a binding is sixteen textures and sixteen
// samplers, not one -- so the sets are cached on their contents instead. That
// keeps the count near the number of distinct material bindings the title uses
// rather than the number of draws.

// Textures and samplers are two separate sets, and that split is not cosmetic.
// D3D12 gives a shader-visible *sampler* heap 1024 descriptors against the view
// heap's 65536. A single set carrying sixteen of each therefore costs 1/64th of
// the sampler heap, and a cache of them dies after sixty four sets -- which this
// title reaches on loading a save, at which point Plume's allocator fails and
// (before it was fixed) handed back a set whose descriptor writes went through
// an invalid heap offset, faulting inside the driver.
//
// Split, the many distinct texture combinations cost only view descriptors and
// the handful of distinct sampler combinations cost sampler descriptors. The
// emitted HLSL declares samplers in space1 to match; see the pipeline layout.

struct BindingKey {
  const void* slots[kTextureSlots] = {};

  bool operator==(const BindingKey& other) const {
    return std::memcmp(slots, other.slots, sizeof(slots)) == 0;
  }

  uint64_t Hash() const {
    uint64_t hash = 1469598103934665603ull;  // FNV-1a
    const auto* bytes = reinterpret_cast<const uint8_t*>(slots);
    for (size_t i = 0; i < sizeof(slots); ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
    return hash;
  }
};

struct BindingSetEntry {
  uint64_t hash = 0;
  BindingKey key;
  std::unique_ptr<RenderDescriptorSet> set;
};

std::vector<BindingSetEntry> g_texture_sets;
std::vector<BindingSetEntry> g_sampler_sets;

// What each heap can actually hold, in sets of sixteen, less a margin for the
// overlay and the transient sets. These are not arbitrary: they are the heap
// sizes Plume creates divided by the descriptors a set uses.
constexpr uint32_t kMaxTextureSets = 3072;  // view heap is 65536 / 16 = 4096
constexpr uint32_t kMaxSamplerSets = 48;    // sampler heap is 1024 / 16 = 64
uint64_t g_texture_set_misses = 0;
uint64_t g_sampler_set_misses = 0;
uint64_t g_texture_set_transient = 0;
uint64_t g_binding_set_failed = 0;

RenderDescriptorSet* AcquireBindingSet(RenderDevice* device, std::vector<BindingSetEntry>& cache,
                                       uint32_t cap, bool samplers, const BindingKey& key,
                                       uint64_t* misses) {
  const uint64_t hash = key.Hash();
  for (auto& entry : cache) {
    if (entry.hash == hash && entry.key == key)
      return entry.set.get();
  }

  ++*misses;

  // The same shape the pipeline layout declares for this set, because it has to
  // be the same shape: one range per slot, so Vulkan gets sixteen bindings
  // rather than one sixteen element array. See EnsureLayout.
  RenderDescriptorRange ranges[kTextureSlots];
  for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
    ranges[slot] = RenderDescriptorRange(
        samplers ? RenderDescriptorRangeType::SAMPLER : RenderDescriptorRangeType::TEXTURE, slot, 1);
  }
  auto set = device->createDescriptorSet(RenderDescriptorSetDesc(ranges, kTextureSlots));
  if (!set) {
    // The heap is full. Plume returns null for this now rather than a set that
    // writes through an invalid offset, so it is a dropped draw instead of a
    // fault in the driver.
    ++g_binding_set_failed;
    return nullptr;
  }

  for (uint32_t i = 0; i < kTextureSlots; ++i) {
    if (samplers)
      set->setSampler(i, static_cast<RenderSampler*>(const_cast<void*>(key.slots[i])));
    else
      set->setTexture(i, static_cast<RenderTexture*>(const_cast<void*>(key.slots[i])),
                      RenderTextureLayout::SHADER_READ);
  }

  // The safety valve: correct but allocating per draw, so a non-zero count in
  // the summary means the cap wants raising rather than that anything is wrong.
  // Held on the frame slot, because a draw in this slot's command list is what
  // reads it.
  if (cache.size() >= cap) {
    ++g_texture_set_transient;
    auto& transient = g_arenas[g_arena_slot].transient_sets;
    transient.push_back(std::move(set));
    return transient.back().get();
  }

  BindingSetEntry entry;
  entry.hash = hash;
  entry.key = key;
  entry.set = std::move(set);
  cache.push_back(std::move(entry));
  return cache.back().set.get();
}

// ---------------------------------------------------------------------------
// Samplers, out of the guest's texture fetch constants.
//
// Sampler state is not a register bank on this hardware: the three
// SetSamplerState_* setters patch fields inside the fetch constant for the
// stage, and SetTexture writes the rest of the same six dwords. So the fetch
// constant the texture came out of is also the sampler description, which is
// why this is decoded from the same read.
//
// It is safe to take it at face value for this title because every tfetch in
// all 128 pixel shaders sets its own filter fields to "use the fetch constant"
// and uses no explicit LOD, LOD bias or texel offset. Nothing overrides what is
// decoded here.

// The distinct samplers the title asks for. Small in practice, so a linear
// probe over a flat array is the right shape; the count is reported so a title
// that has more than this says so instead of falling back silently.
constexpr uint32_t kMaxSamplers = 64;

struct SamplerCacheEntry {
  uint64_t key = 0;
  std::unique_ptr<RenderSampler> sampler;
};
SamplerCacheEntry g_samplers[kMaxSamplers];
uint32_t g_sampler_count = 0;
uint64_t g_sampler_overflow = 0;

// Clamp modes with no host equivalent, substituted for and counted. 4 and 5
// clamp to *halfway into the border*, which no host API has; the nearest thing
// is the ordinary clamp, and the difference is one texel at the edge.
uint64_t g_clamp_inexact = 0;

RenderTextureAddressMode MapClamp(uint32_t clamp) {
  switch (clamp) {
    case 0:  // kRepeat
      return RenderTextureAddressMode::WRAP;
    case 1:  // kMirroredRepeat
      return RenderTextureAddressMode::MIRROR;
    case 2:  // kClampToEdge
      return RenderTextureAddressMode::CLAMP;
    case 3:  // kMirrorClampToEdge
      return RenderTextureAddressMode::MIRROR_ONCE;
    case 4:  // kClampToHalfway
      ++g_clamp_inexact;
      return RenderTextureAddressMode::CLAMP;
    case 5:  // kMirrorClampToHalfway
      ++g_clamp_inexact;
      return RenderTextureAddressMode::MIRROR_ONCE;
    case 6:  // kClampToBorder
      return RenderTextureAddressMode::BORDER;
    default:  // kMirrorClampToBorder
      ++g_clamp_inexact;
      return RenderTextureAddressMode::BORDER;
  }
}

RenderSampler* AcquireSampler(RenderDevice* device, const GuestSamplerState& state) {
  const uint64_t key = GuestSamplerKey(state);
  for (uint32_t i = 0; i < g_sampler_count; ++i) {
    if (g_samplers[i].key == key)
      return g_samplers[i].sampler.get();
  }
  if (g_sampler_count == kMaxSamplers) {
    ++g_sampler_overflow;
    return g_sampler.get();
  }

  RenderSamplerDesc desc;
  // TextureFilter: 0 point, 1 linear. 2 (basemap) only occurs on the mip
  // filter, where it means "no mipmapping at all", and 3 cannot occur in a
  // fetch constant since this *is* the fetch constant.
  desc.minFilter = state.min_filter == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  desc.magFilter = state.mag_filter == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  desc.mipmapMode =
      state.mip_filter == 1 ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
  desc.addressU = MapClamp(state.clamp_x);
  desc.addressV = MapClamp(state.clamp_y);
  desc.addressW = MapClamp(state.clamp_z);
  desc.mipLODBias = state.lod_bias;

  // AnisoFilter: 0 disabled, 1..5 are max 1:1 through 16:1, i.e. 1 << (n-1).
  if (state.aniso >= 1 && state.aniso <= 5) {
    desc.anisotropyEnabled = state.aniso > 1;
    desc.maxAnisotropy = 1u << (state.aniso - 1);
  } else {
    desc.anisotropyEnabled = false;
    desc.maxAnisotropy = 1;
  }

  // BorderColor 0 is black with an alpha the hardware documentation does not
  // pin down and 1 is opaque white; the two YCbCr blacks have no host spelling
  // and are not used by anything that samples ordinary textures.
  desc.borderColor = state.border_color == 1 ? RenderBorderColor::OPAQUE_WHITE
                                             : RenderBorderColor::OPAQUE_BLACK;

  // Only the base level is uploaded by the texture mirror, so there is nothing
  // above LOD 0 to sample even when the guest asks for mipmapping. Left as the
  // full range deliberately: a texture created with one level clamps by itself,
  // and pinning maxLOD here would have to be undone the moment mips arrive.
  auto sampler = device->createSampler(desc);
  if (!sampler)
    return g_sampler.get();

  g_samplers[g_sampler_count].key = key;
  g_samplers[g_sampler_count].sampler = std::move(sampler);
  return g_samplers[g_sampler_count++].sampler.get();
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

  // Signed k_2_10_10_10, which has no host input layout format. Repacked in
  // place into R8G8B8A8_SNORM after the swap; see VertexFormatRepacksToSnorm8
  // in native_renderer_pipeline.h for why this and not a widening conversion.
  bool repack_snorm8 = false;
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
    field.repack_snorm8 = VertexFormatRepacksToSnorm8(element.type);
    if (element.usage == 0 && element.usage_index == 0 && VertexFormatIsFloat(element.type))
      plan->position_field = int32_t(plan->count);
    plan->fields[plan->count++] = field;
    if (plan->count == kMaxVertexElements)
      break;
  }
  return true;
}

// Signed k_2_10_10_10 -> R8G8B8A8_SNORM, in the same four bytes.
//
// The Xenos packing puts x in bits 0..9, y in 10..19, z in 20..29 and w in the
// top two, each two's complement, and normalises an n bit signed component by
// its positive maximum with the most negative value clamped to -1. So x, y and
// z divide by 511 and w, which has the single positive value 1, divides by 1.
// R8G8B8A8_SNORM reads back the same way against 127, and -128 is the value the
// host clamps to -1, so the two ends line up exactly rather than by rounding.
uint32_t RepackSnorm8(uint32_t packed) {
  auto component = [](int32_t raw, int32_t bits) {
    const int32_t sign_bit = 1 << (bits - 1);
    if (raw & sign_bit)
      raw -= (sign_bit << 1);
    const float scale = float(sign_bit - 1);
    float value = float(raw) / scale;
    if (value < -1.0f)
      value = -1.0f;
    const float scaled = value * 127.0f;
    int32_t out = int32_t(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    if (out > 127)
      out = 127;
    if (out < -127)
      out = -127;
    return uint32_t(uint8_t(int8_t(out)));
  };

  return component(int32_t(packed & 0x3FFu), 10) |
         (component(int32_t((packed >> 10) & 0x3FFu), 10) << 8) |
         (component(int32_t((packed >> 20) & 0x3FFu), 10) << 16) |
         (component(int32_t((packed >> 30) & 0x3u), 2) << 24);
}

// float -> IEEE half, for the widened attribute stream. Everything that goes
// through it is a small integer out of an 8 bit field, which a half represents
// exactly up to 2048, so the rounding below never fires on the traffic this
// title has; it is written out in full anyway, because a silently approximated
// bone index would look exactly like a wrong one.
uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;

  if (exponent >= 0x1F)
    return uint16_t(sign | 0x7C00u);  // infinities and anything that overflows
  if (exponent <= 0) {
    if (exponent < -10)
      return uint16_t(sign);  // underflows to zero rather than to a subnormal
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    return uint16_t(sign | ((mantissa + (1u << (shift - 1))) >> shift));
  }
  // The round-to-nearest carry is allowed to run into the exponent field, which
  // is what makes a mantissa that rounds up to 1.0 land on the next exponent.
  return uint16_t(sign | (uint32_t(exponent) << 10) | ((mantissa + 0x1000u) >> 13));
}

void SwapVertex(uint8_t* vertex, const SwapPlan& plan) {
  for (uint32_t f = 0; f < plan.count; ++f) {
    const SwapField& field = plan.fields[f];
    uint8_t* at = vertex + field.offset;
    if (field.repack_snorm8) {
      uint32_t value;
      std::memcpy(&value, at, 4);
      value = RepackSnorm8(Swap32(value));
      std::memcpy(at, &value, 4);
      continue;
    }
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

// What makes two requests for the same upload the same request. The primitive
// field is the expansion kind, so a rectangle expansion is never handed to a
// quad draw off the same source pointer; see kWidenedCacheKind.
struct StreamCacheKey {
  const uint8_t* source = nullptr;
  uint32_t bytes = 0;
  uint32_t stride = 0;
  uint32_t primitive = 0;

  bool operator==(const StreamCacheKey& other) const {
    return source == other.source && bytes == other.bytes && stride == other.stride &&
           primitive == other.primitive;
  }
};

struct StreamCacheKeyHash {
  size_t operator()(const StreamCacheKey& key) const {
    uint64_t hash = uint64_t(reinterpret_cast<uintptr_t>(key.source));
    hash ^= (uint64_t(key.stride) << 32) ^ uint64_t(key.bytes);
    hash ^= uint64_t(key.primitive) * 0x9E3779B97F4A7C15ull;
    hash *= 0xFF51AFD7ED558CCDull;
    hash ^= hash >> 33;
    return size_t(hash);
  }
};

struct StreamCacheEntry {
  RenderBufferReference ref;
  uint32_t size = 0;
};

// Hashed rather than scanned. The cache is cleared every frame and gains an
// entry per upload, reaching a couple of thousand entries in a heavy area, and
// the lookup runs once per stream slot per draw. A linear scan over it is
// quadratic in draws per frame. clear() keeps the buckets, so the per-frame
// reset does not rehash.
std::unordered_map<StreamCacheKey, StreamCacheEntry, StreamCacheKeyHash> g_stream_cache;

struct ConstantCacheEntry {
  std::vector<uint8_t> raw;  // the guest bytes, unswapped, as last uploaded
  // The literal pool overlaid on top of them, which belongs to the bound shader
  // rather than to the device, so the same guest bytes under two shaders are
  // two different uploads.
  const uint8_t* literals = nullptr;
  RenderBufferReference ref;
  bool valid = false;
};
// [0] vertex floats, [1] pixel floats, [2] bool/loop, [3] the water pair's
// patched bool/loop bank. The last one is separate so that alternating between
// a water draw and any other draw does not evict the entry each holds.
// [0] vertex floats, [1] pixel floats, [2] bool/loop, [3] the water pair's
// patched bool/loop bank, [4] its patched vertex floats. The patched banks get
// their own entries so that alternating between a water draw and any other draw
// does not evict the entry each holds.
ConstantCacheEntry g_constant_cache[5];

// Whether the pixel float bank the guest currently holds is entirely zero over
// the registers the guest owns, c0..c251. The literal pool at 252..255 is
// excluded because this renderer writes that itself, so including it would mask
// exactly the case being looked for.
//
// A shader that modulates its texture by a constant reads black out of a zero
// bank, which is indistinguishable in the picture from a black texture or a
// failed light. Recomputed only when the bank is actually re-uploaded, which is
// rare, and then charged to every draw that reads it.
bool g_pixel_bank_zero = false;
uint64_t g_zero_pixel_bank_draws = 0;
uint32_t g_zero_pixel_bank_slots[16] = {};
uint32_t g_zero_pixel_bank_slot_count = 0;

// One-shot dump of everything a single pixel shader reads, selected by slot
// through the ES_DUMP_PS environment variable (unset means off). RenderDoc
// reports zeros for every constant here because the banks are bound as root
// CBVs and it does not resolve them, so when a shader's output has to be
// explained in terms of its constants, this is the instrument: it prints the
// guest's own bytes, swapped exactly the way the upload swaps them.
int PixelDumpSlot() {
  static const int slot = [] {
    const char* value = std::getenv("ES_DUMP_PS");
    return value != nullptr ? std::atoi(value) : -1;
  }();
  return slot;
}
// Dumped repeatedly rather than once. The constants a shader reads are scene
// state, so a single dump on the first matching draw of the run answers for
// whatever scene happened to load first, which is not necessarily the one being
// investigated. Spaced out in wall clock time and capped, so a run can be
// walked into the situation of interest without the log filling up.
constexpr uint32_t kPixelDumpLimit = 8;
constexpr auto kPixelDumpInterval = std::chrono::seconds(10);
uint32_t g_pixel_dumps = 0;
std::chrono::steady_clock::time_point g_pixel_dump_last;

float GuestFloat(const uint8_t* bank, uint32_t reg, uint32_t component) {
  uint32_t raw;
  std::memcpy(&raw, bank + reg * 16 + component * 4, 4);
  raw = Swap32(raw);
  float out;
  std::memcpy(&out, &raw, 4);
  return out;
}

uint32_t GuestDword(const uint8_t* bank, uint32_t index) {
  uint32_t raw;
  std::memcpy(&raw, bank + index * 4, 4);
  return Swap32(raw);
}

void DumpConstantsForShader(const GuestDrawCall& call) {
  if (g_pixel_dumps >= kPixelDumpLimit || PixelDumpSlot() < 0)
    return;
  int vertex_slot = -1, pixel_slot = -1;
  GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
  if (pixel_slot != PixelDumpSlot())
    return;
  const auto now = std::chrono::steady_clock::now();
  if (g_pixel_dumps != 0 && now - g_pixel_dump_last < kPixelDumpInterval)
    return;
  g_pixel_dump_last = now;
  ++g_pixel_dumps;

  const uint8_t* floats = call.device + d3d::kPixelConstantShadow;
  REXLOG_WARN("native_renderer: constant dump #{} for vs={} ps={}", g_pixel_dumps, vertex_slot,
              pixel_slot);
  for (uint32_t reg = 0; reg < 32; ++reg) {
    REXLOG_WARN("native_renderer:   ps c{} = {} {} {} {}", reg, GuestFloat(floats, reg, 0),
                GuestFloat(floats, reg, 1), GuestFloat(floats, reg, 2), GuestFloat(floats, reg, 3));
  }
  REXLOG_WARN("native_renderer:   ps c255 = {} {} {} {}", GuestFloat(floats, 255, 0),
              GuestFloat(floats, 255, 1), GuestFloat(floats, 255, 2), GuestFloat(floats, 255, 3));

  // The bool and loop bank, as the shared cbuffer lays it out: eight bool dwords
  // then thirty two loop dwords. A loop constant packs its trip count in bits
  // 0..7, so a zero here is a body that never runs.
  const uint8_t* bools = call.device + d3d::kBoolConstantShadow;
  for (uint32_t i = 0; i < 8; ++i)
    REXLOG_WARN("native_renderer:   bools[{}..{}] = 0x{:08X}", i * 32, i * 32 + 31,
                GuestDword(bools, i));
  for (uint32_t i = 0; i < 32; ++i)
    REXLOG_WARN("native_renderer:   loop i{} = 0x{:08X}", i, GuestDword(bools, 8 + i));
}

// --- The water probe ---
//
// What this is for, and what it already ruled out. The first theory about the
// water was that it animated too fast, which would have put the motion in the
// UV matrix vs_040 builds from vertex constants c24/c25 (`dp3 r6.y, c24.xyzz,
// r6.xzww`, under bool b2). It does not: over a thirty second run those two
// constants held the identity matrix on every single draw. Whatever the water
// does frame to frame, it is not a scrolling texture matrix, so do not go back
// there.
//
// The symptom as it actually presents is flicker: the strength of the effect
// changing between one frame and the next. That is a frame-to-frame difference,
// so a probe that samples every few seconds cannot see it. This one logs one
// line per frame for a bounded run of frames, carrying the three things that
// could differ between two consecutive frames of the same static scene:
//
//   * how many draws of the pair there were. Alternating between one and two,
//     or one and none, is itself the flicker.
//   * the bool bank over b128..b159. ps_058 is almost entirely branches on
//     those: b128 picks between a computed tangent frame and a normal map fetch,
//     b134 the screen-space refraction through tf13, b138/b140/b142/b143 which
//     maps get sampled at all. A bool flipping per frame changes which shader
//     effectively runs.
//   * what is bound in each texture slot. A slot alternating between a real
//     texture and the white placeholder is the single most likely cause here:
//     ps_058 samples tf11 and tf13 at projected/screen-space coordinates, which
//     is how a title samples the scene behind the water, and those come from
//     resolve destinations rather than from asset memory. A resolve that is not
//     ready every frame reads back as the placeholder on the frames it misses.
//
// Off unless `ES_WATER_PROBE` is set, following the ES_DUMP_PS convention; its
// value is how many frames to log (default 120). `ES_WATER_VS` and
// `ES_WATER_PS` move the pair off 40/58.
struct WaterProbeKnobs {
  bool enabled = false;
  uint32_t frames = 120;
  int vertex_slot = 40;
  // -1 matches any pixel shader. That is the default because the pair is not
  // known: disabling vs_040 hides the puddle and disabling ps_058 does not, so
  // the puddle is this vertex shader with some other pixel shader, and the
  // point of the probe is now to find out which.
  int pixel_slot = -1;
};

const WaterProbeKnobs& WaterProbe() {
  static const WaterProbeKnobs knobs = [] {
    WaterProbeKnobs k;
    const char* frames = std::getenv("ES_WATER_PROBE");
    k.enabled = frames != nullptr;
    if (frames != nullptr && std::atoi(frames) > 0)
      k.frames = uint32_t(std::atoi(frames));
    if (const char* v = std::getenv("ES_WATER_VS"))
      k.vertex_slot = std::atoi(v);
    if (const char* v = std::getenv("ES_WATER_PS"))
      k.pixel_slot = std::atoi(v);
    return k;
  }();
  return knobs;
}

// How many of the pair's draws in a frame get their constants kept separately.
// Two here; the headroom is so a scene with more water surfaces does not
// silently fold them together, which is the bug this indexing replaced.
constexpr uint32_t kMaxWaterProbeSurfaces = 4;

// One frame's worth of observation, accumulated across the frame's draws and
// flushed at the frame boundary.
struct WaterProbeFrame {
  uint32_t draws = 0;
  int vertex_slot = -1;
  int pixel_slot = -1;
  // All of these are **per draw within the frame**, for the same reason the
  // constant banks are: the pair draws more than one surface and they do not
  // share state. Kept as one set for the whole frame, they held whatever the
  // last draw bound, and every conclusion drawn from them described that
  // surface alone. The bool bank especially: "b128 is set, so tf12 is bound but
  // never sampled" was read off the last draw's bank, and tf12 is the one input
  // to this pair that changes every single frame.
  // The render state each surface draws under. Everything examined so far has
  // been an *input* to the shader; how its output is combined with what is
  // already in the target has never been looked at, and the two surfaces do not
  // have to share it. Blend, depth and alpha test are all per draw, and the
  // summary already reports that stencil is enabled on 51 pipelines and is not
  // applied at all, so there is a known gap here.
  // A fingerprint of the guest vertex bytes this surface drew from. See where
  // it is accumulated, in the vertex stream loop.
  uint64_t vertex_hash[kMaxWaterProbeSurfaces] = {};

  uint32_t depth_control[kMaxWaterProbeSurfaces] = {};
  uint32_t blend_control[kMaxWaterProbeSurfaces] = {};
  uint32_t mode_cntl[kMaxWaterProbeSurfaces] = {};
  uint32_t color_mask[kMaxWaterProbeSurfaces] = {};

  uint32_t bools[kMaxWaterProbeSurfaces][2] = {};  // b128..b159, what ps_058 branches on
  uint32_t texture_mask[kMaxWaterProbeSurfaces] = {};
  const void* textures[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  bool placeholder[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  uint32_t address[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  // The guest format code and extent of each slot's fetch. A slot that reads
  // PLACEHOLDER says the mirror produced nothing but not why; the format is
  // what distinguishes "the mirror has no mapping for this" from a source that
  // would not read. See the mirror's per-format refusal counter.
  uint32_t format[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  uint32_t width[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  uint32_t height[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  bool from_resolve[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  bool is_render_target[kMaxWaterProbeSurfaces][kTextureSlots] = {};
  // The float banks, **per draw within the frame** rather than one set for the
  // whole frame. ps_058 scales its output through c10.x, c16.w, c8.x and c4/c7,
  // so a constant oscillating is what "the intensity flickers" looks like from
  // here.
  //
  // Keyed by draw index because the pair draws more than one surface per frame
  // and they do not share constants. An earlier version kept a single set and
  // overwrote it on every draw, so it held the *last* draw's banks and the
  // frame-to-frame diff only ever described that one surface. With
  // ES_WATER_ONLY_SURFACE having since shown that draw 0 flickers and draw 1
  // does not, that meant the diff was describing the surface that behaves.
  // Diffing each surface against itself is the whole point.
  float pixel[kMaxWaterProbeSurfaces][d3d::kConstantRegisters * 4] = {};
  float vertex[kMaxWaterProbeSurfaces][d3d::kConstantRegisters * 4] = {};
  bool banks_valid[kMaxWaterProbeSurfaces] = {};
};
WaterProbeFrame g_water_frame;
uint32_t g_water_frames_logged = 0;

// Accumulated by the vertex stream loop, which runs before the draw is noted,
// and claimed by the surface when it is. Cleared as it is claimed.
uint64_t g_water_pending_vertex_hash = 0;

// True when this draw uses the shader pair under investigation, regardless of
// whether the probe is on or has budget left. The interventions key off this;
// only the logging keys off IsWaterProbeDraw below.
bool IsWaterPairDraw(const GuestDrawCall& call) {
  const WaterProbeKnobs& knobs = WaterProbe();
  int vertex_slot = -1, pixel_slot = -1;
  GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
  if (vertex_slot != knobs.vertex_slot)
    return false;
  return knobs.pixel_slot < 0 || pixel_slot == knobs.pixel_slot;
}

// True when this draw is the pair being probed *and* the probe still wants to
// record it, which both the constant side and the texture-binding side ask.
bool IsWaterProbeDraw(const GuestDrawCall& call) {
  const WaterProbeKnobs& knobs = WaterProbe();
  if (!knobs.enabled || g_water_frames_logged >= knobs.frames)
    return false;
  int vertex_slot = -1, pixel_slot = -1;
  GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
  if (vertex_slot != knobs.vertex_slot)
    return false;
  return knobs.pixel_slot < 0 || pixel_slot == knobs.pixel_slot;
}

// Every pixel shader seen paired with the probed vertex shader, with a draw
// count each. Printed once at the end of the probe's frame budget, so a single
// run names the puddle's pixel shader instead of another round of guessing.
struct WaterPairing {
  int pixel_slot = -1;
  uint64_t draws = 0;
};
WaterPairing g_water_pairs[32];

void WaterProbeNotePair(int pixel_slot) {
  for (WaterPairing& pair : g_water_pairs) {
    if (pair.pixel_slot == pixel_slot) {
      ++pair.draws;
      return;
    }
    if (pair.pixel_slot < 0) {
      pair.pixel_slot = pixel_slot;
      pair.draws = 1;
      return;
    }
  }
}

void WaterProbeLogPairs() {
  std::string list;
  for (const WaterPairing& pair : g_water_pairs) {
    if (pair.pixel_slot < 0)
      break;
    list += fmt::format(" ps_{:03d}={} draws", pair.pixel_slot, pair.draws);
  }
  REXLOG_WARN("native_renderer: water probe pixel shaders paired with vs_{:03d}:{}",
              WaterProbe().vertex_slot, list.empty() ? std::string(" none") : list);
}

// The constant half, called once the banks are known to be readable.
void WaterProbeNoteConstants(const GuestDrawCall& call) {
  if (!IsWaterProbeDraw(call))
    return;
  ++g_water_frame.draws;
  GuestPipelineShaderSlots(call.pipeline, &g_water_frame.vertex_slot, &g_water_frame.pixel_slot);
  WaterProbeNotePair(g_water_frame.pixel_slot);

  // Draw index within the frame, which is what identifies the surface. Past the
  // table the state is simply not kept, rather than folded onto another
  // surface's slot.
  const uint32_t surface = g_water_frame.draws - 1;
  if (surface >= kMaxWaterProbeSurfaces)
    return;

  const uint8_t* bools = call.device + d3d::kBoolConstantShadow;
  g_water_frame.bools[surface][0] = GuestDword(bools, 4);  // b128..b159
  g_water_frame.bools[surface][1] = GuestDword(bools, 5);

  g_water_frame.vertex_hash[surface] = g_water_pending_vertex_hash;
  g_water_pending_vertex_hash = 0;

  GuestPipelineRenderRegisters(call.pipeline, &g_water_frame.depth_control[surface],
                               &g_water_frame.blend_control[surface],
                               &g_water_frame.mode_cntl[surface],
                               &g_water_frame.color_mask[surface]);

  const uint8_t* pixel = call.device + d3d::kPixelConstantShadow;
  const uint8_t* vertex = call.device + d3d::kVertexConstantShadow;
  for (uint32_t i = 0; i < d3d::kConstantRegisters * 4; ++i) {
    g_water_frame.pixel[surface][i] = GuestFloat(pixel, i / 4, i % 4);
    g_water_frame.vertex[surface][i] = GuestFloat(vertex, i / 4, i % 4);
  }
  g_water_frame.banks_valid[surface] = true;
}

// The registers that differ from the previous frame, as "ps c10.x 1.5 -> 0.2".
// Capped, because a moving camera legitimately rewrites every view matrix and
// the point is to see the short list when standing still.
std::string WaterProbeBankDiff(const char* label, const float* now, const float* before) {
  constexpr uint32_t kMaxReported = 10;
  std::string out;
  uint32_t reported = 0, changed = 0;
  for (uint32_t i = 0; i < d3d::kConstantRegisters * 4; ++i) {
    if (now[i] == before[i])
      continue;
    ++changed;
    if (reported++ < kMaxReported)
      out += fmt::format(" {} c{}.{} {} -> {}", label, i / 4, "xyzw"[i % 4], before[i], now[i]);
  }
  if (changed > kMaxReported)
    out += fmt::format(" (+{} more {})", changed - kMaxReported, label);
  return out;
}

// The texture half, called with the bindings this draw resolved to. `white` is
// the placeholder, so a slot equal to it is a texture that could not be
// produced this frame.
void WaterProbeNoteTextures(const GuestDrawCall& call, uint32_t texture_mask,
                            const BindingKey& textures, const void* white,
                            const TextureFetch fetches[kTextureSlots],
                            const bool have_fetch[kTextureSlots]) {
  if (!IsWaterProbeDraw(call))
    return;
  // Same draw as the constants above, which ran first, so the same index.
  if (g_water_frame.draws == 0 || g_water_frame.draws - 1 >= kMaxWaterProbeSurfaces)
    return;
  const uint32_t surface = g_water_frame.draws - 1;

  g_water_frame.texture_mask[surface] = texture_mask;
  for (uint32_t stage = 0; stage < kTextureSlots; ++stage) {
    g_water_frame.textures[surface][stage] = textures.slots[stage];
    g_water_frame.placeholder[surface][stage] = textures.slots[stage] == white;
    g_water_frame.address[surface][stage] = have_fetch[stage] ? fetches[stage].base_address : 0;
    g_water_frame.format[surface][stage] = have_fetch[stage] ? fetches[stage].format : 0;
    g_water_frame.width[surface][stage] = have_fetch[stage] ? fetches[stage].width : 0;
    g_water_frame.height[surface][stage] = have_fetch[stage] ? fetches[stage].height : 0;
    // Which of the mirror's two sources this slot came from. A resolve
    // destination is something the guest rendered and resolved this frame, so
    // its contents are live; anything else was decoded out of guest memory and
    // is only re-read when the content hash notices a change.
    g_water_frame.from_resolve[surface][stage] =
        have_fetch[stage] && FrameResolveTextureByAddress(fetches[stage].base_address,
                                                          fetches[stage].width,
                                                          fetches[stage].height) != nullptr;
    // The hazard: this slot is the image the draw is writing to.
    g_water_frame.is_render_target[surface][stage] =
        textures.slots[stage] != nullptr && textures.slots[stage] == FrameCurrentColorTexture();
  }
}

// --- The water rotation, slowed down ---
//
// vs_040's world matrix lives in vertex constants c0..c3, and for this material
// the guest rewrites four of its components every frame in the pattern
//
//     c0.x =  s*cos(t)   c0.z = s*sin(t)
//     c2.x = -s*sin(t)   c2.z = s*cos(t)
//
// which is a rotation about Y at scale s (0.15 as measured). The angle advances
// a fixed 0.01745 rad -- exactly one degree -- per rendered frame, so the water
// spins at the frame rate rather than in real time: at 60 fps it turns twice as
// fast as it did on a console locked to 30. That is both halves of the reported
// symptom, because one degree per frame against a high frequency normal map
// also aliases temporally, which is what reads as flicker. The refraction under
// b134 is only the term that aliases hardest, not the cause.
//
// The angle is recovered rather than the components scaled, because scaling
// cos and sin independently would stop the matrix being a rotation and shear
// the mesh. The guest's own angle is differenced frame to frame, that delta is
// re-integrated at `ES_WATER_SCALE`, and the matrix is rebuilt from the result
// at the guest's own scale. A frame where the guest did not move the angle
// contributes nothing, so this tracks exactly whenever the water is still.
//
// `ES_WATER_SCALE=0.5` restores the console's speed at 60 fps. Unset means off.
float WaterRotationScale() {
  static const float scale = [] {
    const char* value = std::getenv("ES_WATER_SCALE");
    return value != nullptr ? float(std::atof(value)) : 1.0f;
  }();
  return scale;
}

// Per surface, not global. This shader pair draws more than one water surface
// per frame -- two here, one at magnitude 0.15 and rotating, one at magnitude
// 0.5 and static -- and a single accumulator shared between them differences
// each draw's angle against the other draw's, which produces nonsense and
// snaps instead of slowing. Keyed by the draw's index within the frame, which
// is stable because the guest issues them in the same order every frame.
struct WaterRotationState {
  bool valid = false;
  float previous = 0.0f;  // the guest's angle last frame
  float slowed = 0.0f;    // the angle actually uploaded
};
constexpr uint32_t kMaxWaterSurfaces = 8;
WaterRotationState g_water_rotation[kMaxWaterSurfaces];
uint32_t g_water_surface = 0;  // reset at the frame boundary
uint8_t g_water_vertex_bank[d3d::kConstantRegisters * 16];

// --- One water surface at a time ---
//
// The probed pair draws two surfaces per frame, and every input to them is now
// byte-identical frame to frame except the world matrix rotation: same bools,
// same texture bindings, same host texture pointers, same float banks. An
// oscillation cannot come out of inputs that do not oscillate, so the remaining
// candidate is the two surfaces interacting with each other rather than either
// one being wrong. Two coplanar-ish surfaces covering the same pixels is
// z-fighting, which shimmers, is driven by geometry rather than by time, and
// therefore does not change rate with the frame rate -- the one property of the
// symptom nothing else has explained.
//
// `ES_WATER_ONLY_SURFACE=0` keeps only the first of the pair, `=1` only the
// second. If either alone is steady, the surfaces are fighting and neither
// shader is at fault. If one alone still flickers, that surface owns the defect
// and the other is a bystander. Unset means off; -1 also means off.
int WaterOnlySurface() {
  static const int only = [] {
    const char* value = std::getenv("ES_WATER_ONLY_SURFACE");
    return value != nullptr ? std::atoi(value) : -1;
  }();
  return only;
}

// Counts the probed pair's draws within the frame, independently of the
// rotation slowdown's own counter, which only advances when that knob is on.
uint32_t g_water_draw_index = 0;

// --- Pinning the ping-pong buffer ---
//
// Surface 0 has b128 *clear* (0x8D0) where surface 1 has it set (0xC71), so
// surface 0 takes the branch that samples tf12 -- the opposite of what the
// frame-level bool bank suggested when it was really only ever surface 1's.
// And tf12's fetch address alternates every frame between two 64x64 resolve
// destinations, 0x0AF6C000 and 0x0AF70000. A surface sampling a texture that
// swaps every frame is a flicker whose rate is set by the swap rather than by
// elapsed time, which is the property nothing else has explained.
//
// Both destinations are resolved, so neither is missing; the open question is
// whether their *contents* agree. `ES_WATER_PIN_T12=0AF70000` (hex, no prefix)
// forces every tf12 fetch on the probed pair to one of them. If the flicker
// stops, the two buffers hold different images and the bug is in how one of
// them is produced. If it continues, the alternation is innocent.
uint32_t WaterPinT12() {
  static const uint32_t address = [] {
    const char* value = std::getenv("ES_WATER_PIN_T12");
    return value != nullptr ? uint32_t(std::strtoul(value, nullptr, 16)) : 0u;
  }();
  return address;
}

void PutGuestFloat(uint8_t* bank, uint32_t reg, uint32_t component, float value) {
  uint32_t raw;
  std::memcpy(&raw, &value, 4);
  raw = Swap32(raw);
  std::memcpy(bank + reg * 16 + component * 4, &raw, 4);
}

const uint8_t* WaterSlowVertexBank(const GuestDrawCall& call, bool* patched) {
  *patched = false;
  const uint8_t* bank = call.device + d3d::kVertexConstantShadow;
  const float scale = WaterRotationScale();
  if (scale == 1.0f)
    return bank;
  const WaterProbeKnobs& knobs = WaterProbe();
  int vertex_slot = -1, pixel_slot = -1;
  GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
  if (vertex_slot != knobs.vertex_slot || pixel_slot != knobs.pixel_slot)
    return bank;

  const float cos_term = GuestFloat(bank, 0, 0);  // c0.x = s*cos
  const float sin_term = GuestFloat(bank, 0, 2);  // c0.z = s*sin
  const float magnitude = std::sqrt(cos_term * cos_term + sin_term * sin_term);
  // Not the rotation this is written for. Leave it alone rather than rebuild
  // something that was never a rotation in the first place.
  if (magnitude < 1e-6f)
    return bank;
  const float angle = std::atan2(sin_term, cos_term);

  // Past the table, leave the draw alone rather than fold it into another
  // surface's accumulator, which is the bug this indexing exists to avoid.
  if (g_water_surface >= kMaxWaterSurfaces)
    return bank;
  WaterRotationState& state = g_water_rotation[g_water_surface++];

  if (!state.valid) {
    state.previous = angle;
    state.slowed = angle;
    state.valid = true;
  } else {
    // Shortest way round, so the wrap through +/-pi is a small step rather than
    // a full turn backwards.
    float delta = angle - state.previous;
    while (delta > 3.14159265f)
      delta -= 6.28318531f;
    while (delta < -3.14159265f)
      delta += 6.28318531f;
    state.previous = angle;
    state.slowed += delta * scale;
    while (state.slowed > 3.14159265f)
      state.slowed -= 6.28318531f;
    while (state.slowed < -3.14159265f)
      state.slowed += 6.28318531f;
  }

  // Self reporting, because "nothing changed on screen" has two very different
  // causes: the patch not reaching the draw, and the draw not being what moves.
  // Throttled, and it prints the guest's angle next to the one being uploaded,
  // so a divergence that grows is proof the substitution is live.
  static uint32_t reports = 0;
  static std::chrono::steady_clock::time_point last;
  const auto now = std::chrono::steady_clock::now();
  if (reports < 16 && (reports < 4 || now - last >= std::chrono::seconds(2))) {
    last = now;
    ++reports;
    REXLOG_WARN(
        "native_renderer: water rotation #{} surface {} scale={} guest angle={} -> uploading {} "
        "(magnitude {})",
        reports, g_water_surface - 1, scale, state.previous, state.slowed, magnitude);
  }

  const float c = magnitude * std::cos(state.slowed);
  const float s = magnitude * std::sin(state.slowed);
  std::memcpy(g_water_vertex_bank, bank, sizeof(g_water_vertex_bank));
  PutGuestFloat(g_water_vertex_bank, 0, 0, c);   // c0.x
  PutGuestFloat(g_water_vertex_bank, 0, 2, s);   // c0.z
  PutGuestFloat(g_water_vertex_bank, 2, 0, -s);  // c2.x
  PutGuestFloat(g_water_vertex_bank, 2, 2, c);   // c2.z
  *patched = true;
  return g_water_vertex_bank;
}

// --- The refraction cut, an experiment rather than a fix ---
//
// ps_058 samples the scene behind the water in screen space through tf13, under
// bool b134. The capture shows the guest re-copying the scene into that texture
// immediately before each water draw, so every water surface refracts an image
// that already contains the water drawn before it, and the next frame refracts
// a scene containing this frame's water. That is a feedback path across frames,
// and feedback is what oscillates instead of settling.
//
// Clearing b134 for this shader pair removes the refraction and nothing else.
// If the flicker stops, the feedback is the cause and the fix is to give the
// refraction a copy of the scene taken before any water is drawn. If it
// continues, the feedback is innocent and this rules out the last branch that
// depends on frame history.
//
// The water will look wrong while this is on. It is a diagnostic switch:
// `ES_WATER_NO_REFRACT=1`.
bool WaterCutRefraction() {
  static const bool cut = [] {
    const char* value = std::getenv("ES_WATER_NO_REFRACT");
    return value != nullptr && std::atoi(value) != 0;
  }();
  return cut;
}

uint8_t g_water_bool_bank[d3d::kBoolLoopConstantBytes];

// The bool/loop bank this draw should upload. b134 lives in bit 6 of the fifth
// dword, which covers b128..b159. Patched in guest byte order so the upload's
// own swap still applies uniformly.
const uint8_t* WaterCutBoolBank(const GuestDrawCall& call, bool* patched) {
  *patched = false;
  const uint8_t* bank = call.device + d3d::kBoolConstantShadow;
  if (!WaterCutRefraction())
    return bank;
  const WaterProbeKnobs& knobs = WaterProbe();
  int vertex_slot = -1, pixel_slot = -1;
  GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
  if (vertex_slot != knobs.vertex_slot || pixel_slot != knobs.pixel_slot)
    return bank;

  std::memcpy(g_water_bool_bank, bank, sizeof(g_water_bool_bank));
  uint32_t dword = GuestDword(bank, 4);
  dword &= ~(1u << 6);  // b134, the screen-space refraction
  const uint32_t swapped = Swap32(dword);
  std::memcpy(g_water_bool_bank + 4 * 4, &swapped, 4);
  *patched = true;
  return g_water_bool_bank;
}

// Flushed at the frame boundary rather than per draw, so consecutive lines are
// consecutive frames and a value that alternates is visible as an alternation.
void WaterProbeEndFrame() {
  const WaterProbeKnobs& knobs = WaterProbe();
  if (!knobs.enabled || g_water_frames_logged >= knobs.frames)
    return;
  if (g_water_frame.draws == 0 && g_water_frames_logged == 0)
    return;  // the scene has not been reached yet; do not spend the budget
  ++g_water_frames_logged;

  // One line per surface. Each surface has its own bool bank and its own
  // bindings, and reading either off "the frame" is what hid the difference
  // between the surface that flickers and the one that does not.
  const uint32_t surfaces = g_water_frame.draws < kMaxWaterProbeSurfaces
                                ? g_water_frame.draws
                                : kMaxWaterProbeSurfaces;
  for (uint32_t surface = 0; surface < surfaces; ++surface) {
    std::string slots;
    for (uint32_t stage = 0; stage < kTextureSlots; ++stage) {
      if ((g_water_frame.texture_mask[surface] & (1u << stage)) == 0)
        continue;
      slots += fmt::format(
          " t{}={}@{:08X}/f{}/{}x{}{}", stage,
          g_water_frame.placeholder[surface][stage]
              ? std::string("PLACEHOLDER")
              : fmt::format("{:016X}", uintptr_t(g_water_frame.textures[surface][stage])),
          g_water_frame.address[surface][stage], g_water_frame.format[surface][stage],
          g_water_frame.width[surface][stage], g_water_frame.height[surface][stage],
          g_water_frame.from_resolve[surface][stage]
              ? (g_water_frame.is_render_target[surface][stage] ? "(resolve,IS_RENDER_TARGET)"
                                                                : "(resolve)")
              : (g_water_frame.is_render_target[surface][stage] ? "(IS_RENDER_TARGET)" : ""));
    }
    REXLOG_WARN(
        "native_renderer: water probe frame {} surface {}/{} vs={} ps={} b128..159=0x{:08X} "
        "0x{:08X} vhash=0x{:016X} depth=0x{:08X} blend=0x{:08X} mode=0x{:08X} mask=0x{:08X}{}",
        g_water_frames_logged, surface, g_water_frame.draws, g_water_frame.vertex_slot,
        g_water_frame.pixel_slot, g_water_frame.bools[surface][0],
        g_water_frame.bools[surface][1], g_water_frame.vertex_hash[surface],
        g_water_frame.depth_control[surface],
        g_water_frame.blend_control[surface], g_water_frame.mode_cntl[surface],
        g_water_frame.color_mask[surface], slots);
  }

  // One line per surface, each diffed against the same surface's previous
  // frame. ES_WATER_ONLY_SURFACE has shown that surface 0 flickers and surface
  // 1 does not, so the two lines are the comparison that matters: whatever
  // oscillates on 0 and not on 1 is the defect.
  static WaterProbeFrame previous;
  for (uint32_t surface = 0; surface < kMaxWaterProbeSurfaces; ++surface) {
    if (!g_water_frame.banks_valid[surface] || !previous.banks_valid[surface])
      continue;
    const std::string diff =
        WaterProbeBankDiff("ps", g_water_frame.pixel[surface], previous.pixel[surface]) +
        WaterProbeBankDiff("vs", g_water_frame.vertex[surface], previous.vertex[surface]);
    REXLOG_WARN("native_renderer:   surface {} constants changed since last frame:{}", surface,
                diff.empty() ? std::string(" none") : diff);
  }
  for (uint32_t surface = 0; surface < kMaxWaterProbeSurfaces; ++surface) {
    if (!g_water_frame.banks_valid[surface])
      continue;
    std::memcpy(previous.pixel[surface], g_water_frame.pixel[surface],
                sizeof(previous.pixel[surface]));
    std::memcpy(previous.vertex[surface], g_water_frame.vertex[surface],
                sizeof(previous.vertex[surface]));
    previous.banks_valid[surface] = true;
  }
  if (g_water_frames_logged == knobs.frames)
    WaterProbeLogPairs();

  g_water_frame = WaterProbeFrame();
}

// The alpha test constants, which are built rather than copied, so they are
// cached on their two values instead of on the guest bytes behind them.
struct AlphaTestCache {
  uint32_t func = 0;
  float ref = 0.0f;
  RenderBufferReference buffer;
  bool valid = false;
};
AlphaTestCache g_alpha_cache;

// A one-shot probe for the world-locked "colour filter" boundary. The terrain
// pixel shader branches on a projected mask in texture slot 11, sampled at
// coordinates a matrix in vertex constants c36..c39 builds out of world space.
// A frame capture cannot say which of the two is wrong: RenderDoc reports no
// sampler address mode for this renderer, and reads root CBVs as zero (see the
// note about get_cbuffer_contents in the handoff). So latch both off the first
// draw each frame that declares the slot and print them next to each other.
//
// What the two answers mean. The mask is a 2x2 atlas whose two diagonal cells
// are unused white, and the projected coordinates run well outside [0,1], so
// a repeat mode sweeps the terrain across all four cells and the two empty ones
// become the regions with no filter. Either the address mode should be a clamp,
// or the matrix is scaled too large for the mesh to stay inside one cell.
constexpr uint32_t kProbeSlot = 11;
constexpr uint32_t kProbeConstant = 36;
constexpr uint32_t kProbeConstantCount = 4;
// c10, which scales the light accumulator in `mad r2.xyz, r6.yzww, c10.xxxx`.
constexpr uint32_t kProbeLightScale = 10;

constexpr const char* kClampNames[8] = {
    "repeat",        "mirrored-repeat",       "clamp-to-edge",   "mirror-clamp-to-edge",
    "clamp-halfway", "mirror-clamp-halfway",  "clamp-to-border", "mirror-clamp-to-border"};

struct ProjectionProbe {
  bool captured = false;  // already latched this frame
  bool valid = false;     // ever latched
  GuestSamplerState sampler;
  TextureFetch fetch;
  float matrix[kProbeConstantCount][4] = {};
  // The light loop's trip count and the constant its accumulator is scaled by,
  // read on this same draw. The guest toggles the count between 3 and 0 per
  // batch, so a count sampled at any other draw says nothing about this one.
  uint32_t loop = 0;
  float light_scale[4] = {};
};
ProjectionProbe g_projection_probe;

// The shadow holds guest order dwords, so the swap here is the same one
// UploadConstantBank does. Reading it any other way would report a number the
// shader never sees, which is the whole failure mode this is meant to rule out.
void CaptureProjectionProbe(const GuestDrawCall& call, uint32_t texture_mask) {
  if (g_projection_probe.captured || (texture_mask & (1u << kProbeSlot)) == 0)
    return;

  TextureFetch fetch;
  GuestSamplerState sampler;
  if (!GetBoundTextureFetch(call.memory_base, kProbeSlot, fetch, &sampler))
    return;

  const uint8_t* bank = call.device + d3d::kVertexConstantShadow + kProbeConstant * 16;
  for (uint32_t row = 0; row < kProbeConstantCount; ++row) {
    for (uint32_t col = 0; col < 4; ++col) {
      uint32_t word;
      std::memcpy(&word, bank + row * 16 + col * 4, 4);
      word = Swap32(word);
      std::memcpy(&g_projection_probe.matrix[row][col], &word, 4);
    }
  }
  // Unified loop register 16, the pixel half's first. The bank is contiguous
  // from device+10016, so the index addresses both halves.
  const uint8_t* loops = call.device + d3d::kBoolConstantShadow + 32;
  std::memcpy(&g_projection_probe.loop, loops + 4 * 16, 4);
  g_projection_probe.loop = Swap32(g_projection_probe.loop);

  const uint8_t* pixel = call.device + d3d::kPixelConstantShadow + kProbeLightScale * 16;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t word;
    std::memcpy(&word, pixel + c * 4, 4);
    word = Swap32(word);
    std::memcpy(&g_projection_probe.light_scale[c], &word, 4);
  }

  g_projection_probe.sampler = sampler;
  g_projection_probe.fetch = fetch;
  g_projection_probe.captured = true;
  g_projection_probe.valid = true;
}

// Copy a constant bank out of the device, swapping, and reuse the previous
// upload when the guest bytes have not changed. The comparison is against the
// raw guest memory rather than against our own idea of what was set, so a write
// that did not come through the two constant setters cannot go unnoticed.
//
// `literals`, when given, is 64 host order bytes written over registers
// 252..255 after the copy. That pool comes from the bound shader's own
// container and the guest never puts it in the shadow, so without this every
// shader that reads c252..c255 reads zero. It is compared by pointer rather
// than by value because the pack deduplicates identical pools, so two shaders
// sharing one are genuinely the same 64 bytes.
bool UploadConstantBank(RenderDevice* device, const uint8_t* source, uint32_t bytes,
                        ConstantCacheEntry& cache, RenderBufferReference* out,
                        const uint8_t* literals = nullptr, bool* uploaded = nullptr) {
  if (uploaded)
    *uploaded = false;
  if (cache.valid && cache.raw.size() == bytes && cache.literals == literals &&
      std::memcmp(cache.raw.data(), source, bytes) == 0) {
    *out = cache.ref;
    return true;
  }
  if (uploaded)
    *uploaded = true;

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

  // The pool occupies the last four float4s of a 256 entry bank. A bank that is
  // not the float bank (the bool/loop one) never gets a pool, so the size check
  // is what keeps this from writing past a smaller allocation.
  constexpr uint32_t kLiteralBytes = 64;
  if (literals != nullptr && bytes >= kLiteralBytes)
    std::memcpy(out_bytes + bytes - kLiteralBytes, literals, kLiteralBytes);

  cache.raw.assign(source, source + bytes);
  cache.literals = literals;
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

namespace {
bool RecordGuestDraw(const GuestDrawCall& call);
}  // namespace

// The draw, plus the shader debugger's two hooks into it: a shader switched off
// in the F2 overlay drops every draw that binds it, and one that draws is
// marked active (and timed, while the overlay has profiling on). Both live out
// here rather than inside the recording below so that a dropped draw still
// counts as a request and a shader is only marked active when it really drew.
bool IssueGuestDraw(const GuestDrawCall& call) {
  ProfileZone zone(kPhaseDraw);
  ++g_draws_requested;

  int vertex_slot = -1, pixel_slot = -1;
  if (call.pipeline != nullptr)
    GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
  if (GuestShaderDrawDisabled(vertex_slot, pixel_slot)) {
    Drop(kDropShaderDisabled, "toggled off in the F2 shader debugger");
    return false;
  }

  // See WaterOnlySurface. Counted before the test so that the surface a draw is
  // given keeps its identity whichever one is being kept.
  if (const int only = WaterOnlySurface(); only >= 0) {
    const WaterProbeKnobs& knobs = WaterProbe();
    if (vertex_slot == knobs.vertex_slot &&
        (knobs.pixel_slot < 0 || pixel_slot == knobs.pixel_slot)) {
      if (int(g_water_draw_index++) != only)
        return false;
    }
  }

  const bool profiling = GuestShaderProfilingEnabled();
  const std::chrono::steady_clock::time_point started =
      profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

  if (!RecordGuestDraw(call))
    return false;

  uint64_t elapsed_ns = 0;
  if (profiling) {
    const auto delta = std::chrono::steady_clock::now() - started;
    // Zero means "not profiling" to the registry, so a draw too fast for the
    // clock is charged one nanosecond rather than being silently uncounted.
    elapsed_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
    elapsed_ns = elapsed_ns != 0 ? elapsed_ns : 1;
  }
  NoteGuestShaderDraw(vertex_slot, pixel_slot, elapsed_ns);
  return true;
}

namespace {

bool RecordGuestDraw(const GuestDrawCall& call) {
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
  bool have_targets;
  {
    ProfileZone targets_zone(kPhaseBindTargets);
    have_targets = FrameBindDrawTargets(commands, &target_width, &target_height) != nullptr;
  }
  if (!have_targets) {
    Drop(kDropNoTarget, "no colour or depth surface is bound, so there is nowhere to draw");
    return false;
  }

  const bool rect_list = call.primitive_type == 8;
  const bool quad_list = call.primitive_type == 13;

  // The vertex streams. Each host slot is filled from the guest stream the
  // pipeline recorded for it, or from the zero buffer for the missing-attribute
  // slot.
  uint32_t slot_count = 0;
  const RenderInputSlot* slots = GuestPipelineInputSlots(call.pipeline, &slot_count);
  const uint32_t* slot_streams = GuestPipelineSlotStreams(call.pipeline, nullptr);

  RenderVertexBufferView views[kMaxPipelineStreams + 2];
  uint32_t draw_count = call.count;
  bool expanded = false;

  // The unnormalised integer elements, if this pipeline has any. They are read
  // out of one of the guest's streams and written into a host stream of their
  // own; see kWidenedInputSlot.
  uint32_t widened_count = 0, widened_stream_index = 0, widened_stride = 0;
  const GuestWidenedElement* widened = GuestPipelineWidenedElements(
      call.pipeline, &widened_count, &widened_stream_index, &widened_stride);

  // Marks a widened upload in the per-frame stream cache. Distinct from the two
  // values the ordinary path uses (0, and 8 for a rectangle expansion), because
  // the two uploads share a source pointer and are not interchangeable.
  constexpr uint32_t kWidenedCacheKind = 0x10u;

  {
  ProfileZone stream_zone(kPhaseStreamSetup);
  for (uint32_t slot = 0; slot < slot_count; ++slot) {
    const uint32_t stream_index = slot_streams[slot];
    if (stream_index == kNullInputSlot) {
      views[slot] = RenderVertexBufferView(g_null_stream->at(0), kUploadAlignment);
      continue;
    }

    if (stream_index == kWidenedInputSlot) {
      const GuestDrawStream& source = call.streams[widened_stream_index];
      if (source.data == nullptr || source.stride == 0) {
        Drop(kDropStreamMissing, "the pipeline widens out of a stream SetStreamSource never bound");
        return false;
      }
      if (source.size > kMaxStreamBytes) {
        Drop(kDropStreamTooBig, "the resource size field at +28 is not what it is believed to be");
        return false;
      }
      for (uint32_t w = 0; w < widened_count; ++w) {
        if (widened[w].guest_offset + 4 > source.stride) {
          Drop(kDropVertexFormat, "a widened element lies past the end of the stream's vertex");
          return false;
        }
      }

      uint32_t vertices = call.indexed ? source.size / source.stride : call.count;
      if (!call.indexed && vertices * source.stride > source.size)
        vertices = source.size / source.stride;
      if (vertices == 0)
        continue;
      const uint64_t bytes = uint64_t(vertices) * widened_stride;

      const StreamCacheKey widened_key{source.data, uint32_t(bytes), widened_stride,
                                       kWidenedCacheKind};
      if (const auto found = g_stream_cache.find(widened_key); found != g_stream_cache.end()) {
        ++g_stream_cache_hits;
        views[slot] = RenderVertexBufferView(found->second.ref, found->second.size);
        continue;
      }

      ProfileZone upload_zone(kPhaseVertexUpload);
      const Allocation allocation = ArenaAllocate(device, bytes);
      if (!allocation) {
        Drop(kDropNoArena, "the widened attribute stream could not be uploaded");
        return false;
      }

      // Built in ordinary memory and written out once, for the same reason the
      // ordinary stream copy below is: the arena is write-combined, and reading
      // back out of it costs two orders of magnitude more than a read from RAM.
      g_swap_scratch.resize(size_t(bytes));
      for (uint32_t v = 0; v < vertices; ++v) {
        const uint8_t* vertex = source.data + size_t(v) * source.stride;
        uint8_t* out = g_swap_scratch.data() + size_t(v) * widened_stride;
        for (uint32_t w = 0; w < widened_count; ++w) {
          // k_8_8_8_8 is one packed word, so it swaps as a dword and x is the
          // low byte afterwards, the same convention k_2_10_10_10 follows.
          uint32_t packed;
          std::memcpy(&packed, vertex + widened[w].guest_offset, 4);
          packed = Swap32(packed);
          const bool is_signed = ((widened[w].type >> 8) & 1u) != 0;
          uint8_t* dest = out + widened[w].host_offset;
          for (uint32_t c = 0; c < 4; ++c) {
            const uint8_t raw = uint8_t((packed >> (8u * c)) & 0xFFu);
            const uint16_t half = FloatToHalf(is_signed ? float(int8_t(raw)) : float(raw));
            std::memcpy(dest + 2 * c, &half, 2);
          }
        }
      }
      std::memcpy(allocation.cpu, g_swap_scratch.data(), size_t(bytes));

      g_vertex_bytes += bytes;
      views[slot] = RenderVertexBufferView(allocation.ref, uint32_t(bytes));

      g_stream_cache.emplace(widened_key, StreamCacheEntry{allocation.ref, uint32_t(bytes)});
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
    {
      ProfileZone plan_zone(kPhaseSwapPlan);
      if (!BuildSwapPlan(*call.declaration, stream_index, &plan)) {
        Drop(kDropVertexFormat, "the pipeline accepted a format the swap table does not know");
        return false;
      }
    }

    // An indexed draw indexes the whole bound buffer, so the whole bound buffer
    // is what has to be there; a non-indexed one reads exactly what it draws.
    // The rectangle expansion turns each triple into two triangles, which is
    // where the vertex count changes.
    // An indexed quad list expands its *indices* instead, below, so only the
    // non-indexed case duplicates vertices here.
    const bool expand = rect_list && !call.indexed && slot_count == 1 && call.count >= 3;
    const bool expand_quads = quad_list && !call.indexed && slot_count == 1 && call.count >= 4;
    uint32_t source_vertices = call.indexed ? stream.size / stream.stride : call.count;
    if (source_vertices == 0)
      continue;
    if (!call.indexed && source_vertices * stream.stride > stream.size)
      source_vertices = stream.size / stream.stride;

    const uint32_t triples = expand ? source_vertices / 3 : 0;
    const uint32_t quads = expand_quads ? source_vertices / 4 : 0;
    const uint32_t out_vertices =
        expand ? triples * 6 : expand_quads ? quads * 6 : source_vertices;

    // Which expansion produced the upload, so the per-frame stream cache never
    // hands a rectangle expansion to a quad draw off the same source pointer.
    const uint32_t cache_kind = expand ? 8u : expand_quads ? 13u : 0u;
    const uint64_t out_bytes = uint64_t(out_vertices) * stream.stride;
    if (out_bytes == 0)
      continue;

    // The last input to this draw that has never been measured. Every constant,
    // bool, binding and register on the flickering surface is identical frame to
    // frame, so if anything about it varies it is the geometry -- and the guest
    // animating a water mesh in place is exactly the shape that would not show
    // up anywhere else. Hashed from the guest bytes before the swap, so this is
    // what the guest wrote rather than what was uploaded.
    //
    // Note the stream cache below keys on the source *pointer*, not on its
    // contents, which is safe only because the cache is cleared every frame.
    // IsWaterProbeDraw, not IsWaterPairDraw. This hash is read back only as the
    // vhash= field of the probe's per frame line, so it is logging, and the
    // rule stated above IsWaterPairDraw is that only logging keys off the probe
    // being on. Keyed off the pair alone it ran whenever the scene used that
    // vertex shader, probe or not, hashing the whole stream a byte at a time.
    if (IsWaterProbeDraw(call)) {
      ProfileZone water_zone(kPhaseWaterHash);
      const uint64_t bytes = uint64_t(source_vertices) * stream.stride;
      uint64_t hash = 1469598103934665603ull;
      for (uint64_t i = 0; i < bytes; ++i) {
        hash ^= stream.data[i];
        hash *= 1099511628211ull;
      }
      g_water_pending_vertex_hash ^= hash;
    }

    // Reuse the upload when the same bytes were already uploaded this frame.
    // A run of draws out of one mesh buffer is the common shape here, and the
    // swap is the expensive part.
    const StreamCacheKey stream_key{stream.data, uint32_t(out_bytes), stream.stride, cache_kind};
    const auto cached = g_stream_cache.find(stream_key);

    if (cached != g_stream_cache.end()) {
      ++g_stream_cache_hits;
      views[slot] = RenderVertexBufferView(cached->second.ref, cached->second.size);
    } else {
      ProfileZone upload_zone(kPhaseVertexUpload);
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
          // Emit both halves with the same winding. The three source vertices
          // arrive in arbitrary rectangle-list order, so first/second form the
          // diagonal and opposite is mirrored to make vertex 3. Starting each
          // triangle at opposite makes their winding agree for every diagonal.
          const uint32_t order[6] = {opposite, first, 3, opposite, 3, second};
          for (uint32_t v = 0; v < 6; ++v)
            std::memcpy(out + size_t(v) * stride, scratch[order[v]], stride);
        }
        if (!small_enough) {
          // A rect list vertex wider than the scratch. Nothing in this title has
          // one, and falling back to the unexpanded triples keeps the draw
          // rather than losing it.
          ++g_rect_fallbacks;
          const size_t bytes = size_t(triples) * 3 * stride;
          g_swap_scratch.resize(bytes);
          std::memcpy(g_swap_scratch.data(), stream.data, bytes);
          for (uint32_t v = 0; v < triples * 3; ++v)
            SwapVertex(g_swap_scratch.data() + size_t(v) * stride, plan);
          std::memcpy(allocation.cpu, g_swap_scratch.data(), bytes);
        }
      } else if (expand_quads) {
        // A quad list supplies all four corners in winding order, so unlike a
        // rectangle list nothing has to be synthesised and no diagonal has to
        // be worked out: each quad becomes the two triangles (0,1,2) and
        // (0,2,3), which share the 0-2 diagonal and therefore wind the same
        // way. Swapped in cached memory and written to the arena once, for the
        // write-combined reason spelled out in the ordinary path below.
        const uint32_t stride = stream.stride;
        const size_t source_bytes = size_t(quads) * 4 * stride;
        g_swap_scratch.resize(source_bytes);
        std::memcpy(g_swap_scratch.data(), stream.data, source_bytes);
        for (uint32_t v = 0; v < quads * 4; ++v)
          SwapVertex(g_swap_scratch.data() + size_t(v) * stride, plan);

        static constexpr uint32_t kQuadOrder[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t quad = 0; quad < quads; ++quad) {
          const uint8_t* source = g_swap_scratch.data() + size_t(quad) * 4 * stride;
          uint8_t* out = allocation.cpu + size_t(quad) * 6 * stride;
          for (uint32_t v = 0; v < 6; ++v)
            std::memcpy(out + size_t(v) * stride, source + size_t(kQuadOrder[v]) * stride, stride);
        }
      } else {
        // Swapped in ordinary memory and written out once, rather than swapped
        // in place in the arena.
        //
        // The arena is an upload heap, which is write-combined: writes to it
        // are cheap and coalesced, but *reads* are uncached and cost about two
        // orders of magnitude more than a read from RAM. Swapping in place
        // reads every vertex back out of it, and that alone was 76% of the
        // frame (93 us per upload, 420 ns per vertex) on the in-game scenes.
        // The extra pass over cached memory here is far cheaper than the reads
        // it removes. The rect expansion path above already had this shape, via
        // its own stack scratch, which is why it never showed the cost.
        g_swap_scratch.resize(size_t(out_bytes));
        std::memcpy(g_swap_scratch.data(), stream.data, size_t(out_bytes));
        for (uint32_t v = 0; v < out_vertices; ++v)
          SwapVertex(g_swap_scratch.data() + size_t(v) * stream.stride, plan);
        std::memcpy(allocation.cpu, g_swap_scratch.data(), size_t(out_bytes));
      }

      g_vertex_bytes += out_bytes;
      views[slot] = RenderVertexBufferView(allocation.ref, uint32_t(out_bytes));

      g_stream_cache.emplace(stream_key, StreamCacheEntry{allocation.ref, uint32_t(out_bytes)});
    }

    if (expand || expand_quads) {
      draw_count = out_vertices;
      expanded = true;
    }
  }
  }

  if (quad_list) {
    ++g_quad_draws;
    if (call.indexed)
      ++g_quad_indexed_draws;
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
  uint32_t index_count = call.count;
  if (call.indexed) {
    ProfileZone index_zone(kPhaseIndexUpload);
    if (call.indices == nullptr) {
      Drop(kDropIndicesMissing, "SetIndices was never called, or its buffer decodes as null");
      return false;
    }
    // An indexed quad list expands here rather than in the vertex path: the
    // four corners are already distinct entries in the index buffer, so each
    // quad becomes six indices in the same (0,1,2)(0,2,3) order the
    // non-indexed expansion uses, and the vertex buffer is left alone.
    const uint32_t quads = quad_list ? call.count / 4 : 0;
    index_count = quad_list ? quads * 6 : call.count;
    const uint32_t index_stride = call.index_32bit ? 4u : 2u;
    const uint32_t index_bytes = index_count * index_stride;
    const Allocation allocation = ArenaAllocate(device, index_bytes);
    if (!allocation) {
      Drop(kDropNoArena, "an index upload could not be allocated");
      return false;
    }

    // Where output index i reads from in the guest's buffer. Identity for
    // everything except a quad list.
    const auto source_index = [&](uint32_t i) -> uint32_t {
      if (!quad_list)
        return i;
      static constexpr uint32_t kQuadOrder[6] = {0, 1, 2, 0, 2, 3};
      return (i / 6) * 4 + kQuadOrder[i % 6];
    };

    if (call.index_32bit) {
      for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t value;
        std::memcpy(&value, call.indices + 4 * source_index(i), 4);
        value = Swap32(value);
        std::memcpy(allocation.cpu + 4 * i, &value, 4);
      }
    } else {
      for (uint32_t i = 0; i < index_count; ++i) {
        uint16_t value;
        std::memcpy(&value, call.indices + 2 * source_index(i), 2);
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
  bool constants_ok;
  bool pixel_bank_uploaded = false;
  {
    ProfileZone constant_zone(kPhaseConstantUpload);
    bool bool_patched = false;
    const uint8_t* bool_bank = WaterCutBoolBank(call, &bool_patched);
    bool vertex_patched = false;
    const uint8_t* vertex_bank = WaterSlowVertexBank(call, &vertex_patched);
    constants_ok =
        UploadConstantBank(device, vertex_bank, d3d::kConstantRegisters * 16,
                           g_constant_cache[vertex_patched ? 4 : 0], &vertex_floats,
                           GuestPipelineVertexLiterals(call.pipeline)) &&
        UploadConstantBank(device, call.device + d3d::kPixelConstantShadow,
                           d3d::kConstantRegisters * 16, g_constant_cache[1], &pixel_floats,
                           GuestPipelinePixelLiterals(call.pipeline), &pixel_bank_uploaded) &&
        UploadConstantBank(device, bool_bank, d3d::kBoolLoopConstantBytes,
                           g_constant_cache[bool_patched ? 3 : 2], &bool_loops);
  }
  if (!constants_ok) {
    Drop(kDropNoArena, "a constant bank could not be uploaded");
    return false;
  }

  // See g_pixel_bank_zero. Only the four literal pool registers are skipped;
  // everything below them is the guest's, and all of it being zero says the
  // mirror is reading a shadow the guest does not write.
  if (pixel_bank_uploaded) {
    constexpr uint32_t kGuestOwnedBytes = (d3d::kConstantRegisters - 4) * 16;
    const uint8_t* bank = call.device + d3d::kPixelConstantShadow;
    g_pixel_bank_zero = true;
    for (uint32_t i = 0; i < kGuestOwnedBytes; ++i) {
      if (bank[i] != 0) {
        g_pixel_bank_zero = false;
        break;
      }
    }
  }
  if (g_pixel_bank_zero) {
    ++g_zero_pixel_bank_draws;
    int vertex_slot = -1, pixel_slot = -1;
    GuestPipelineShaderSlots(call.pipeline, &vertex_slot, &pixel_slot);
    if (pixel_slot >= 0) {
      bool seen = false;
      for (uint32_t i = 0; i < g_zero_pixel_bank_slot_count; ++i)
        seen = seen || g_zero_pixel_bank_slots[i] == uint32_t(pixel_slot);
      if (!seen && g_zero_pixel_bank_slot_count < std::size(g_zero_pixel_bank_slots)) {
        g_zero_pixel_bank_slots[g_zero_pixel_bank_slot_count++] = uint32_t(pixel_slot);
        REXLOG_WARN(
            "native_renderer: vs={} ps={} draws with an all-zero pixel float bank; any constant "
            "it modulates by reads 0",
            vertex_slot, pixel_slot);
      }
    }
  }

  DumpConstantsForShader(call);
  WaterProbeNoteConstants(call);

  // The alpha test. Not a guest bank: these are host-order values built from
  // RB_COLORCONTROL and RB_ALPHA_REF, so nothing here swaps. The disabled case
  // is sent as ALWAYS rather than skipped, because the buffer has to hold
  // something and a stale enabled test would discard the whole draw.
  //
  // Reused between draws while the state holds, the same way the constant banks
  // are: a 256 byte root CBV allocation per draw would otherwise be the largest
  // single consumer of the arena, and this changes far less often than it is
  // read.
  const bool alpha_enabled = call.state.valid && call.state.alpha_test_enabled;
  if (alpha_enabled)
    ++g_alpha_test_draws;
  const uint32_t alpha_func = alpha_enabled ? uint32_t(call.state.alpha_func) : 7u;  // 7 is ALWAYS
  if (!g_alpha_cache.valid || g_alpha_cache.func != alpha_func ||
      g_alpha_cache.ref != call.state.alpha_ref) {
    const Allocation allocation = ArenaAllocate(device, kUploadAlignment);
    if (!allocation) {
      Drop(kDropNoArena, "the alpha test constants could not be uploaded");
      return false;
    }
    std::memcpy(allocation.cpu, &alpha_func, 4);
    std::memcpy(allocation.cpu + 4, &call.state.alpha_ref, 4);
    std::memset(allocation.cpu + 8, 0, 8);
    g_alpha_cache.func = alpha_func;
    g_alpha_cache.ref = call.state.alpha_ref;
    g_alpha_cache.buffer = allocation.ref;
    g_alpha_cache.valid = true;
    g_constant_bytes += kUploadAlignment;
  }
  const RenderBufferReference alpha_test = g_alpha_cache.buffer;

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
  // Shifted half a pixel down and right, because the two APIs disagree about
  // where a pixel's centre is. D3D9 and the Xenos put it on the integer
  // coordinate; D3D12 puts it at the half. Interpolants are evaluated at that
  // centre, so the same geometry sampled through the same UVs lands half a
  // pixel further along here than it did on the console.
  //
  // The sign is worth deriving rather than recalling, and the obvious rule of
  // thumb ("port D3D9 to D3D11 by offsetting vertices -0.5") gets it backwards
  // here. Measured: a quad whose left edge is at 0 interpolates t =
  // (i+0.5)/64 at host pixel i, and the guest's baked half texel takes that to
  // (i+1)/64. The console, sampling at integer centres, gets t = i/64 and so
  // lands on (i+0.5)/64, the centre of texel i. Reproducing that needs the
  // edge at +0.5, not -0.5.
  //
  // Note that -0.5 is not merely useless but invisible: it takes the UV to
  // (i+1.5)/64, which point sampling floors to the same texel i+1 as the
  // unfixed path. A capture cannot tell the two apart, so do not read "the
  // capture is unchanged" as "the viewport is not the lever".
  //
  // Titles compensate for the console's convention by baking half a texel into
  // their screen-space UVs, and this one does. Left alone the two biases add:
  // the ripple simulation's five tap stencil asks for texel i and gets exactly
  // the boundary between i and i+1, which point sampling resolves to i+1. That
  // reads the whole stencil, prev-prev tap included, one texel diagonally away
  // from the texel being written. Measured over eight texels of a capture, and
  // it is fatal rather than blurry: at the axis-aligned Nyquist modes the
  // amplification polynomial becomes l^2 + 1.9289l - 1, whose root at -2.354
  // grows 2.35x per frame while flipping sign. That is the water flicker, and
  // the saturation and the frame-rate independence both follow from it.
  //
  // Moving the viewport rather than the vertices puts it before the rasteriser
  // for every draw, which is what makes it correct for the passes that never
  // touch a screen-space UV as well.
  commands->setViewports(
      RenderViewport(x + 0.5f, y + 0.5f, width, height, g_viewport.min_z, g_viewport.max_z));
  commands->setScissors(
      RenderRect(int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height)));

  commands->setGraphicsPipelineLayout(GuestPipelineLayout());
  commands->setPipeline(GuestPipelineObject(call.pipeline));
  commands->setGraphicsRootDescriptor(vertex_floats, kRootVertexFloatConstants);
  commands->setGraphicsRootDescriptor(bool_loops, kRootVertexBoolConstants);
  commands->setGraphicsRootDescriptor(pixel_floats, kRootPixelFloatConstants);
  commands->setGraphicsRootDescriptor(bool_loops, kRootPixelBoolConstants);
  commands->setGraphicsRootDescriptor(alpha_test, kRootAlphaTest);

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
  //
  // The sampler comes out of the same six dwords as the texture, because on
  // this hardware it *is* the same six dwords: the three SetSamplerState_*
  // setters patch fields inside the fetch constant rather than writing a
  // register. A slot the shader does not declare keeps the fallback sampler,
  // for the same reason it keeps the white texture.
  // All of that is derived from the fetch constants and the shader's slot mask,
  // and neither moves between most consecutive draws: this title issues roughly
  // three draws per SetTexture call. So the whole loop, and the two set lookups
  // under it, are cached against the fetch constants they were built from.
  //
  // Keyed on what the bindings are actually made of rather than on the binding
  // generation. The generation moves on every SetTexture, and this title issues
  // roughly three draws per SetTexture while frequently setting the same
  // texture back, so keying on it missed about 39% of the time and paid the
  // whole loop again for bindings that had not moved. The signature is a hash
  // of the raw fetch constants of the declared slots, which covers everything
  // SetTexture and the three sampler setters can do; the content epoch covers
  // the two things they cannot, a resolve replacing the host image behind an
  // unchanged address and the frame boundary.
  //
  // That frame boundary is what keeps this honest: the texture mirror refreshes
  // a rewritten texture at most once per texture per frame, so a cache that
  // survived one would freeze the font atlas, which is a bug this renderer has
  // already had once. Within a frame, skipping the mirror on identical fetch
  // constants is exactly equivalent, since the second visit would find the
  // refresh already throttled out.
  const uint32_t texture_mask = GuestPipelineTextureMask(call.pipeline);
  const uint64_t content_epoch = TextureContentEpoch();
  const uint64_t fetch_signature = BoundTextureFetchSignature(call.memory_base, texture_mask);

  // Deliberately outside the binding cache below: the first draw of a frame to
  // declare the probe slot may well be a cache hit, and then the loop that
  // decodes fetch constants never runs. One extra decode per frame.
  {
    ProfileZone probe_zone(kPhaseProjectionProbe);
    CaptureProjectionProbe(call, texture_mask);
  }

  RenderDescriptorSet* texture_set;
  RenderDescriptorSet* sampler_set;
  // WaterPinT12 rewrites a fetch constant *after* it is read, so a draw under
  // it does not describe itself; the pin is a debug knob and is zero in a
  // shipped frame, so the cache is simply stood down while it is set.
  const bool cacheable = fetch_signature != 0 && WaterPinT12() == 0;
  if (cacheable && g_binding_cache.valid && g_binding_cache.epoch == content_epoch &&
      g_binding_cache.signature == fetch_signature && g_binding_cache.mask == texture_mask) {
    ++g_binding_cache_hits;
    texture_set = g_binding_cache.texture_set;
    sampler_set = g_binding_cache.sampler_set;
  } else {
    BindingKey textures;
    BindingKey samplers;
    {
      ProfileZone texture_zone(kPhaseTextureBind);
      TextureFetch probe_fetches[kTextureSlots];
      bool probe_have_fetch[kTextureSlots] = {};
      for (uint32_t stage = 0; stage < kTextureSlots; ++stage) {
        RenderTexture* texture = nullptr;
        RenderSampler* sampler = g_sampler.get();
        TextureFetch fetch;
        GuestSamplerState sampler_state;
        bool have_fetch = false;
        if ((texture_mask & (1u << stage)) != 0) {
          ProfileZone fetch_zone(kPhaseFetchDecode);
          have_fetch = GetBoundTextureFetch(call.memory_base, stage, fetch, &sampler_state);
        }
        // See WaterPinT12. Rewritten before the mirror lookup so the pinned
        // buffer is what is decoded as well as what is bound.
        if (have_fetch && stage == 12) {
          if (const uint32_t pinned = WaterPinT12();
              pinned != 0 && IsWaterPairDraw(call) && fetch.base_address != pinned) {
            fetch.base_address = pinned;
          }
        }
        if (have_fetch) {
          {
            ProfileZone lookup_zone(kPhaseMirrorLookup);
            texture = static_cast<RenderTexture*>(TextureMirrorLookup(call.memory_base, fetch));
          }
          ProfileZone sampler_zone(kPhaseAcquireSampler);
          sampler = AcquireSampler(device, sampler_state);
        }
        textures.slots[stage] = texture != nullptr ? texture : g_white_texture.get();
        samplers.slots[stage] = sampler;
        probe_fetches[stage] = fetch;
        probe_have_fetch[stage] = have_fetch;
      }
      WaterProbeNoteTextures(call, texture_mask, textures, g_white_texture.get(), probe_fetches,
                             probe_have_fetch);
    }

    // Sets matching these bindings, not a rewrite of shared ones. See the note
    // above AcquireBindingSet: rewriting would change what every draw already
    // recorded this frame reads, not what this draw reads.
    ProfileZone set_zone(kPhaseDescriptorSet);
    texture_set = AcquireBindingSet(device, g_texture_sets, kMaxTextureSets, false, textures,
                                    &g_texture_set_misses);
    sampler_set = AcquireBindingSet(device, g_sampler_sets, kMaxSamplerSets, true, samplers,
                                    &g_sampler_set_misses);
    ++g_binding_cache_misses;
    g_binding_cache.epoch = content_epoch;
    g_binding_cache.signature = fetch_signature;
    g_binding_cache.mask = texture_mask;
    g_binding_cache.texture_set = texture_set;
    g_binding_cache.sampler_set = sampler_set;
    // A failed lookup is not worth remembering, and caching a null set would
    // hand the next draw the same failure without retrying it.
    g_binding_cache.valid = cacheable && texture_set != nullptr && sampler_set != nullptr;
  }
  if (texture_set == nullptr || sampler_set == nullptr) {
    Drop(kDropNoArena, "a descriptor set could not be created; the heap behind it is full");
    return false;
  }

  {
    ProfileZone submit_zone(kPhaseSubmit);
    commands->setGraphicsDescriptorSet(texture_set, kTextureDescriptorSet);
    commands->setGraphicsDescriptorSet(sampler_set, kSamplerDescriptorSet);
    commands->setVertexBuffers(0, views, slot_count, slots);

    if (call.indexed) {
      commands->setIndexBuffer(&index_view);
      commands->drawIndexedInstanced(index_count, 1, 0, call.base_vertex, 0);
    } else {
      commands->drawInstanced(draw_count, 1, 0, 0);
    }
  }

  ++g_draws_issued;
  return true;
}

}  // namespace

void BeginGuestDrawFrame(uint32_t slot) {
  g_arena_slot = slot < kFramesInFlight ? slot : 0;
  for (ArenaBlock& block : Arena())
    block.used = 0;
  g_arena_block = 0;

  // These three hold references into the arena that was just recycled, so they
  // have to go with it. They are per recording rather than per slot: only one
  // frame is being recorded at a time, so there is never a second frame's worth
  // of them to keep.
  g_stream_cache.clear();
  for (ConstantCacheEntry& cache : g_constant_cache)
    cache.valid = false;
  g_alpha_cache.valid = false;

  // Safe here and only here: the caller has waited on this slot's fence, so the
  // frame that recorded draws against these sets is done with them.
  g_arenas[g_arena_slot].transient_sets.clear();

  // A cached set may be one of those, so it cannot outlive them. The frame
  // boundary bumps the content epoch too, which would invalidate this on
  // its own; this is here so the lifetime does not depend on that ordering.
  g_binding_cache.valid = false;

  // Latch again next frame, so the probe follows the camera rather than
  // reporting whatever the first frame of the run happened to bind.
  g_projection_probe.captured = false;

  g_water_surface = 0;
  g_water_draw_index = 0;
  WaterProbeEndFrame();
}

void LogGuestDrawSummary() {
  uint64_t dropped = 0;
  for (uint64_t count : g_drops)
    dropped += count;

  REXLOG_INFO(
      "native_renderer: draws issued={} requested={} dropped={} | uploaded {} KiB vertices, {} "
      "KiB indices, {} KiB constants | stream cache hits={} | arena {} block(s) | rect lists={} "
      "(unexpanded {}) | quad lists={} (indexed {})",
      g_draws_issued, g_draws_requested, dropped, g_vertex_bytes / 1024, g_index_bytes / 1024,
      g_constant_bytes / 1024, g_stream_cache_hits, Arena().size(), g_rect_draws, g_rect_fallbacks,
      g_quad_draws, g_quad_indexed_draws);

  for (uint32_t i = 0; i < kDropCount; ++i) {
    if (g_drops[i] != 0)
      REXLOG_INFO("native_renderer:   dropped {}x: {}", g_drops[i], kDropNames[i]);
  }

  REXLOG_INFO(
      "native_renderer:   samplers={} (overflowed {}x, inexact clamp mode {}x, alpha test {}x) | "
      "descriptor sets: texture={} (misses {}) sampler={} (misses {}) transient={} failed={}",
      g_sampler_count, g_sampler_overflow, g_clamp_inexact, g_alpha_test_draws,
      g_texture_sets.size(), g_texture_set_misses, g_sampler_sets.size(), g_sampler_set_misses,
      g_texture_set_transient, g_binding_set_failed);

  REXLOG_INFO("native_renderer:   binding cache: hits={} misses={}", g_binding_cache_hits,
              g_binding_cache_misses);

  if (g_zero_pixel_bank_draws != 0) {
    REXLOG_INFO("native_renderer:   {} draw(s) read an all-zero pixel float bank, over {} shader(s)",
                g_zero_pixel_bank_draws, g_zero_pixel_bank_slot_count);
  }

  if (g_projection_probe.valid) {
    const ProjectionProbe& probe = g_projection_probe;
    REXLOG_INFO(
        "native_renderer:   projection probe, slot {}: address u={} v={} w={} | mask 0x{:08X} "
        "{}x{} format {}",
        kProbeSlot, kClampNames[probe.sampler.clamp_x & 7u],
        kClampNames[probe.sampler.clamp_y & 7u], kClampNames[probe.sampler.clamp_z & 7u],
        probe.fetch.base_address, probe.fetch.width, probe.fetch.height, probe.fetch.format);
    REXLOG_INFO(
        "native_renderer:     light loop i16 = 0x{:08X} (count={}) | c{} = {} {} {} {}",
        probe.loop, probe.loop & 0xFFu, kProbeLightScale, probe.light_scale[0],
        probe.light_scale[1], probe.light_scale[2], probe.light_scale[3]);
    for (uint32_t row = 0; row < kProbeConstantCount; ++row) {
      REXLOG_INFO("native_renderer:     c{} = {: .6f} {: .6f} {: .6f} {: .6f}",
                  kProbeConstant + row, probe.matrix[row][0], probe.matrix[row][1],
                  probe.matrix[row][2], probe.matrix[row][3]);
    }
  }
}

void LogProfileSummary() {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t wall_ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_profile_window_start).count());
  if (wall_ns == 0)
    return;

  // Only the top level phases count against the wall clock; the nested ones are
  // already inside one of those.
  uint64_t accounted = 0;
  for (uint32_t i = 0; i < kPhaseCount; ++i) {
    if (!PhaseIsNested(i))
      accounted += g_profile_ns[i];
  }

  REXLOG_INFO("native_renderer: frame time over {:.1f} ms of wall clock, {:.2f} ms/frame average",
              double(wall_ns) / 1e6, double(wall_ns) / 1e6 / 300.0);
  for (uint32_t i = 0; i < kPhaseCount; ++i) {
    if (g_profile_hits[i] == 0)
      continue;
    REXLOG_INFO("native_renderer:   {:<18} {:7.2f} ms/frame {:5.1f}% over {} call(s), {:.2f} us each",
                kPhaseNames[i], double(g_profile_ns[i]) / 1e6 / 300.0,
                100.0 * double(g_profile_ns[i]) / double(wall_ns), g_profile_hits[i],
                double(g_profile_ns[i]) / 1e3 / double(g_profile_hits[i]));
  }
  REXLOG_INFO("native_renderer:   {:<18} {:7.2f} ms/frame {:5.1f}% (guest CPU and anything not instrumented)",
              "other", double(wall_ns - accounted) / 1e6 / 300.0,
              100.0 * double(wall_ns - accounted) / double(wall_ns));

  // Not part of the accounting above, and deliberately printed after `other`:
  // this is the same wall clock measured on the other processor, so it overlaps
  // every CPU phase rather than adding to them.
  if (g_gpu_frame_count != 0) {
    REXLOG_INFO("native_renderer:   {:<18} {:7.2f} ms/frame {:5.1f}% of wall clock, over {} timed frame(s)",
                "gpu", double(g_gpu_frame_ns) / 1e6 / double(g_gpu_frame_count),
                100.0 * double(g_gpu_frame_ns) * 300.0 /
                    (double(g_gpu_frame_count) * double(wall_ns)),
                g_gpu_frame_count);
  }
  g_gpu_frame_ns = 0;
  g_gpu_frame_count = 0;

  for (uint32_t i = 0; i < kPhaseCount; ++i) {
    g_profile_ns[i] = 0;
    g_profile_hits[i] = 0;
  }
  g_profile_window_start = now;
}

void ShutdownGuestDraws() {
  for (FrameArena& arena : g_arenas) {
    for (ArenaBlock& block : arena.blocks) {
      if (block.mapped != nullptr)
        block.buffer->unmap();
    }
    arena.blocks.clear();
    arena.transient_sets.clear();
  }
  g_arena_block = 0;
  g_stream_cache.clear();
  for (ConstantCacheEntry& cache : g_constant_cache) {
    cache.valid = false;
    cache.raw.clear();
  }
  g_alpha_cache.valid = false;
  g_texture_sets.clear();
  g_sampler_sets.clear();
  for (SamplerCacheEntry& entry : g_samplers) {
    entry.sampler.reset();
    entry.key = 0;
  }
  g_sampler_count = 0;
  g_sampler.reset();
  g_white_texture.reset();
  g_null_stream.reset();
  g_resources_ready = false;
  g_resources_failed = false;
}

}  // namespace eternalsonata
