// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_d3d.h. Observation only: no drawing happens here.

#include "native_renderer_d3d.h"

#include <atomic>
#include <cstring>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/eternalsonata_init.h"
#include "native_renderer.h"
#include "native_renderer_draw.h"
#include "native_renderer_texture.h"
#include "native_renderer_frame.h"
#include "native_renderer_pipeline.h"
#include "native_renderer_plume.h"
#include "native_renderer_profile.h"
#include "native_renderer_readback.h"
#include "native_renderer_shader_debug.h"

REX_EXTERN(__imp__D3D__CreateDevice);
REX_EXTERN(__imp__D3DDevice__SetVertexShaderConstantF);
REX_EXTERN(__imp__D3DDevice__SetPixelShaderConstantF);
REX_EXTERN(__imp__D3DDevice__SetVertexShader);
REX_EXTERN(__imp__D3DDevice__SetPixelShader);
REX_EXTERN(__imp__D3DDevice__SetTexture);
REX_EXTERN(__imp__D3DDevice__Swap);
REX_EXTERN(__imp__D3DDevice__BlockUntilGpuIdle);
REX_EXTERN(__imp__D3DDevice__BlockUntilFenceRetired);
REX_EXTERN(__imp__D3DDevice__ThrottleWait_Poll);
REX_EXTERN(__imp__D3DDevice__SetVertexShaderConstantI);
REX_EXTERN(__imp__D3DDevice__SetPixelShaderConstantI);
REX_EXTERN(__imp__D3DDevice__SetSamplerState_MinFilter);
REX_EXTERN(__imp__D3DDevice__SetSamplerState_MagFilter);
REX_EXTERN(__imp__D3DDevice__SetSamplerState_MipMapLodBias);
REX_EXTERN(__imp__D3DDevice__DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice__DrawVertices);
REX_EXTERN(__imp__D3DDevice__BeginVertices);
REX_EXTERN(__imp__D3DDevice__EndVertices);
REX_EXTERN(__imp__D3DDevice__SetStreamSource);
REX_EXTERN(__imp__D3DDevice__SetIndices);
REX_EXTERN(__imp__D3DDevice__SetVertexDeclaration);
REX_EXTERN(__imp__D3DDevice__CreateRenderTarget);
REX_EXTERN(__imp__D3DDevice__CreateTexture);
REX_EXTERN(__imp__D3DDevice__SetRenderTarget);
REX_EXTERN(__imp__D3DDevice__SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice__SetViewport);
REX_EXTERN(__imp__D3DDevice__Clear);
REX_EXTERN(__imp__D3DDevice__Resolve);

namespace eternalsonata {
namespace {

std::atomic<uint32_t> g_device{0};

// See TextureBindingGeneration in the header. Atomic only because the frame
// boundary bump can arrive from the present path; every other bump and every
// read is on the guest thread.
std::atomic<uint64_t> g_texture_binding_generation{1};

void BumpTextureBindingGeneration() {
  g_texture_binding_generation.fetch_add(1, std::memory_order_relaxed);
}

// Bound state, guest pointers. Written from the guest thread only.
uint32_t g_vertex_shader = 0;
uint32_t g_pixel_shader = 0;
uint32_t g_textures[d3d::kSamplerCount] = {};

// Host copies of the float constant banks, host float order.
float g_vs_constants[d3d::kConstantRegisters * 4] = {};
float g_ps_constants[d3d::kConstantRegisters * 4] = {};

// Which table slots have actually been bound. The summary below samples the
// currently bound shader at swap time, which is the last thing the frame drew
// with (typically UI) and so says nothing about how many distinct shaders the
// frame used; these sets do.
bool g_vs_slots_seen[d3d::kShaderTableEntries] = {};
bool g_ps_slots_seen[d3d::kShaderTableEntries] = {};
uint32_t g_vs_unresolved = 0;
uint32_t g_ps_unresolved = 0;

// Resolve a bound shader object back to its index in one of the two tables.
// Returns -1 when the object is not in the table, which would mean the game
// binds shaders it did not get from the static blob -- worth knowing, since
// the whole static compilation plan rests on that closed set.
int ResolveShaderSlot(uint8_t* base, uint32_t table, uint32_t object) {
  if (object == 0)
    return -1;
  for (uint32_t i = 0; i < d3d::kShaderTableEntries; ++i) {
    if (REX_LOAD_U32(table + 4 * i) == object)
      return static_cast<int>(i);
  }
  return -1;
}

// Does SetTexture clobber sampler state set earlier?
//
// Both write the same texture fetch constant: the three SetSamplerState_*
// setters patch fields inside it, and SetTexture writes the six dwords for the
// stage inline. Whether the second can undo the first was an open correctness
// question, so answer it from the running game rather than by staring at the
// stores. Snapshot the sampler-owned fields after a sampler setter, then
// re-read them after the next SetTexture on that stage and compare.
//
// The owned fields, from the setters' own read-modify-writes:
//   word 3: min_filter (+21), mag_filter (+19), aniso_filter (+25)
//   word 4: min/mag aniso walk (bits 11, 10), lod_bias (bits 12..21)
constexpr uint32_t kSamplerWord3Mask = 0x0E780000u;
constexpr uint32_t kSamplerWord4Mask = 0x003FFC00u;

struct SamplerSnapshot {
  bool valid = false;
  uint32_t word3 = 0;
  uint32_t word4 = 0;
};
SamplerSnapshot g_sampler_snapshot[d3d::kSamplerCount];
bool g_clobber_reported[d3d::kSamplerCount] = {};

// The tiling extent, refreshed wherever the device is in hand. BeginTiling is
// not hooked: it does not have to be, because the extent is device state and
// reading it is the same argument as reading the register shadows.
std::atomic<uint32_t> g_tiling_width{0};
std::atomic<uint32_t> g_tiling_height{0};

void UpdateTilingExtent(uint8_t* base, uint32_t device) {
  if (base == nullptr || device == 0)
    return;
  const uint32_t width = REX_LOAD_U32(device + d3d::kTilingExtentWidth);
  const uint32_t height = REX_LOAD_U32(device + d3d::kTilingExtentHeight);

  // Bounded before it is believed, the same way the surface decode is: this is
  // guest memory, and a plausible extent is the check on the offsets.
  if (width == 0 || height == 0 || width > 8192 || height > 8192)
    return;
  g_tiling_width.store(width, std::memory_order_relaxed);
  g_tiling_height.store(height, std::memory_order_relaxed);
}

uint32_t FetchConstantWord(uint8_t* base, uint32_t device, uint32_t stage, uint32_t word) {
  return REX_LOAD_U32(device + d3d::kTextureFetchConstants + d3d::kTextureFetchStride * stage +
                      4 * word);
}

void SnapshotSampler(uint8_t* base, uint32_t device, uint32_t stage) {
  if (stage >= d3d::kSamplerCount)
    return;
  auto& snap = g_sampler_snapshot[stage];
  snap.word3 = FetchConstantWord(base, device, stage, 3) & kSamplerWord3Mask;
  snap.word4 = FetchConstantWord(base, device, stage, 4) & kSamplerWord4Mask;
  snap.valid = true;
}

void CheckSamplerClobber(uint8_t* base, uint32_t device, uint32_t stage) {
  if (stage >= d3d::kSamplerCount || g_clobber_reported[stage])
    return;
  auto& snap = g_sampler_snapshot[stage];
  if (!snap.valid)
    return;

  const uint32_t word3 = FetchConstantWord(base, device, stage, 3) & kSamplerWord3Mask;
  const uint32_t word4 = FetchConstantWord(base, device, stage, 4) & kSamplerWord4Mask;
  if (word3 == snap.word3 && word4 == snap.word4)
    return;

  g_clobber_reported[stage] = true;
  REXLOG_WARN(
      "native_renderer: SetTexture CLOBBERED sampler state on stage {}: word3 0x{:08X} -> "
      "0x{:08X}, word4 0x{:08X} -> 0x{:08X}. Sampler state is not independent of texture "
      "binding; a backend must re-apply it after every bind.",
      stage, snap.word3, word3, snap.word4, word4);
}

uint64_t g_sampler_sets = 0;

// The distinct textures the game actually binds, keyed by base address. This
// is the working set a texture mirror would have to own. Bounded so a long run
// cannot grow it without limit; if the cap is ever hit that is itself worth
// knowing, so it is reported.
constexpr uint32_t kMaxTrackedTextures = 4096;
uint32_t g_texture_addresses[kMaxTrackedTextures] = {};
uint32_t g_texture_count = 0;
bool g_texture_overflow = false;
uint32_t g_formats_seen[64] = {};
uint32_t g_tiled_count = 0;
uint32_t g_linear_count = 0;
uint32_t g_logged_examples = 0;

// Base addresses that a Resolve has written to. Kept next to the texture
// tracking because the interesting question is the overlap between the two: a
// texture the game samples whose contents came out of EDRAM is a render target
// the backend has to produce, not an asset it can upload once. Small by
// construction -- a title has a handful of resolve targets, not hundreds.
constexpr uint32_t kMaxResolveTargets = 256;
uint32_t g_resolve_targets[kMaxResolveTargets] = {};
uint32_t g_resolve_target_count = 0;
bool g_resolve_target_overflow = false;

bool IsResolveDestination(uint32_t address) {
  for (uint32_t i = 0; i < g_resolve_target_count; ++i) {
    if (g_resolve_targets[i] == address)
      return true;
  }
  return false;
}

// Returns true the first time an address is seen.
bool TrackResolveDestination(uint32_t address) {
  if (IsResolveDestination(address))
    return false;
  if (g_resolve_target_count >= kMaxResolveTargets) {
    g_resolve_target_overflow = true;
    return false;
  }
  g_resolve_targets[g_resolve_target_count++] = address;
  return true;
}

// Sampled textures whose contents a resolve produced, counted two ways: how
// many distinct ones, and how many binds hit one. Distinct is tracked with its
// own list rather than off the texture set's first-seen flag, because a texture
// is often bound before the first resolve into it and would then never be
// counted.
uint32_t g_sampled_resolve_targets[kMaxResolveTargets] = {};
uint32_t g_sampled_resolve_count = 0;
uint64_t g_resolved_texture_binds = 0;

void TrackSampledResolveTarget(uint32_t address) {
  for (uint32_t i = 0; i < g_sampled_resolve_count; ++i) {
    if (g_sampled_resolve_targets[i] == address)
      return;
  }
  if (g_sampled_resolve_count < kMaxResolveTargets)
    g_sampled_resolve_targets[g_sampled_resolve_count++] = address;
}

bool TrackTexture(uint32_t address) {
  for (uint32_t i = 0; i < g_texture_count; ++i) {
    if (g_texture_addresses[i] == address)
      return false;
  }
  if (g_texture_count >= kMaxTrackedTextures) {
    g_texture_overflow = true;
    return false;
  }
  g_texture_addresses[g_texture_count++] = address;
  return true;
}

// Read the six fetch constant dwords for a stage and record what they say.
void RecordTextureFetch(uint8_t* base, uint32_t device, uint32_t stage) {
  if (stage >= d3d::kSamplerCount)
    return;

  uint32_t words[6];
  for (uint32_t i = 0; i < 6; ++i)
    words[i] = FetchConstantWord(base, device, stage, i);

  const TextureFetch fetch = DecodeTextureFetch(words);
  // Type 2 is kTexture. Anything else is an unbound or invalid slot, which
  // SetTexture(stage, nullptr) legitimately produces.
  if (fetch.type != 2 || fetch.base_address == 0)
    return;

  if (fetch.format < 64)
    ++g_formats_seen[fetch.format];
  if (fetch.tiled)
    ++g_tiled_count;
  else
    ++g_linear_count;

  const bool first_bind = TrackTexture(fetch.base_address);
  if (IsResolveDestination(fetch.base_address)) {
    ++g_resolved_texture_binds;
    TrackSampledResolveTarget(fetch.base_address);
  }

  if (first_bind && g_logged_examples < 12) {
    ++g_logged_examples;
    REXLOG_INFO(
        "native_renderer: texture stage {} -> {}x{} fmt {} {} pitch {} at 0x{:08X} "
        "(endian {})",
        stage, fetch.width, fetch.height, fetch.format, fetch.tiled ? "tiled" : "linear",
        fetch.pitch, fetch.base_address, fetch.endianness);
  }
}

uint64_t g_vs_constant_calls = 0;
uint64_t g_ps_constant_calls = 0;
uint64_t g_texture_calls = 0;
uint64_t g_shader_binds = 0;

// Loop constant traffic, in the unified numbering the microcode uses: i0..i15
// are the vertex half, i16..i31 the pixel half.
//
// A `loop` whose trip count is zero does not merely skip its lights, it leaves
// whatever the shader seeded its accumulator with in place, and in this title
// those seeds are comparisons (`sgt r8, -r0.xxxx, c255.zzzz`, and r0 is the
// position interpolator). So a zero trip count puts a step on the sign of a
// world coordinate into the frame. Reading the shadow at one draw cannot tell
// whether that zero is the guest's own intent or a write we never saw, so
// these count the writes themselves: `nonzero` is the slots that have ever
// been given a trip count at all, over the whole run.
constexpr uint32_t kLoopConstants = 32;
uint64_t g_loop_constant_calls = 0;
uint32_t g_loop_written = 0;  // bit per slot: written at all
uint32_t g_loop_nonzero = 0;  // bit per slot: given a non-zero trip count
uint8_t g_loop_max_count[kLoopConstants] = {};
uint8_t g_loop_last_count[kLoopConstants] = {};

// Both setters share this; `first` is the unified index the stage's register 0
// maps to, which is 0 for the vertex half and 16 for the pixel half.
//
// This reads the shadow *back* after the setter has run rather than decoding
// the source itself. Decoding the source needs an assumption about how the
// entry is laid out, and the setter takes single bytes out of it (a3[3], a3[7],
// a3[11]) where a byte-swapping load would report something else entirely. The
// shadow is the value the draw will read, so reading it removes the guess.
//
// The two halves are contiguous: the pixel base device+10080 is device+10016
// plus 16 registers, so one unified index addresses the whole bank.
constexpr uint32_t kLoopConstantShadow = 10016;

void RecordLoopConstants(uint8_t* base, uint32_t device, uint32_t first, uint32_t start,
                         uint32_t source, uint32_t count) {
  ++g_loop_constant_calls;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t slot = first + start + i;
    if (slot >= kLoopConstants)
      break;
    const uint32_t written = REX_LOAD_U32(device + kLoopConstantShadow + 4 * slot);
    const uint8_t trip = uint8_t(written & 0xFFu);
    g_loop_written |= 1u << slot;
    if (trip != 0)
      g_loop_nonzero |= 1u << slot;
    if (trip > g_loop_max_count[slot])
      g_loop_max_count[slot] = trip;
    g_loop_last_count[slot] = trip;
    if (written != 0) {
      REXLOG_INFO(
          "native_renderer: loop i{} <- 0x{:08X} (count={} start={} step={}) from source 0x{:08X}",
          slot, written, trip, (written >> 8) & 0xFFu, int8_t((written >> 16) & 0xFFu),
          source + 16 * i);
    }
  }
}

// The shadow offsets were derived by reading the setters' stores. Rather than
// trust that, the first call of each kind re-derives the answer from the
// running game: copy out of the shadow at the offset we believe in, and
// compare against the source buffer the caller handed us. A mismatch means
// the offset (or the stride) is wrong and everything built on it would be.
bool g_vs_shadow_checked = false;
bool g_ps_shadow_checked = false;

void CheckShadow(uint8_t* base, const char* which, uint32_t device, uint32_t shadow_offset,
                 uint32_t start, uint32_t source, uint32_t count, bool& checked) {
  if (checked || count == 0)
    return;
  checked = true;

  const uint32_t shadow = device + shadow_offset + 16 * start;
  const uint32_t bytes = count * 16;
  const bool match =
      std::memcmp(REX_RAW_ADDR(shadow), REX_RAW_ADDR(source), bytes) == 0;

  REXLOG_INFO(
      "native_renderer: {} constant shadow check: device=0x{:08X} start={} count={} "
      "shadow=0x{:08X} source=0x{:08X} -> {}",
      which, device, start, count, shadow, source, match ? "MATCH" : "MISMATCH");
}

// Pull `count` vec4s out of a guest shadow into a host float array, swapping
// each dword. The guest stores big endian, the backend wants host order.
void MirrorConstants(uint8_t* base, uint32_t shadow_base, uint32_t start, uint32_t count,
                     float* dest, uint32_t capacity) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t reg = start + i;
    if (reg >= capacity)
      break;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t raw = REX_LOAD_U32(shadow_base + 16 * reg + 4 * c);
      float value;
      std::memcpy(&value, &raw, sizeof(value));
      dest[reg * 4 + c] = value;
    }
  }
}

// ---------------------------------------------------------------------------
// The draw path.
//
// Everything above tracks binding state without ever observing a draw, so none
// of it is anchored to anything: it is a running total, not a picture of what
// one frame does. The hooks below close that gap, and while they are here they
// answer the question the static compilation plan has left open.
//
// That question is how many pipelines an offline pipeline set has to contain.
// Two numbers bound it, and they are very different numbers:
//
//   * distinct (vertex shader, pixel shader) pairs -- the program count, if
//     vertex fetch is lifted out of the shader and expressed as a host input
//     layout, which is the design the handoff argues for.
//   * distinct (vertex shader, declaration) pairs -- the variant count, if
//     fetch is left inside the shader. This is exactly the key the guest's own
//     variant cache at 0x82267D08 uses, so it is what the game itself pays.
//
// The gap between them is the cost of getting that decision wrong, measured
// rather than assumed. Both are counted below.
//
// The declaration decode is also a live check on a layout that so far exists
// only as a reading of 0x82267218's disassembly: element count at +24,
// elements from +52 with a 12 byte stride, usage at element+9. If that is
// misread, the usage bytes are garbage and the validation below says so.

// Open addressed set of packed keys, sized well past what a frame needs so
// that occupancy stays low and probes stay short. Saturates rather than grows;
// hitting the cap is reported, since a count that stopped counting is worse
// than no count.
template <uint32_t kCapacity>
class KeySet {
 public:
  // Returns true if `key` was not already present.
  bool Insert(uint64_t key) {
    if (count_ >= kCapacity * 3 / 4) {
      saturated_ = true;
      return false;
    }
    // +1 so that a zero key is distinguishable from an empty slot.
    const uint64_t stored = key + 1;
    uint32_t i = static_cast<uint32_t>(Hash(stored)) & (kCapacity - 1);
    while (slots_[i] != 0) {
      if (slots_[i] == stored)
        return false;
      i = (i + 1) & (kCapacity - 1);
    }
    slots_[i] = stored;
    ++count_;
    return true;
  }

  uint32_t count() const { return count_; }
  bool saturated() const { return saturated_; }

 private:
  static uint64_t Hash(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return x;
  }

  static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");
  uint64_t slots_[kCapacity] = {};
  uint32_t count_ = 0;
  bool saturated_ = false;
};

KeySet<8192> g_program_pairs;    // (vs slot, ps slot)
KeySet<8192> g_shader_variants;  // (vs slot, declaration identity)
KeySet<2048> g_declarations;     // declaration identity

uint64_t g_draws_indexed = 0;
uint64_t g_draws_vertices = 0;
uint64_t g_draws_inline = 0;  // BeginVertices/EndVertices blocks
uint64_t g_draw_vertices_total = 0;
uint32_t g_prim_types[16] = {};

// Declaration decode health, all of it a check on the statically read layout.
uint64_t g_decl_decode_ok = 0;
uint64_t g_decl_decode_bad = 0;
uint32_t g_decl_max_elements = 0;
uint32_t g_usages_seen[16] = {};
bool g_decl_bad_reported = false;

// Bound stream sources, as the guest last set them. Stride matters: it is not
// in the fetch instruction at all, it lives in the fetch constant written by
// SetStreamSource, so a backend reads it from here and not from the microcode.
struct StreamSource {
  uint32_t buffer = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
};
constexpr uint32_t kMaxStreams = 16;
StreamSource g_streams[kMaxStreams];
uint32_t g_index_buffer = 0;
uint32_t g_declaration = 0;

uint32_t g_draw_examples_logged = 0;

// Read the declaration the guest currently has bound and fold what it says
// into the counters. Returns false when it does not decode; on success `decl`
// carries it, and its content hash is the variant cache key half that is not
// the shader.
bool RecordDeclaration(uint8_t* base, uint32_t address, VertexDeclaration& decl) {
  if (!DecodeVertexDeclaration(base, address, decl)) {
    ++g_decl_decode_bad;
    if (!g_decl_bad_reported) {
      g_decl_bad_reported = true;
      REXLOG_WARN(
          "native_renderer: vertex declaration at 0x{:08X} does not decode: element count "
          "{} at +{}. The declaration layout read out of 0x82267218 is wrong, and the "
          "vertex input signatures derived from it cannot be trusted.",
          address, REX_LOAD_U32(address + d3d::kDeclElementCount), d3d::kDeclElementCount);
    }
    return false;
  }

  ++g_decl_decode_ok;
  if (decl.element_count > g_decl_max_elements)
    g_decl_max_elements = decl.element_count;
  for (uint32_t i = 0; i < decl.element_count && i < kMaxVertexElements; ++i) {
    if (decl.elements[i].usage < 16)
      ++g_usages_seen[decl.elements[i].usage];
  }

  g_declarations.Insert(decl.identity);
  return true;
}

// What a draw entry point knows about itself, beyond the state the device
// already carries.
struct DrawParams {
  uint32_t prim_type = 0;
  uint32_t count = 0;  // vertices, or indices when `indexed`
  bool indexed = false;
  uint32_t start_index = 0;
  int32_t base_vertex = 0;

  // The inline path (BeginVertices/EndVertices). The vertices live in a block
  // the guest was handed and has just finished filling, not in a bound stream,
  // and the stride is the one BeginVertices was called with -- it writes it into
  // the stream 0 fetch constant slot and restores the old value before
  // returning, so reading the device would give the wrong one.
  bool inlined = false;
  uint32_t inline_address = 0;
  uint32_t inline_stride = 0;
};

// The inline block currently open, between BeginVertices and EndVertices.
// Blocks do not nest: BeginVertices stores its own block at device+13300 and
// the guest closes each one before opening the next.
struct InlineDraw {
  bool open = false;
  uint32_t device = 0;
  DrawParams params;
};
InlineDraw g_inline;

// The 360's physical address aperture. Every place the D3D block turns a
// resource address into something the GPU sees, it strips the cache attribute
// bits and adds one 4 KB page for anything at or above 0xE0000000; see
// SetStreamSource (0x8225B550) and Resolve (0x82260C68), which both spell this
// out inline.
uint32_t PhysicalAddress(uint32_t raw) {
  return (raw & 0x1FFFFFFFu) + (((raw >> 20) + 512) & 0x1000u);
}

// The address to actually *read* a resource through, which is not the one
// above. PhysicalAddress is a key: its extra page exists only so that an
// E-aperture resource compares equal to its resolve destination, and reading
// through it is a page of skew into whatever follows -- or, when the resource
// sits near the top of its allocation, an access violation.
//
// A guest address that already carries aperture bits is readable as it stands.
// A bare physical one is not: only 0xA0000000, 0xC0000000 and 0xE0000000 back
// those pages, so it is aliased into one. See GuestPhysicalPointer in the
// texture mirror, which is the same choice for the same reason.
uint32_t ReadableAddress(uint32_t raw) {
  return raw >= 0x80000000u ? raw : (0xC0000000u | (raw & 0x1FFFFFFFu));
}

// Common tail of every draw: sample the pipeline the draw is about to run with,
// then issue it.
void RecordDraw(uint8_t* base, uint32_t device, const DrawParams& params) {
  const uint32_t prim_type = params.prim_type;
  const uint32_t vertex_count = params.count;
  if (prim_type < 16)
    ++g_prim_types[prim_type];
  g_draw_vertices_total += vertex_count;

  const int vs = ResolveShaderSlot(base, d3d::kVertexShaderTable, g_vertex_shader);
  const int ps = ResolveShaderSlot(base, d3d::kPixelShaderTable, g_pixel_shader);

  // RenderDoc identified this fullscreen path by its static shader slot. Its
  // list-ordered vertices can only come from RECTANGLE_LIST expansion, whereas
  // the captured strip topology says the guest submitted TRIANGLE_STRIP. Log
  // the raw primitive at the hook boundary to distinguish those cases.
  if (vs == 5229) {
    REXLOG_INFO(
        "native_renderer: VS 5229 draw: prim_type={} count={} indexed={} inlined={} PS={}",
        prim_type, vertex_count, params.indexed, params.inlined, ps);
  }

  // Read the declaration from the device rather than from the SetVertexDeclaration
  // hook's shadow: the flush at 0x82267E48 reads device+11684, so that is the one
  // the draw actually uses, and comparing the two would only be testing our own
  // bookkeeping.
  const uint32_t decl_address = REX_LOAD_U32(device + d3d::kVertexDeclaration);
  VertexDeclaration decl;
  const bool have_decl = decl_address != 0 && RecordDeclaration(base, decl_address, decl);
  const uint64_t identity = have_decl ? decl.identity : 0;

  if (vs >= 0 && ps >= 0)
    g_program_pairs.Insert((static_cast<uint64_t>(vs) << 32) | static_cast<uint32_t>(ps));
  if (vs >= 0)
    g_shader_variants.Insert((static_cast<uint64_t>(vs) << 32) ^ identity);

  // Ask the pipeline cache for the host pipeline this draw would run with. The
  // draw is still not issued -- the vertex data has not been uploaded and the
  // constants have not been bound -- but building the pipeline here is what
  // turns the offline shader pack from something that loads into something that
  // links, and it is the only way to find out which (shader, declaration) pairs
  // the host refuses before there is a frame to look at.
  PipelineRequest request;
  request.vertex_slot = vs;
  request.pixel_slot = ps;
  request.declaration = have_decl ? &decl : nullptr;
  request.primitive_type = prim_type;
  for (uint32_t i = 0; i < kMaxPipelineStreams && i < kMaxStreams; ++i)
    request.strides[i] = g_streams[i].stride;
  if (params.inlined)
    request.strides[0] = params.inline_stride;
  request.has_color_target = REX_LOAD_U32(device + d3d::kColorSurfaces) != 0;
  request.has_depth_target = REX_LOAD_U32(device + d3d::kDepthSurface) != 0;

  // Depth, cull, blend and colour mask, read out of the register shadows rather
  // than mirrored from the setters, so state a setter nobody has named still
  // arrives. See native_renderer_state.h.
  request.state = ReadGuestRenderState(base, device);

  // Cheap, and it keeps the extent current even if a frame never rebinds a
  // target: the host target is sized from it.
  UpdateTilingExtent(base, device);
  const GuestPipeline* pipeline = AcquireGuestPipeline(request);

  // And issue it. Everything below turns guest addresses into host pointers;
  // the draw layer does the rest, including the byte swapping, because it is
  // the side that knows what the destination looks like.
  if (pipeline != nullptr && have_decl) {
    GuestDrawCall call;
    call.device = REX_RAW_ADDR(device);
    call.memory_base = base;
    call.declaration = &decl;
    call.pipeline = pipeline;
    call.primitive_type = prim_type;
    call.count = vertex_count;
    call.indexed = params.indexed;
    call.base_vertex = params.base_vertex;
    call.state = request.state;

    if (params.inlined) {
      if (params.inline_address != 0 && params.inline_stride != 0) {
        call.streams[0].data = REX_RAW_ADDR(params.inline_address);
        call.streams[0].stride = params.inline_stride;
        call.streams[0].size = vertex_count * params.inline_stride;
      }
    } else {
      for (uint32_t i = 0; i < kMaxPipelineStreams && i < kMaxStreams; ++i) {
        const StreamSource& stream = g_streams[i];
        if (stream.buffer == 0 || stream.stride == 0)
          continue;
        // The resource carries its own vertex fetch constant at +24, and it is
        // a *packed* one, which is not obvious from how SetStreamSource uses it:
        //
        //   +24  type : 2 | address : 30   address counted in dwords
        //   +28  endian : 2 | size : 24 | pad : 6   size counted in dwords
        //
        // 0x8225B550 stores `*(res+24) + offset` and `*(res+28) - offset`
        // straight into the device without unpacking either, which reads as a
        // byte address and a byte size until you notice that a byte offset is a
        // multiple of four and so lands exactly on the dword-counted field in
        // both words. That is the confirmation of the packing.
        //
        // Read as raw dwords instead: the type in the low two bits of +24 is a
        // three byte skew on every stream, and the endian bits in +28 make the
        // size read as hundreds of megabytes, which is what dropped every
        // indexed draw in the frame.
        const uint32_t fetch_address = REX_LOAD_U32(stream.buffer + 24);
        const uint32_t fetch_size = REX_LOAD_U32(stream.buffer + 28);
        const uint32_t raw = (fetch_address & ~3u) + stream.offset;
        const uint32_t size_bytes = ((fetch_size >> 2) & 0xFFFFFFu) * 4;
        const uint32_t size = size_bytes > stream.offset ? size_bytes - stream.offset : 0;
        call.streams[i].data = REX_RAW_ADDR(ReadableAddress(raw));
        call.streams[i].size = size;
        call.streams[i].stride = stream.stride;
      }
    }

    if (params.indexed) {
      // The index buffer object the guest bound, at device+12300. Bit 31 of its
      // first fetch constant dword is the 32 bit index flag, which is exactly
      // how DrawIndexedVertices itself decides between advancing by two and by
      // four; the address is at +24 like every other resource.
      const uint32_t index_buffer = REX_LOAD_U32(device + 12300);
      if (index_buffer != 0) {
        const bool index_32 = (REX_LOAD_U32(index_buffer) & 0x80000000u) != 0;
        const uint32_t element = index_32 ? 4u : 2u;
        // Same packed fetch constant as a vertex stream's, so the type in the
        // low two bits comes off before the start index is added. It matters
        // more here than there: at two bytes an element the added offset is not
        // always a multiple of four, so the guest's own add-into-the-packed-word
        // trick does not apply and the unpack cannot be skipped.
        const uint32_t raw =
            (REX_LOAD_U32(index_buffer + 24) & ~3u) + element * params.start_index;
        call.index_32bit = index_32;
        call.indices = REX_RAW_ADDR(ReadableAddress(raw));
      }
    }

    IssueGuestDraw(call);
  }

  if (g_draw_examples_logged < 8) {
    ++g_draw_examples_logged;
    REXLOG_INFO(
        "native_renderer: draw prim={} verts={} vs={} ps={} decl=0x{:08X} identity={:016X} "
        "stream0={{buf 0x{:08X} off {} stride {}}}",
        prim_type, vertex_count, vs, ps, decl_address, identity, g_streams[0].buffer,
        g_streams[0].offset, g_streams[0].stride);
  }
}

// ---------------------------------------------------------------------------
// Surfaces, clear and resolve.
//
// This is the half of the frame the draw hooks above cannot see. A 360 title
// does not render into a texture: it renders into EDRAM, which is a fixed 10 MB
// scratch the surface objects carve up by tile, and then *resolves* a rectangle
// of it out to main memory where it becomes a sampleable texture. So the chain
// a backend has to reproduce is Clear -> draws -> Resolve, once per render
// target the frame uses, and the swap chain's front buffer is just the last
// resolve destination.
//
// Two things are worth measuring here and both are measured below.
//
// First, the surface layout itself. CreateRenderTarget's arguments are API
// values, but what it stores in the object is already hardware register fields
// (see the header). Decoding the object back and comparing against the
// arguments the caller passed is a real check on those offsets, in the same
// shape as the constant shadow check: the two agree only if the layout is read
// right.
//
// Second, and this is the design-relevant number: how much of the texture
// working set is produced by a resolve rather than loaded from disk. Every such
// texture is a render target the backend has to schedule, order and transition
// correctly, which is exactly the part of Plume its README calls unfinished. A
// count of one means the frame is a straight render-to-screen; a large count
// means real render-to-texture and a dependency graph to go with it.

uint32_t g_surfaces_created = 0;
uint32_t g_textures_created = 0;
uint32_t g_surface_decode_ok = 0;
uint32_t g_surface_decode_bad = 0;
uint32_t g_surface_mismatch = 0;
bool g_surface_check_reported = false;

uint32_t g_color_surface[d3d::kColorSurfaceCount] = {};
uint32_t g_depth_surface = 0;

uint64_t g_clears = 0;
uint32_t g_clear_flags_seen = 0;
uint64_t g_resolves = 0;
uint32_t g_resolve_flags_seen = 0;
uint32_t g_resolve_sources[8] = {};  // by the low 3 bits of the flags
uint32_t g_surface_examples_logged = 0;
uint32_t g_resolve_examples_logged = 0;

// Check a freshly created surface by reading it back through the layout in the
// header and comparing against what the caller asked for. Reported once: the
// point is whether the offsets are right, and if they are wrong they are wrong
// every time.
void CheckSurface(uint8_t* base, uint32_t address, uint32_t want_width, uint32_t want_height,
                  uint32_t want_format) {
  Surface surface;
  if (!DecodeSurface(base, address, surface)) {
    ++g_surface_decode_bad;
    return;
  }
  ++g_surface_decode_ok;

  // The format the object keeps is the argument verbatim, so it must match
  // exactly; the size fields are re-derived from register bits and so are the
  // part actually under test.
  // An EDRAM tile is 80x16 samples at 4 bytes each, so a surface's footprint is
  // fully determined by its sample count. Checking the stored footprint against
  // that ties three separately decoded fields together, and it is what caught
  // this field being a size rather than an offset.
  //
  // Samples, not pixels: 0x8225DB30 doubles the height at 2x multisampling and
  // the width as well at 4x, before it sizes anything. Leaving that out is what
  // made the two 1280x384 multisampled surfaces read as half their real
  // footprint while every single sample surface matched.
  const uint32_t sample_width = surface.msaa == 2 ? surface.width * 2 : surface.width;
  const uint32_t sample_height = surface.msaa >= 1 ? surface.height * 2 : surface.height;
  const uint32_t expected_tiles = ((sample_width + 79) / 80) * ((sample_height + 15) / 16);

  const bool match = surface.width == want_width && surface.height == want_height &&
                     surface.format == want_format && surface.edram_tiles == expected_tiles;
  // Report the mismatches individually as well as counting them. A handful out
  // of thousands is not noise to be averaged away: either the layout has a case
  // it does not cover, or the guest really does create a surface whose stored
  // fields disagree with its arguments, and the two are worth telling apart.
  if (!match) {
    ++g_surface_mismatch;
    if (g_surface_mismatch <= 4) {
      REXLOG_WARN(
          "native_renderer: surface 0x{:08X} does not read back as created: {}x{} fmt 0x{:X} "
          "against {}x{} fmt 0x{:X}, pitch {} msaa {}, EDRAM {} tiles against {} expected",
          address, surface.width, surface.height, surface.format, want_width, want_height,
          want_format, surface.pitch, surface.msaa, surface.edram_tiles, expected_tiles);
    }
  }

  if (!g_surface_check_reported) {
    g_surface_check_reported = true;
    REXLOG_INFO(
        "native_renderer: surface layout check: object 0x{:08X} decodes as {}x{} fmt 0x{:X} "
        "against created {}x{} fmt 0x{:X} -> {}",
        address, surface.width, surface.height, surface.format, want_width, want_height,
        want_format, match ? "MATCH" : "MISMATCH");
  }

  if (g_surface_examples_logged < 8) {
    ++g_surface_examples_logged;
    REXLOG_INFO(
        "native_renderer: surface 0x{:08X} {}x{} pitch {} msaa {} fmt 0x{:X} hw fmt {} | EDRAM "
        "{} tiles ({} bytes) at base tile {}",
        address, surface.width, surface.height, surface.pitch, surface.msaa, surface.format,
        surface.hw_format, surface.edram_tiles, surface.edram_bytes, surface.base_tile);
  }
}

}  // namespace

bool DecodeSurface(uint8_t* base, uint32_t address, Surface& out) {
  if (address == 0)
    return false;

  const uint32_t size = REX_LOAD_U32(address + d3d::kSurfaceSize);
  const uint32_t pitch_msaa = REX_LOAD_U32(address + d3d::kSurfacePitchMsaa);
  const uint32_t color_info = REX_LOAD_U32(address + d3d::kSurfaceColorInfo);

  out.address = address;
  out.width = (size >> 18) + 1;
  out.height = ((size >> 3) & 0x7FFFu) + 1;
  out.pitch = pitch_msaa & 0x3FFFu;
  out.msaa = (pitch_msaa >> 16) & 3u;
  out.format = REX_LOAD_U32(address + d3d::kSurfaceFormat);
  out.hw_format = (color_info >> 16) & 0xFu;
  out.base_tile = color_info & 0xFFFu;
  out.edram_bytes = REX_LOAD_U32(address + d3d::kSurfaceEdramBytes);
  out.edram_tiles = out.edram_bytes / d3d::kEdramTileBytes;

  // A surface bigger than the 360 can address, or one whose footprint does not
  // fit in EDRAM at all, means the offsets above are not what is being read.
  if (out.width == 0 || out.width > 4096 || out.height == 0 || out.height > 4096)
    return false;
  if (out.edram_tiles == 0 || out.edram_tiles > d3d::kEdramTiles)
    return false;
  return true;
}

bool DecodeResourceTexture(uint8_t* base, uint32_t address, TextureFetch& out) {
  if (address == 0)
    return false;

  uint32_t words[6];
  for (uint32_t i = 0; i < 6; ++i)
    words[i] = REX_LOAD_U32(address + d3d::kResourceFetchConstant + 4 * i);

  out = DecodeTextureFetch(words);
  return out.type == 2 && out.base_address != 0;
}

bool DecodeVertexDeclaration(uint8_t* base, uint32_t address, VertexDeclaration& out) {
  ProfileZone zone(kPhaseDeclDecode);
  if (address == 0)
    return false;

  const uint32_t count = REX_LOAD_U32(address + d3d::kDeclElementCount);
  // A declaration with no elements would leave a draw with nothing to fetch,
  // and a huge one means the offset is not the element count at all. Either
  // way the layout, not the data, is what is in question.
  if (count == 0 || count > kMaxVertexElements)
    return false;

  out.address = address;
  out.serial = REX_LOAD_U32(address + d3d::kDeclSerial);
  out.element_count = count;
  out.truncated = false;

  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t element = address + d3d::kDeclElements + d3d::kDeclElementStride * i;
    VertexElement& e = out.elements[i];
    e.stream = REX_LOAD_U16(element + 0);
    e.offset = REX_LOAD_U16(element + 2);
    e.type = REX_LOAD_U32(element + 4);
    e.usage = REX_LOAD_U8(element + d3d::kDeclUsage);
    e.usage_index = REX_LOAD_U8(element + d3d::kDeclUsageIndex);

    // D3DDECLUSAGE runs 0 (POSITION) to 13 (SAMPLE). Anything above that is
    // not a usage byte, so the +9 offset would be wrong.
    if (e.usage > 13)
      return false;
  }

  // FNV-1a over the decoded elements, which is the declaration's identity for
  // every purpose this renderer has. Built here rather than at the cache so the
  // pipeline key and the counters cannot drift apart about what "the same
  // declaration" means.
  uint64_t hash = 0xCBF29CE484222325ull;
  const auto mix = [&hash](uint32_t value) {
    for (uint32_t byte = 0; byte < 4; ++byte) {
      hash ^= (value >> (8 * byte)) & 0xFFu;
      hash *= 0x100000001B3ull;
    }
  };
  mix(count);
  for (uint32_t i = 0; i < count; ++i) {
    const VertexElement& e = out.elements[i];
    mix(uint32_t(e.stream) | (uint32_t(e.offset) << 16));
    mix(e.type);
    mix(uint32_t(e.usage) | (uint32_t(e.usage_index) << 8));
  }
  out.identity = hash;
  return true;
}

TextureFetch DecodeTextureFetch(const uint32_t words[6]) {
  TextureFetch out;
  out.type = words[0] & 0x3u;
  out.pitch = ((words[0] >> 22) & 0x1FFu) << 5;
  out.tiled = ((words[0] >> 31) & 1u) != 0;

  out.format = words[1] & 0x3Fu;
  out.endianness = (words[1] >> 6) & 0x3u;
  out.stacked = ((words[1] >> 10) & 1u) != 0;

  // The base address field is 20 bits of page number, but turning it into an
  // address is not just a shift. Resolve (0x82260C68) computes its destination
  // as `(((base >> 20) + 512) & 0x1000) + (base & 0x1FFFFFFF)`, and the same
  // expression appears everywhere the D3D block turns a resource address into
  // something the GPU sees. It does two things: it strips the cache attribute
  // bits down to a physical address, and it adds one 4 KB page for anything at
  // or above 0xE0000000, which is the 360's physical aperture quirk.
  //
  // This is not cosmetic. Without it a resolve destination decodes 0x1000 below
  // the address the very texture it produced is sampled from, so the two never
  // match and the render-to-texture overlap below reads as zero. The formula is
  // an identity for addresses outside that aperture, which is why the sampled
  // side looked right on its own.
  const uint32_t raw_base = ((words[1] >> 12) & 0xFFFFFu) << 12;
  out.base_address = (raw_base & 0x1FFFFFFFu) + (((raw_base >> 20) + 512u) & 0x1000u);

  // The fixed-up address above is a *key*: it is what makes a sampled texture
  // and the resolve that produced it compare equal. It is not something to
  // dereference. The bare physical address is not mapped on the host at all --
  // physical memory is reachable only through the 0xA0000000, 0xC0000000 and
  // 0xE0000000 apertures, which was confirmed by probing all four -- and the
  // 0x1000 the fixup adds for the last of those would be a page of skew if it
  // were carried into a different aperture. So the raw form is kept alongside,
  // and the readable pointer is derived from it rather than from the key.
  out.raw_base_address = raw_base;

  // size_2d: 13 bits each, stored with one subtracted.
  out.width = (words[2] & 0x1FFFu) + 1u;
  out.height = ((words[2] >> 13) & 0x1FFFu) + 1u;
  return out;
}

uint32_t D3DDeviceAddress() { return g_device.load(std::memory_order_relaxed); }

// Read sampler `stage`'s fetch constant out of the live device and decode it.
// False when the stage holds no texture, which is both the "never bound" case
// and the zeroed constant SetTexture(stage, nullptr) leaves behind.
//
// The device address is taken from the mirror rather than passed in, because
// the only other pointer to hand at a draw is the *host* pointer to the device
// object and REX_LOAD_U32 wants the guest one. Conflating the two reads garbage
// that decodes as an unbound stage, so the failure is silent.
bool GetBoundTextureFetch(uint8_t* base, uint32_t stage, TextureFetch& out,
                          GuestSamplerState* sampler_out) {
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  if (base == nullptr || stage >= d3d::kSamplerCount || device == 0)
    return false;

  uint32_t words[6];
  for (uint32_t i = 0; i < 6; ++i)
    words[i] = FetchConstantWord(base, device, stage, i);

  if (sampler_out != nullptr)
    *sampler_out = DecodeSamplerState(words);
  out = DecodeTextureFetch(words);
  return out.type == 2 && out.base_address != 0 && out.width != 0 && out.height != 0;
}

uint64_t TextureBindingGeneration() {
  return g_texture_binding_generation.load(std::memory_order_relaxed);
}

void D3DTilingExtent(uint32_t* width, uint32_t* height) {
  if (width != nullptr)
    *width = g_tiling_width.load(std::memory_order_relaxed);
  if (height != nullptr)
    *height = g_tiling_height.load(std::memory_order_relaxed);
}

void LogD3DMirrorSummary() {
  uint32_t vs_distinct = 0;
  uint32_t ps_distinct = 0;
  for (uint32_t i = 0; i < d3d::kShaderTableEntries; ++i) {
    vs_distinct += g_vs_slots_seen[i] ? 1 : 0;
    ps_distinct += g_ps_slots_seen[i] ? 1 : 0;
  }

  REXLOG_INFO(
      "native_renderer: device=0x{:08X} | calls: vs_const={} ps_const={} settexture={} "
      "samplerstate={} shaderbind={} | distinct shader slots bound: vs={} ps={} (off-table "
      "binds: vs={} ps={}) | distinct textures {}{} tiled/linear {}/{}",
      D3DDeviceAddress(), g_vs_constant_calls, g_ps_constant_calls, g_texture_calls,
      g_sampler_sets, g_shader_binds, vs_distinct, ps_distinct, g_vs_unresolved,
      g_ps_unresolved, g_texture_count, g_texture_overflow ? "+ (capped)" : "",
      g_tiled_count, g_linear_count);

  // A slot written but never with a non-zero trip count is the interesting
  // case: the guest is maintaining the register and keeping the loop switched
  // off, rather than never getting to it.
  REXLOG_INFO("native_renderer: loop constants: {} call(s), written=0x{:08X} ever-nonzero=0x{:08X}",
              g_loop_constant_calls, g_loop_written, g_loop_nonzero);
  for (uint32_t slot = 0; slot < kLoopConstants; ++slot) {
    if ((g_loop_written & (1u << slot)) == 0)
      continue;
    REXLOG_INFO("native_renderer:   i{} ({}): last trip count={} max={}", slot,
                slot < 16 ? "vertex" : "pixel", g_loop_last_count[slot], g_loop_max_count[slot]);
  }
}

void LogDrawMirrorSummary() {
  REXLOG_INFO(
      "native_renderer: draws: indexed={} vertices={} inline={} total_verts={} | prim types "
      "1..8: {}/{}/{}/{}/{}/{}/{}/{}",
      g_draws_indexed, g_draws_vertices, g_draws_inline, g_draw_vertices_total,
      g_prim_types[1], g_prim_types[2], g_prim_types[3], g_prim_types[4], g_prim_types[5],
      g_prim_types[6], g_prim_types[7], g_prim_types[8]);

  // The two numbers the offline pipeline set is sized by. If they are close,
  // leaving vertex fetch inside the shader costs little; if the variant count
  // is far larger, lifting fetch into a host input layout is what keeps the
  // static set finite.
  REXLOG_INFO(
      "native_renderer: pipelines: (vs,ps) programs={}{} (vs,decl) variants={}{} distinct "
      "declarations={}{}",
      g_program_pairs.count(), g_program_pairs.saturated() ? "+ (capped)" : "",
      g_shader_variants.count(), g_shader_variants.saturated() ? "+ (capped)" : "",
      g_declarations.count(), g_declarations.saturated() ? "+ (capped)" : "");

  REXLOG_INFO(
      "native_renderer: declarations decoded ok={} bad={} max elements={} | usage counts "
      "POSITION={} BLENDWEIGHT={} BLENDINDICES={} NORMAL={} TEXCOORD={} TANGENT={} COLOR={}",
      g_decl_decode_ok, g_decl_decode_bad, g_decl_max_elements, g_usages_seen[0],
      g_usages_seen[1], g_usages_seen[2], g_usages_seen[3], g_usages_seen[5],
      g_usages_seen[6], g_usages_seen[10]);
}

void LogSurfaceMirrorSummary() {
  REXLOG_INFO(
      "native_renderer: surfaces created={} textures created={} | decode ok={} bad={} "
      "mismatched={} | bound rt0=0x{:08X} rt1=0x{:08X} rt2=0x{:08X} rt3=0x{:08X} "
      "depth=0x{:08X}",
      g_surfaces_created, g_textures_created, g_surface_decode_ok, g_surface_decode_bad,
      g_surface_mismatch, g_color_surface[0], g_color_surface[1], g_color_surface[2],
      g_color_surface[3], g_depth_surface);

  // The resolve traffic, and the overlap with the sampled texture set. The last
  // pair is the render-to-texture load a backend has to carry: distinct resolve
  // destinations that the game then samples, and how often it samples them.
  REXLOG_INFO(
      "native_renderer: clears={} (flag bits seen 0x{:X}) resolves={} (flag bits seen 0x{:X}) "
      "sources 0..4: {}/{}/{}/{}/{} | resolve destinations {}{}, of which sampled {} over {} "
      "binds",
      g_clears, g_clear_flags_seen, g_resolves, g_resolve_flags_seen, g_resolve_sources[0],
      g_resolve_sources[1], g_resolve_sources[2], g_resolve_sources[3], g_resolve_sources[4],
      g_resolve_target_count, g_resolve_target_overflow ? "+ (capped)" : "",
      g_sampled_resolve_count, g_resolved_texture_binds);
}

}  // namespace eternalsonata

// The draw entry points. Each records the pipeline the draw runs with; see the
// block comment above RecordDraw for what the counters are for. All four are
// observe-only: the guest still writes its packets into the unread ring.
//
// (dev, primType, baseVertexIndex, startIndex, indexCount)
REX_HOOK_RAW(D3DDevice__DrawIndexedVertices) {
  const uint32_t device = ctx.r3.u32;
  eternalsonata::DrawParams params;
  params.prim_type = ctx.r4.u32;
  params.base_vertex = int32_t(ctx.r5.u32);
  params.start_index = ctx.r6.u32;
  params.count = ctx.r7.u32;
  params.indexed = true;
  __imp__D3DDevice__DrawIndexedVertices(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  ++eternalsonata::g_draws_indexed;
  eternalsonata::RecordDraw(base, device, params);
}

// (dev, primType, startVertex, vertexCount)
REX_HOOK_RAW(D3DDevice__DrawVertices) {
  const uint32_t device = ctx.r3.u32;
  eternalsonata::DrawParams params;
  params.prim_type = ctx.r4.u32;
  params.count = ctx.r6.u32;
  __imp__D3DDevice__DrawVertices(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  ++eternalsonata::g_draws_vertices;
  eternalsonata::RecordDraw(base, device, params);
}

// (dev, primType, vertexCount, stride) -> the write pointer the caller fills in.
//
// This is the dominant draw path in this title, and the one place where the
// draw cannot be issued where it is announced: the vertices do not exist yet.
// What is recorded here is everything the draw needs, including the returned
// pointer; EndVertices is where the data is complete and the draw goes out.
REX_HOOK_RAW(D3DDevice__BeginVertices) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t prim_type = ctx.r4.u32;
  const uint32_t vertex_count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;
  __imp__D3DDevice__BeginVertices(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  ++eternalsonata::g_draws_inline;

  // A zero return is the guest failing to reserve the block, in which case it
  // draws nothing either.
  eternalsonata::g_inline = {};
  eternalsonata::g_inline.device = device;
  eternalsonata::g_inline.params.prim_type = prim_type;
  eternalsonata::g_inline.params.count = vertex_count;
  eternalsonata::g_inline.params.inlined = true;
  eternalsonata::g_inline.params.inline_address = ctx.r3.u32;
  eternalsonata::g_inline.params.inline_stride = stride;
  eternalsonata::g_inline.open = ctx.r3.u32 != 0;
}

REX_HOOK_RAW(D3DDevice__EndVertices) {
  __imp__D3DDevice__EndVertices(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  if (!eternalsonata::g_inline.open)
    return;

  eternalsonata::g_inline.open = false;
  eternalsonata::RecordDraw(base, eternalsonata::g_inline.device, eternalsonata::g_inline.params);
}

// (dev, index, vertexBuffer, offset, stride, ?). Stride is recorded because it
// is not in the vfetch instruction: the patcher takes format, swizzle and
// offset from the declaration, but stride comes from the fetch constant this
// writes.
REX_HOOK_RAW(D3DDevice__SetStreamSource) {
  const uint32_t index = ctx.r4.u32;
  const uint32_t buffer = ctx.r5.u32;
  const uint32_t offset = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;
  __imp__D3DDevice__SetStreamSource(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  if (index < eternalsonata::kMaxStreams)
    eternalsonata::g_streams[index] = {buffer, offset, stride};
}

REX_HOOK_RAW(D3DDevice__SetIndices) {
  const uint32_t buffer = ctx.r4.u32;
  __imp__D3DDevice__SetIndices(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  eternalsonata::g_index_buffer = buffer;
}

REX_HOOK_RAW(D3DDevice__SetVertexDeclaration) {
  const uint32_t declaration = ctx.r4.u32;
  __imp__D3DDevice__SetVertexDeclaration(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  eternalsonata::g_declaration = declaration;
}

// D3D__CreateDevice(..., IDirect3DDevice9** out). The device pointer lands in
// the caller's out parameter (r8), so it can only be read after the original
// runs.
REX_HOOK_RAW(D3D__CreateDevice) {
  const uint32_t out = static_cast<uint32_t>(ctx.r8.u32);
  __imp__D3D__CreateDevice(ctx, base);

  if (!eternalsonata::NativeRendererEnabled() || ctx.r3.u32 != 0 || out == 0)
    return;

  const uint32_t device = REX_LOAD_U32(out);
  eternalsonata::g_device.store(device, std::memory_order_relaxed);
  REXLOG_INFO(
      "native_renderer: D3D device created at 0x{:08X} ({} bytes). Constant shadows at "
      "0x{:08X}/0x{:08X}, fetch constants at 0x{:08X}.",
      device, eternalsonata::d3d::kDeviceSize,
      device + eternalsonata::d3d::kVertexConstantShadow,
      device + eternalsonata::d3d::kPixelConstantShadow,
      device + eternalsonata::d3d::kTextureFetchConstants);
}

// (device, start, source, count), count in vec4s. The guest's own constant
// cache (0x8212CDE0) coalesces these 16 at a time, so this is already
// deduplicated traffic by the time it arrives.
REX_HOOK_RAW(D3DDevice__SetVertexShaderConstantF) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  const uint32_t source = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;

  __imp__D3DDevice__SetVertexShaderConstantF(ctx, base);

  if (!eternalsonata::NativeRendererEnabled())
    return;

  using namespace eternalsonata;
  ++g_vs_constant_calls;
  CheckShadow(base, "vertex", device, d3d::kVertexConstantShadow, start, source, count,
              g_vs_shadow_checked);
  MirrorConstants(base, device + d3d::kVertexConstantShadow, start, count, g_vs_constants,
                  d3d::kConstantRegisters);
}

REX_HOOK_RAW(D3DDevice__SetPixelShaderConstantF) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  const uint32_t source = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;

  __imp__D3DDevice__SetPixelShaderConstantF(ctx, base);

  if (!eternalsonata::NativeRendererEnabled())
    return;

  using namespace eternalsonata;
  ++g_ps_constant_calls;
  CheckShadow(base, "pixel", device, d3d::kPixelConstantShadow, start, source, count,
              g_ps_shadow_checked);
  MirrorConstants(base, device + d3d::kPixelConstantShadow, start, count, g_ps_constants,
                  d3d::kConstantRegisters);
}

// (device, start, source, count) like the float setters, but `start` is stage
// relative: the pixel setter's register 0 is unified loop register i16.
REX_HOOK_RAW(D3DDevice__SetVertexShaderConstantI) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  const uint32_t source = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;

  __imp__D3DDevice__SetVertexShaderConstantI(ctx, base);

  if (!eternalsonata::NativeRendererEnabled())
    return;
  eternalsonata::RecordLoopConstants(base, device, 0, start, source, count);
}

REX_HOOK_RAW(D3DDevice__SetPixelShaderConstantI) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  const uint32_t source = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;

  __imp__D3DDevice__SetPixelShaderConstantI(ctx, base);

  if (!eternalsonata::NativeRendererEnabled())
    return;
  eternalsonata::RecordLoopConstants(base, device, 16, start, source, count);
}

REX_HOOK_RAW(D3DDevice__SetVertexShader) {
  const uint32_t shader = ctx.r4.u32;
  __imp__D3DDevice__SetVertexShader(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  using namespace eternalsonata;
  g_vertex_shader = shader;
  ++g_shader_binds;
  const int slot = ResolveShaderSlot(base, d3d::kVertexShaderTable, shader);
  if (slot >= 0)
    g_vs_slots_seen[slot] = true;
  else if (shader != 0)
    ++g_vs_unresolved;
}

REX_HOOK_RAW(D3DDevice__SetPixelShader) {
  const uint32_t shader = ctx.r4.u32;
  __imp__D3DDevice__SetPixelShader(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  using namespace eternalsonata;
  g_pixel_shader = shader;
  ++g_shader_binds;
  const int slot = ResolveShaderSlot(base, d3d::kPixelShaderTable, shader);
  if (slot >= 0)
    g_ps_slots_seen[slot] = true;
  else if (shader != 0)
    ++g_ps_unresolved;
}

// The general cut: every GPU wait in the D3D block goes through this.
//
// The block's waits all have the same shape -- a ThrottleWait_Begin /
// ThrottleWait_Poll / ThrottleWait_End bracket around a spin whose loop
// condition is `... && Poll(...)`, or which breaks on `!Poll(...)`. Poll
// normally returns 1 to keep spinning and, once a 5000 timebase tick window
// expires, calls OnGpuHang instead. Returning 0 makes every one of those loops
// fall straight out through its normal exit; none of them has an error path on
// that edge, they just proceed to ThrottleWait_End.
//
// This is one place rather than one stub per waiting caller, and it also means
// the "GPU is hung" recovery path can never be reached, which it otherwise
// would be, since with no GPU every wait exceeds the window by definition.
REX_HOOK_RAW(D3DDevice__ThrottleWait_Poll) {
  if (!eternalsonata::NativeRendererEnabled()) {
    __imp__D3DDevice__ThrottleWait_Poll(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// The fence wait underneath everything else that blocks.
//
// It spins until the retired counter the GPU writes into the identifier block
// at device+10768 passes the requested fence. Nothing writes that counter with
// no GPU plugin, so every caller blocks forever: this is where the game parked
// once the ring buffer init was got past, reached from Swap's frame throttle
// (the guest lets itself run at most 15 frames ahead of the GPU).
//
// With no GPU, work submitted is work already finished, so every fence is
// retired the moment it is asked about. Returns its first argument, which is
// the device, so leaving r3 alone is the correct return value.
REX_HOOK_RAW(D3DDevice__BlockUntilFenceRetired) {
  if (!eternalsonata::NativeRendererEnabled()) {
    __imp__D3DDevice__BlockUntilFenceRetired(ctx, base);
    return;
  }
}

// The first ring buffer call that has to go.
//
// This emits a wait-for-idle packet, submits it, and then busy waits on
// device+10872 until the GPU clears it. With no GPU plugin nothing ever will,
// so the guest spins forever: this is where the game parked on the first
// headless run, reached from the renderer init (0x8210A148) through
// SetRingBufferParameters, which drains the previous ring before replacing it.
//
// A GPU that does not exist is always idle, so the honest answer here is to
// return success immediately. Note this deliberately does not stub
// SetRingBufferParameters itself: letting it run keeps the ring buffer memory
// allocated and the command buffer pointers at device+48/52/56 valid, so the
// guest's packet writers keep writing somewhere harmless instead of through a
// null write pointer.
REX_HOOK_RAW(D3DDevice__BlockUntilGpuIdle) {
  if (!eternalsonata::NativeRendererEnabled()) {
    __imp__D3DDevice__BlockUntilGpuIdle(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// The surface path. See the block comment above the surface counters for what
// these are for; as with everything else here they only observe.
//
// (width, height, format, multisample, params) -> surface object. Note this is
// a static, not a method: the device is not an argument.
REX_HOOK_RAW(D3DDevice__CreateRenderTarget) {
  const uint32_t width = ctx.r3.u32;
  const uint32_t height = ctx.r4.u32;
  const uint32_t format = ctx.r5.u32;
  __imp__D3DDevice__CreateRenderTarget(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  const uint32_t surface = ctx.r3.u32;
  if (surface == 0)
    return;
  ++eternalsonata::g_surfaces_created;
  eternalsonata::CheckSurface(base, surface, width, height, format);
}

// (width, height, levels, usage, ?, format, ?, pool) -> texture object.
REX_HOOK_RAW(D3DDevice__CreateTexture) {
  __imp__D3DDevice__CreateTexture(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  ++eternalsonata::g_textures_created;
}

// (device, index, surface). The store is to device+12304+4*index, so the hook
// records the index it was given and the summary reads the device to confirm.
REX_HOOK_RAW(D3DDevice__SetRenderTarget) {
  // Every argument is read *before* the call, because r3..r12 are volatile: the
  // callee has overwritten r3 with its return value by the time it comes back,
  // and the device pointer is gone. This is exactly what the index and surface
  // above have always done, and reading the device after the call instead is
  // what made the game crash a few seconds in.
  const uint32_t device = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t surface = ctx.r5.u32;
  __imp__D3DDevice__SetRenderTarget(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  if (index < eternalsonata::d3d::kColorSurfaceCount)
    eternalsonata::g_color_surface[index] = surface;

  using namespace eternalsonata;

  // Before the target is acquired, because the host target is sized from it.
  UpdateTilingExtent(base, device);

  Surface decoded;
  if (surface != 0 && DecodeSurface(base, surface, decoded))
    FrameSetColorSurface(index, &decoded);
  else
    FrameSetColorSurface(index, nullptr);
}

REX_HOOK_RAW(D3DDevice__SetDepthStencilSurface) {
  const uint32_t device = ctx.r3.u32;  // volatile across the call; see above
  const uint32_t surface = ctx.r4.u32;
  __imp__D3DDevice__SetDepthStencilSurface(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  eternalsonata::g_depth_surface = surface;

  using namespace eternalsonata;
  UpdateTilingExtent(base, device);

  Surface decoded;
  if (surface != 0 && DecodeSurface(base, surface, decoded))
    FrameSetDepthSurface(&decoded);
  else
    FrameSetDepthSurface(nullptr);
}

// (device, D3DVIEWPORT9*). 0x8225BC88 reads it as four u32s followed by two
// floats before converting them, which is what makes the layout below a reading
// of the code rather than an assumption about the struct.
REX_HOOK_RAW(D3DDevice__SetViewport) {
  const uint32_t viewport = ctx.r4.u32;
  __imp__D3DDevice__SetViewport(ctx, base);
  if (!eternalsonata::NativeRendererEnabled() || viewport == 0)
    return;

  const uint32_t min_raw = REX_LOAD_U32(viewport + 16);
  const uint32_t max_raw = REX_LOAD_U32(viewport + 20);
  float min_z, max_z;
  std::memcpy(&min_z, &min_raw, sizeof(min_z));
  std::memcpy(&max_z, &max_raw, sizeof(max_z));
  eternalsonata::DrawSetViewport(REX_LOAD_U32(viewport + 0), REX_LOAD_U32(viewport + 4),
                                 REX_LOAD_U32(viewport + 8), REX_LOAD_U32(viewport + 12), min_z,
                                 max_z);
}

// (device, rectCount, rects, flags, color, z in f1, stencil). Integer arguments
// keep their GPR slots whether or not a float argument sits between them, so Z
// does not displace anything: flags is r6 and the packed colour is r7. Z itself
// arrives in f1 and still consumes r8, which is why the stencil is r9.
//
// This is the first hook that does more than watch: the clear is applied to the
// host render target standing in for the bound EDRAM surface, which is what
// puts the guest's own colours on screen.
REX_HOOK_RAW(D3DDevice__Clear) {
  const uint32_t flags = ctx.r6.u32;
  const uint32_t color = ctx.r7.u32;
  const float z = float(ctx.f1.f64);
  const uint32_t stencil = ctx.r9.u32;
  __imp__D3DDevice__Clear(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  ++eternalsonata::g_clears;
  eternalsonata::g_clear_flags_seen |= flags;
  eternalsonata::FrameClear(flags, color, z, stencil);
}

// (device, flags, srcRect, destTexture, destPoint, level, slice, clearColor,
// clearZ in f1). The low 3 bits of the flags pick the source: 0..3 are the
// colour targets and 4 is the depth stencil, which is how 0x82260C68 itself
// indexes device+12304 / device+12320.
//
// The destination is a resource object, so its six fetch constant dwords sit at
// +28 and give the base address the resolved pixels land at. That address is
// the join between this and the texture mirror: a texture the game samples from
// the same address is a render target, not an asset.
REX_HOOK_RAW(D3DDevice__Resolve) {
  const uint32_t flags = ctx.r4.u32;
  const uint32_t rect = ctx.r5.u32;
  const uint32_t destination = ctx.r6.u32;
  const uint32_t point = ctx.r7.u32;
  __imp__D3DDevice__Resolve(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  using namespace eternalsonata;
  ++g_resolves;
  g_resolve_flags_seen |= flags;
  const uint32_t source = flags & 7u;
  if (source < 8)
    ++g_resolve_sources[source];

  TextureFetch fetch;
  if (!DecodeResourceTexture(base, destination, fetch))
    return;

  // The rectangle and the destination point. A null rect means the whole
  // surface, which is what D3D9 means by it everywhere else; the frame layer
  // clamps to the source either way, so the sentinel below is a bound and not a
  // guess. D3DRECT is four int32s and D3DPOINT two, both big endian.
  int32_t x1 = 0, y1 = 0, x2 = 0x7FFFFFFF, y2 = 0x7FFFFFFF;
  if (rect != 0) {
    x1 = int32_t(REX_LOAD_U32(rect + 0));
    y1 = int32_t(REX_LOAD_U32(rect + 4));
    x2 = int32_t(REX_LOAD_U32(rect + 8));
    y2 = int32_t(REX_LOAD_U32(rect + 12));
  }
  int32_t dest_x = 0, dest_y = 0;
  if (point != 0) {
    dest_x = int32_t(REX_LOAD_U32(point + 0));
    dest_y = int32_t(REX_LOAD_U32(point + 4));
  }

  FrameResolve(source, base, fetch, x1, y1, x2, y2, dest_x, dest_y);

  // A resolve can replace the host image behind an address the guest never
  // rebinds, so a cached binding over it has to be invalidated here too.
  BumpTextureBindingGeneration();

  const bool first = TrackResolveDestination(fetch.base_address);
  if (first && g_resolve_examples_logged < 8) {
    ++g_resolve_examples_logged;
    REXLOG_INFO(
        "native_renderer: resolve flags 0x{:X} source {} -> texture 0x{:08X} {}x{} fmt {} {} "
        "at 0x{:08X}",
        flags, source, destination, fetch.width, fetch.height, fetch.format,
        fetch.tiled ? "tiled" : "linear", fetch.base_address);
  }
}

// The guest's frame boundary, and the host's. Presenting from here rather than
// from a host timer keeps the two frame rates locked together, which is what we
// want while the host frame is derived entirely from guest state: there is
// nothing to show that the guest has not finished producing.
REX_HOOK_RAW(D3DDevice__Swap) {
  __imp__D3DDevice__Swap(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  eternalsonata::PlumePresentFrame();
  eternalsonata::TextureMirrorBeginFrame();
  // The frame boundary invalidates every cached binding, so the once-per-frame
  // content hash still happens. See TextureBindingGeneration.
  eternalsonata::BumpTextureBindingGeneration();
  // Rolls the shader debugger's "active this frame" flags over, so its list
  // shows what the game is drawing now rather than what it ever drew.
  eternalsonata::GuestShaderDebugEndFrame();

  static uint64_t swaps = 0;
  if (++swaps % 300 == 0) {
    REXLOG_INFO("native_renderer: guest swap #{}", swaps);
    eternalsonata::LogD3DMirrorSummary();
    eternalsonata::LogDrawMirrorSummary();
    eternalsonata::LogSurfaceMirrorSummary();
    eternalsonata::LogFrameSummary();
    eternalsonata::LogReadbackSummary();
    eternalsonata::LogPipelineSummary();
    eternalsonata::LogGuestDrawSummary();
    eternalsonata::LogTextureMirrorSummary();
    eternalsonata::LogProfileSummary();
  }
}

// (device, sampler, texture, flags)
REX_HOOK_RAW(D3DDevice__SetTexture) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t sampler = ctx.r4.u32;
  const uint32_t texture = ctx.r5.u32;
  __imp__D3DDevice__SetTexture(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;

  using namespace eternalsonata;
  if (sampler < d3d::kSamplerCount)
    g_textures[sampler] = texture;
  ++g_texture_calls;
  CheckSamplerClobber(base, device, sampler);
  RecordTextureFetch(base, device, sampler);
  BumpTextureBindingGeneration();
}

// The three sampler setters, hooked only to snapshot what they wrote so the
// next SetTexture on the same stage can be checked against it. (device, stage,
// value) in all three.
REX_HOOK_RAW(D3DDevice__SetSamplerState_MinFilter) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t stage = ctx.r4.u32;
  __imp__D3DDevice__SetSamplerState_MinFilter(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  ++eternalsonata::g_sampler_sets;
  eternalsonata::SnapshotSampler(base, device, stage);
  eternalsonata::BumpTextureBindingGeneration();
}

REX_HOOK_RAW(D3DDevice__SetSamplerState_MagFilter) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t stage = ctx.r4.u32;
  __imp__D3DDevice__SetSamplerState_MagFilter(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  ++eternalsonata::g_sampler_sets;
  eternalsonata::SnapshotSampler(base, device, stage);
  eternalsonata::BumpTextureBindingGeneration();
}

REX_HOOK_RAW(D3DDevice__SetSamplerState_MipMapLodBias) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t stage = ctx.r4.u32;
  __imp__D3DDevice__SetSamplerState_MipMapLodBias(ctx, base);
  if (!eternalsonata::NativeRendererEnabled())
    return;
  ++eternalsonata::g_sampler_sets;
  eternalsonata::SnapshotSampler(base, device, stage);
  eternalsonata::BumpTextureBindingGeneration();
}
