// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_pipeline.h.

#include "native_renderer_pipeline_internal.h"

#include <iterator>
#include <memory>
#include <vector>

#include <rex/logging.h>

#include "guest_shaders.h"
#include "native_renderer_plume_internal.h"

namespace eternalsonata {
namespace {

using namespace plume;

// D3DDECLUSAGE -> HLSL semantic name. This has to agree with USAGE_SEMANTICS in
// scripts/xenos_hlsl.py, which is what the compiled blobs declare their inputs
// with; a disagreement shows up as a pipeline that will not link rather than as
// wrong pixels.
constexpr const char* kUsageSemantics[] = {
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",  "TEXCOORD", "TANGENT",
    "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",    "DEPTH",    "SAMPLE",
};
constexpr uint32_t kUsageCount = uint32_t(std::size(kUsageSemantics));

// xenos::VertexFormat, the low 6 bits of a declaration element's `type`.
enum : uint32_t {
  kVf_8_8_8_8 = 6,
  kVf_2_10_10_10 = 7,
  kVf_10_11_11 = 16,
  kVf_11_11_10 = 17,
  kVf_16_16 = 25,
  kVf_16_16_16_16 = 26,
  kVf_16_16_FLOAT = 31,
  kVf_16_16_16_16_FLOAT = 32,
  kVf_32 = 33,
  kVf_32_32 = 34,
  kVf_32_32_32_32 = 35,
  kVf_32_FLOAT = 36,
  kVf_32_32_FLOAT = 37,
  kVf_32_32_32_32_FLOAT = 38,
  kVf_32_32_32_FLOAT = 57,
};

// Every reason a request can be refused, each reported once rather than per
// draw. A draw that cannot get a pipeline is geometry that will not appear, so
// these counters are the first thing to read when the frame is missing
// something.
enum RefusalReason {
  kRefuseUnresolvedSlot,
  kRefuseNoBlob,
  kRefuseNoDeclaration,
  kRefuseVertexFormat,
  kRefuseInterpolatorMismatch,
  kRefuseTopology,
  kRefuseTooManyElements,
  kRefuseDeviceDown,
  kRefuseCreateFailed,
  kRefuseCount,
};
constexpr const char* kRefusalNames[kRefuseCount] = {
    "unresolved shader slot",
    "shader not in the pack",
    "no vertex declaration",
    "vertex format with no host equivalent",
    "pixel inputs are not a subset of vertex outputs",
    "primitive type with no host topology",
    "declaration has more elements than the layout holds",
    "device not up",
    "pipeline creation failed",
};
uint64_t g_refusals[kRefuseCount] = {};
bool g_refusal_reported[kRefuseCount] = {};

void Refuse(RefusalReason reason, const char* detail) {
  ++g_refusals[reason];
  if (g_refusal_reported[reason])
    return;
  g_refusal_reported[reason] = true;
  REXLOG_WARN("native_renderer: no pipeline, {}: {}", kRefusalNames[reason], detail);
}

// The vertex formats seen in a declaration, so the next run says which of the
// gaps below this title actually walks into rather than leaving all of them
// hypothetical.
uint64_t g_format_seen[64] = {};
uint64_t g_integer_inputs = 0;
bool g_integer_reported = false;
bool g_packed_reported = false;

// A declaration element's `type` is already the hardware fetch encoding, not a
// D3DDECLTYPE: data format in bits 0..5, `format_comp_all` (signed) at bit 8 and
// `num_format_all` (not normalised, i.e. integer) at bit 9. Those are the same
// two bits the patcher shifts into vfetch word1 bits 12..13, and the same fields
// ucode.h names on VertexFetchInstruction.
RenderFormat MapVertexFormat(uint32_t type) {
  const uint32_t format = type & 0x3Fu;
  const bool is_signed = ((type >> 8) & 1u) != 0;
  const bool is_integer = ((type >> 9) & 1u) != 0;

  if (format < 64)
    ++g_format_seen[format];

  // The emitted HLSL declares every vertex input as float4, because the shader
  // is compiled without knowing which declaration it will be paired with. A
  // *_UINT or *_SINT input layout format feeding a float register is a type
  // mismatch the D3D12 debug layer objects to, so it is counted and reported
  // here: the fix is an emitter change (declare those inputs uint4/int4 and
  // convert), and the counter says which shaders would need it. The format is
  // still emitted rather than the draw refused, so the geometry appears and the
  // problem is visible in the picture instead of absent from it.
  // `num_format_all` says nothing for a format that is already floating point:
  // a float is never "normalised", so every float declaration element has the
  // bit set. Only the fixed point formats can actually arrive as raw integers,
  // which is the case that mismatches the shader's float4.
  const bool is_float = format == kVf_16_16_FLOAT || format == kVf_16_16_16_16_FLOAT ||
                        format == kVf_32_FLOAT || format == kVf_32_32_FLOAT ||
                        format == kVf_32_32_32_FLOAT || format == kVf_32_32_32_32_FLOAT;

  if (is_integer && !is_float) {
    ++g_integer_inputs;
    if (!g_integer_reported) {
      g_integer_reported = true;
      REXLOG_WARN(
          "native_renderer: vertex format {} is unnormalised integer, and the emitted HLSL reads "
          "every input as float4. The input layout is built anyway; those components arrive as "
          "raw bits rather than as the value the guest fetched.",
          format);
    }
  }

  switch (format) {
    case kVf_8_8_8_8:
      if (is_integer)
        return is_signed ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT;
      return is_signed ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case kVf_16_16:
      if (is_integer)
        return is_signed ? RenderFormat::R16G16_SINT : RenderFormat::R16G16_UINT;
      return is_signed ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM;
    case kVf_16_16_16_16:
      if (is_integer)
        return is_signed ? RenderFormat::R16G16B16A16_SINT : RenderFormat::R16G16B16A16_UINT;
      return is_signed ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM;
    case kVf_16_16_FLOAT:
      return RenderFormat::R16G16_FLOAT;
    case kVf_16_16_16_16_FLOAT:
      return RenderFormat::R16G16B16A16_FLOAT;
    case kVf_32:
      return is_signed ? RenderFormat::R32_SINT : RenderFormat::R32_UINT;
    case kVf_32_32:
      return is_signed ? RenderFormat::R32G32_SINT : RenderFormat::R32G32_UINT;
    case kVf_32_32_32_32:
      return is_signed ? RenderFormat::R32G32B32A32_SINT : RenderFormat::R32G32B32A32_UINT;
    case kVf_32_FLOAT:
      return RenderFormat::R32_FLOAT;
    case kVf_32_32_FLOAT:
      return RenderFormat::R32G32_FLOAT;
    case kVf_32_32_32_FLOAT:
      return RenderFormat::R32G32B32_FLOAT;
    case kVf_32_32_32_32_FLOAT:
      return RenderFormat::R32G32B32A32_FLOAT;

    // k_2_10_10_10 is most of this title's geometry, so it is served by a real
    // host input format rather than unpacked in the shader: Plume gained
    // R10G10B10A2_UNORM (DXGI R10G10B10A2_UNORM, Vulkan A2B10G10R10_PACK32,
    // Metal RGB10A2Unorm), all of which pack red in the low bits exactly as the
    // Xenos format does. The signed spelling, which this title uses for its
    // normals and tangents, has no host equivalent anywhere in that set and is
    // repacked into R8G8B8A8_SNORM on upload instead -- same four bytes, so the
    // stride and every offset are untouched. See VertexFormatRepacksToSnorm8.
    // The unnormalised spelling has neither, and falls through.
    case kVf_2_10_10_10:
      if (!is_integer)
        return is_signed ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R10G10B10A2_UNORM;
      [[fallthrough]];

    // 10_11_11 and 11_11_10 have no host input-assembler equivalent at all, and
    // have not been seen in this title. If they turn up, the way out is to fetch
    // them as R32_UINT and unpack in the shader, which puts the declaration back
    // into the shader's key -- affordable at 22 variants, but a real design
    // change, so it is measured before it is made.
    case kVf_10_11_11:
    case kVf_11_11_10:
      if (!g_packed_reported) {
        g_packed_reported = true;
        REXLOG_WARN(
            "native_renderer: vertex format {} (signed {}, integer {}) is a packed 10/11 bit "
            "format with no host input layout equivalent. Draws using it get no pipeline until it "
            "is unpacked in the shader.",
            format, is_signed, is_integer);
      }
      return RenderFormat::UNKNOWN;

    default:
      return RenderFormat::UNKNOWN;
  }
}

// Xenos PrimitiveType. Note these are not D3D9 values: the traffic in this
// title is 6 (TRIANGLE_STRIP) and 4 (TRIANGLE_LIST) almost entirely, with a
// little 8 (RECTANGLE_LIST) for UI quads and 1 (POINT_LIST).
RenderPrimitiveTopology MapTopology(uint32_t primitive_type) {
  switch (primitive_type) {
    case 1:
      return RenderPrimitiveTopology::POINT_LIST;
    case 2:
      return RenderPrimitiveTopology::LINE_LIST;
    case 3:
      return RenderPrimitiveTopology::LINE_STRIP;
    case 4:
      return RenderPrimitiveTopology::TRIANGLE_LIST;
    case 5:
      return RenderPrimitiveTopology::TRIANGLE_FAN;
    case 6:
      return RenderPrimitiveTopology::TRIANGLE_STRIP;

    // RECTANGLE_LIST is the 360's three-vertex quad: the fourth corner is
    // implied. There is no host topology for it, and the standard way out is to
    // expand each triple into two triangles when the vertices are uploaded, so
    // the pipeline it needs is an ordinary triangle list. The expansion is the
    // vertex path's job; this only has to agree with it.
    case 8:
      return RenderPrimitiveTopology::TRIANGLE_LIST;

    default:
      return RenderPrimitiveTopology::UNKNOWN;
  }
}

// Xenos CompareFunction is the same 0..7 ordering the host uses, but spelling
// the map out is what makes that a decision rather than a coincidence.
RenderComparisonFunction MapCompare(GuestCompare func) {
  switch (func) {
    case GuestCompare::kNever:
      return RenderComparisonFunction::NEVER;
    case GuestCompare::kLess:
      return RenderComparisonFunction::LESS;
    case GuestCompare::kEqual:
      return RenderComparisonFunction::EQUAL;
    case GuestCompare::kLessEqual:
      return RenderComparisonFunction::LESS_EQUAL;
    case GuestCompare::kGreater:
      return RenderComparisonFunction::GREATER;
    case GuestCompare::kNotEqual:
      return RenderComparisonFunction::NOT_EQUAL;
    case GuestCompare::kGreaterEqual:
      return RenderComparisonFunction::GREATER_EQUAL;
    default:
      return RenderComparisonFunction::ALWAYS;
  }
}

// A blend factor the host has no name for. Counted rather than substituted,
// because a wrong factor is a picture that looks nearly right.
uint64_t g_blend_factor_unknown = 0;
uint64_t g_stencil_draws = 0;
uint64_t g_cull_both_draws = 0;

// xenos::BlendFactor. The gaps (2, 3) are not defined by the hardware.
RenderBlend MapBlendFactor(uint32_t factor, bool alpha_half) {
  switch (factor) {
    case 0:
      return RenderBlend::ZERO;
    case 1:
      return RenderBlend::ONE;
    // The colour factors have no meaning in the alpha half of a host blend
    // desc, which takes only the alpha channel; the console applies the colour
    // factor's alpha component there, so SRC_COLOR in the alpha half is
    // SRC_ALPHA. That is the same substitution xenia's kBlendFactorAlphaMap
    // makes.
    case 4:
      return alpha_half ? RenderBlend::SRC_ALPHA : RenderBlend::SRC_COLOR;
    case 5:
      return alpha_half ? RenderBlend::INV_SRC_ALPHA : RenderBlend::INV_SRC_COLOR;
    case 6:
      return RenderBlend::SRC_ALPHA;
    case 7:
      return RenderBlend::INV_SRC_ALPHA;
    case 8:
      return alpha_half ? RenderBlend::DEST_ALPHA : RenderBlend::DEST_COLOR;
    case 9:
      return alpha_half ? RenderBlend::INV_DEST_ALPHA : RenderBlend::INV_DEST_COLOR;
    case 10:
      return RenderBlend::DEST_ALPHA;
    case 11:
      return RenderBlend::INV_DEST_ALPHA;
    case 12:
    case 14:
      return RenderBlend::BLEND_FACTOR;
    case 13:
    case 15:
      return RenderBlend::INV_BLEND_FACTOR;
    case 16:
      return RenderBlend::SRC_ALPHA_SAT;
    default:
      ++g_blend_factor_unknown;
      return RenderBlend::ONE;
  }
}

RenderBlendOperation MapBlendOp(uint32_t op) {
  switch (op) {
    case 0:
      return RenderBlendOperation::ADD;
    case 1:
      return RenderBlendOperation::SUBTRACT;
    case 2:
      return RenderBlendOperation::MIN;
    case 3:
      return RenderBlendOperation::MAX;
    case 4:
      return RenderBlendOperation::REV_SUBTRACT;
    default:
      return RenderBlendOperation::ADD;
  }
}

RenderBlendDesc MapBlend(const GuestRenderState& state) {
  RenderBlendDesc blend = RenderBlendDesc::Copy();
  blend.renderTargetWriteMask = uint8_t(state.write_mask);
  if (!state.blend_enabled)
    return blend;

  blend.blendEnabled = true;
  blend.blendOp = MapBlendOp((state.blend_control >> 5) & 7u);
  blend.blendOpAlpha = MapBlendOp((state.blend_control >> 21) & 7u);
  blend.srcBlend = MapBlendFactor(state.blend_control & 0x1Fu, false);
  blend.dstBlend = MapBlendFactor((state.blend_control >> 8) & 0x1Fu, false);
  blend.srcBlendAlpha = MapBlendFactor((state.blend_control >> 16) & 0x1Fu, true);
  blend.dstBlendAlpha = MapBlendFactor((state.blend_control >> 24) & 0x1Fu, true);

  // MIN and MAX ignore the factors on the console, and a host that applies them
  // would blend something else entirely. Forcing ONE is what makes the two
  // agree; this is xenia's rule as well.
  if (blend.blendOp == RenderBlendOperation::MIN || blend.blendOp == RenderBlendOperation::MAX) {
    blend.srcBlend = RenderBlend::ONE;
    blend.dstBlend = RenderBlend::ONE;
  }
  if (blend.blendOpAlpha == RenderBlendOperation::MIN ||
      blend.blendOpAlpha == RenderBlendOperation::MAX) {
    blend.srcBlendAlpha = RenderBlend::ONE;
    blend.dstBlendAlpha = RenderBlend::ONE;
  }
  return blend;
}

struct PipelineKey {
  uint8_t vertex_slot = 0;
  uint8_t pixel_slot = 0;
  uint8_t topology = 0;
  uint8_t targets = 0;  // bit 0 colour, bit 1 depth
  uint32_t declaration_serial = 0;
  uint32_t strides[kMaxPipelineStreams] = {};

  // The guest's own register values, not a decode of them. Two states that
  // differ in a bit no host pipeline reads cost one extra pipeline, which is
  // cheap; two states that decode the same but were reached differently cannot
  // be confused, which is not. Alpha test is deliberately absent: it lives in
  // the pixel shader, not in the state object.
  uint32_t depth_control = 0;
  uint32_t blend_control = 0;
  uint32_t mode_cntl = 0;
  uint32_t color_mask = 0;

  bool operator==(const PipelineKey& other) const {
    if (vertex_slot != other.vertex_slot || pixel_slot != other.pixel_slot ||
        topology != other.topology || targets != other.targets ||
        declaration_serial != other.declaration_serial ||
        depth_control != other.depth_control || blend_control != other.blend_control ||
        mode_cntl != other.mode_cntl || color_mask != other.color_mask) {
      return false;
    }
    for (uint32_t i = 0; i < kMaxPipelineStreams; ++i) {
      if (strides[i] != other.strides[i])
        return false;
    }
    return true;
  }
};

}  // namespace

// The one predicate the upload path and the input layout must not disagree
// about. Kept next to MapVertexFormat, which is the other half of the same
// decision.
bool VertexFormatRepacksToSnorm8(uint32_t type) {
  return (type & 0x3Fu) == kVf_2_10_10_10 && ((type >> 8) & 1u) != 0 && ((type >> 9) & 1u) == 0;
}

// Deliberately outside the anonymous namespace: the type is named in the public
// header, so it has to be the same type there and here.
struct GuestPipeline {
  PipelineKey key;
  std::unique_ptr<RenderPipeline> pipeline;

  // Host input slots, densely numbered from 0. They cannot carry the guest's own
  // stream numbers: setVertexBuffers takes an array of views positionally, with
  // view `i` belonging to slot `startSlot + i`, so a pipeline that reads guest
  // stream 0 and the missing-attribute stream would need seventeen views to
  // describe two buffers. `slot_streams` is the mapping back, one guest stream
  // per host slot, with kNullInputSlot meaning the zero-filled buffer.
  RenderInputSlot slots[kMaxPipelineStreams + 1];
  uint32_t slot_streams[kMaxPipelineStreams + 1] = {};
  uint32_t slot_count = 0;
  bool uses_null_slot = false;

  // Which texture/sampler slots the bound pixel shader declares, straight out
  // of the pack. The other stages hold whatever fetch constant was last written
  // there, which the guest never fetches from because its shader does not name
  // the slot; anything that reads all sixteen is reading state nothing owns.
  uint32_t texture_mask = 0;

  // The two shaders' literal constant pools, resolved once here so the draw
  // path does not have to look a shader up per draw. Each is 64 bytes that
  // overlay constants 252..255 of its own bank. See GuestShader::literals.
  const uint8_t* vertex_literals = nullptr;
  const uint8_t* pixel_literals = nullptr;
};

namespace {

std::vector<std::unique_ptr<GuestPipeline>> g_pipelines;
std::unique_ptr<RenderPipelineLayout> g_layout;
bool g_layout_failed = false;

// One RenderShader per guest table slot, created on first use and kept. The
// blobs live in the pack's buffer, which is alive for the process, so this only
// caches the driver-side object.
std::unique_ptr<RenderShader> g_vertex_shaders[d3d::kShaderTableEntries];
std::unique_ptr<RenderShader> g_pixel_shaders[d3d::kShaderTableEntries];

uint64_t g_requests = 0;
uint64_t g_hits = 0;

RenderPipelineLayout* EnsureLayout(RenderDevice* device) {
  if (g_layout)
    return g_layout.get();
  if (g_layout_failed)
    return nullptr;

  // Two sets, not one: t0..t15 in set 0 and s0..s15 in set 1, which is register
  // space 1 in the emitted HLSL because Plume maps a set's index onto
  // RegisterSpace.
  //
  // They are split because the two live in different heaps with very different
  // sizes. A D3D12 shader-visible sampler heap holds 1024 descriptors against
  // the view heap's 65536, so a cache of sets carrying sixteen samplers each
  // exhausts the sampler heap after sixty four sets -- which this title reaches
  // on loading a save. Splitting them means the many distinct *texture*
  // combinations cost only view descriptors, and the handful of distinct
  // sampler combinations cost sampler descriptors.
  //
  // The whole range is reserved in each whatever a given pixel shader declares.
  // The pack carries a texture slot mask per shader, so a narrower layout per
  // shader is possible; it would buy nothing but more layouts to manage.
  const RenderDescriptorRange texture_ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, kTextureSlots),
  };
  const RenderDescriptorRange sampler_ranges[] = {
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 0, kTextureSlots),
  };
  const RenderDescriptorSetDesc set_descs[] = {
      RenderDescriptorSetDesc(texture_ranges, uint32_t(std::size(texture_ranges))),
      RenderDescriptorSetDesc(sampler_ranges, uint32_t(std::size(sampler_ranges))),
  };

  // The four constant banks, as root descriptors rather than a descriptor set:
  // they change per draw and a root CBV is the cheapest way to point at a new
  // slice of an upload buffer. b0/b1 are the vertex float and bool banks,
  // b2/b3 the pixel ones, and b4 is the alpha test, which is render state
  // rather than a guest constant bank: the console tests alpha in fixed
  // function hardware and the host has to do it in the shader.
  //
  // Register space 2, matching the generated HLSL: under Vulkan these become a
  // push descriptor set at set index 2, and space 0 is already the textures'.
  // See CB_SPACE in scripts/xenos_hlsl.py for why they cannot share a space.
  const RenderRootDescriptorDesc root_descriptors[] = {
      RenderRootDescriptorDesc(0, kConstantRegisterSpace, RenderRootDescriptorType::CONSTANT_BUFFER),
      RenderRootDescriptorDesc(1, kConstantRegisterSpace, RenderRootDescriptorType::CONSTANT_BUFFER),
      RenderRootDescriptorDesc(2, kConstantRegisterSpace, RenderRootDescriptorType::CONSTANT_BUFFER),
      RenderRootDescriptorDesc(3, kConstantRegisterSpace, RenderRootDescriptorType::CONSTANT_BUFFER),
      RenderRootDescriptorDesc(4, kConstantRegisterSpace, RenderRootDescriptorType::CONSTANT_BUFFER),
  };

  RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = set_descs;
  desc.descriptorSetDescsCount = uint32_t(std::size(set_descs));
  desc.rootDescriptorDescs = root_descriptors;
  desc.rootDescriptorDescsCount = uint32_t(std::size(root_descriptors));
  desc.allowInputLayout = true;

  g_layout = device->createPipelineLayout(desc);
  if (!g_layout) {
    g_layout_failed = true;
    REXLOG_ERROR(
        "native_renderer: could not create the guest pipeline layout, so no guest geometry will "
        "draw");
    return nullptr;
  }
  return g_layout.get();
}

RenderShader* EnsureShader(RenderDevice* device, const GuestShader& shader,
                           std::unique_ptr<RenderShader>& cached) {
  if (cached)
    return cached.get();

  const RenderShaderFormat format = PlumeShaderFormat();
  if (format == RenderShaderFormat::DXIL && shader.dxil_size != 0)
    cached = device->createShader(shader.dxil, shader.dxil_size, "main", format);
  else if (format == RenderShaderFormat::SPIRV && shader.spirv_size != 0)
    cached = device->createShader(shader.spirv, shader.spirv_size, "main", format);
  return cached.get();
}

// D3D12 requires a pixel shader's input signature to be a subset of the bound
// vertex shader's output signature. The hardware instead synthesises a constant
// for an unmatched input (0x82267820), so a pair the game forms happily is one
// the host can refuse. Both key lists ship sorted in the pack, so this is a
// merge rather than a search.
bool InterpolatorsSubset(const GuestShader& vertex, const GuestShader& pixel) {
  uint32_t v = 0;
  for (uint32_t p = 0; p < pixel.interpolator_key_count; ++p) {
    const uint8_t key = pixel.interpolator_keys[p];
    while (v < vertex.interpolator_key_count && vertex.interpolator_keys[v] < key)
      ++v;
    if (v >= vertex.interpolator_key_count || vertex.interpolator_keys[v] != key)
      return false;
  }
  return true;
}

}  // namespace

RenderPipelineLayout* GuestPipelineLayout() { return g_layout.get(); }

const RenderPipeline* GuestPipelineObject(const GuestPipeline* pipeline) {
  return pipeline ? pipeline->pipeline.get() : nullptr;
}

const RenderInputSlot* GuestPipelineInputSlots(const GuestPipeline* pipeline, uint32_t* count) {
  if (pipeline == nullptr) {
    if (count)
      *count = 0;
    return nullptr;
  }
  if (count)
    *count = pipeline->slot_count;
  return pipeline->slots;
}

bool GuestPipelineUsesNullSlot(const GuestPipeline* pipeline) {
  return pipeline != nullptr && pipeline->uses_null_slot;
}

uint32_t GuestPipelineTextureMask(const GuestPipeline* pipeline) {
  return pipeline != nullptr ? pipeline->texture_mask : 0;
}

const uint8_t* GuestPipelineVertexLiterals(const GuestPipeline* pipeline) {
  return pipeline != nullptr ? pipeline->vertex_literals : nullptr;
}

const uint8_t* GuestPipelinePixelLiterals(const GuestPipeline* pipeline) {
  return pipeline != nullptr ? pipeline->pixel_literals : nullptr;
}

const uint32_t* GuestPipelineSlotStreams(const GuestPipeline* pipeline, uint32_t* count) {
  if (pipeline == nullptr) {
    if (count)
      *count = 0;
    return nullptr;
  }
  if (count)
    *count = pipeline->slot_count;
  return pipeline->slot_streams;
}

const GuestPipeline* AcquireGuestPipeline(const PipelineRequest& request) {
  ++g_requests;

  if (request.vertex_slot < 0 || request.pixel_slot < 0) {
    Refuse(kRefuseUnresolvedSlot, "a bound shader object is not in the guest's own shader table");
    return nullptr;
  }
  if (request.declaration == nullptr || request.declaration->serial == 0) {
    Refuse(kRefuseNoDeclaration, "the draw has no decoded vertex declaration");
    return nullptr;
  }

  const RenderPrimitiveTopology topology = MapTopology(request.primitive_type);
  if (topology == RenderPrimitiveTopology::UNKNOWN) {
    Refuse(kRefuseTopology, "see the Xenos PrimitiveType histogram in the draw summary");
    return nullptr;
  }

  PipelineKey key;
  key.vertex_slot = uint8_t(request.vertex_slot);
  key.pixel_slot = uint8_t(request.pixel_slot);
  key.topology = uint8_t(topology);
  key.targets = uint8_t((request.has_color_target ? 1u : 0u) | (request.has_depth_target ? 2u : 0u));
  key.declaration_serial = request.declaration->serial;
  if (request.state.valid) {
    key.depth_control = request.state.depth_control;
    key.blend_control = request.state.blend_control;
    key.mode_cntl = request.state.mode_cntl;
    key.color_mask = request.state.color_mask;
  }

  // Only the streams this declaration references, so an unrelated stream the
  // guest left bound cannot split the cache.
  const VertexDeclaration& decl = *request.declaration;
  const uint32_t element_count =
      decl.element_count < kMaxVertexElements ? decl.element_count : kMaxVertexElements;
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t stream = decl.elements[i].stream;
    if (stream < kMaxPipelineStreams)
      key.strides[stream] = request.strides[stream];
  }

  for (auto& candidate : g_pipelines) {
    if (candidate->key == key) {
      ++g_hits;
      return candidate.get();
    }
  }

  if (decl.truncated) {
    Refuse(kRefuseTooManyElements, "raise kMaxVertexElements, or the declaration layout is wrong");
    return nullptr;
  }

  RenderDevice* device = PlumeDevice();
  if (device == nullptr) {
    Refuse(kRefuseDeviceDown, "the backend is not up yet");
    return nullptr;
  }
  RenderPipelineLayout* layout = EnsureLayout(device);
  if (layout == nullptr)
    return nullptr;

  const GuestShader& vertex = GuestVertexShader(uint32_t(request.vertex_slot));
  const GuestShader& pixel = GuestPixelShader(uint32_t(request.pixel_slot));
  if (!vertex.valid() || !pixel.valid()) {
    Refuse(kRefuseNoBlob, "guest_shaders.bin is missing, stale, or was built without this format");
    return nullptr;
  }
  if (!InterpolatorsSubset(vertex, pixel)) {
    Refuse(kRefuseInterpolatorMismatch,
           "the hardware would have synthesised a constant for the unmatched input; the host will "
           "not link the pair");
    return nullptr;
  }

  RenderShader* vertex_shader = EnsureShader(device, vertex, g_vertex_shaders[request.vertex_slot]);
  RenderShader* pixel_shader = EnsureShader(device, pixel, g_pixel_shaders[request.pixel_slot]);
  if (vertex_shader == nullptr || pixel_shader == nullptr) {
    Refuse(kRefuseNoBlob, "the pack carries no blob in the format this render interface wants");
    return nullptr;
  }

  // The input layout. This is the match 0x82267218 performs when it patches the
  // microcode -- shader input signature against declaration elements, by usage
  // and usage index -- except that the result is a host input layout instead of
  // a rewritten fetch instruction.
  auto pipeline = std::make_unique<GuestPipeline>();
  pipeline->key = key;
  pipeline->texture_mask = pixel.texture_mask;
  pipeline->vertex_literals = vertex.literals;
  pipeline->pixel_literals = pixel.literals;

  std::vector<RenderInputElement> elements;
  elements.reserve(vertex.input_count);

  // Guest stream -> host slot, assigned in order of first use so the slots stay
  // dense. -1 is "not used by this pipeline".
  int32_t host_slot[kMaxPipelineStreams] = {};
  for (int32_t& slot : host_slot)
    slot = -1;
  int32_t null_host_slot = -1;

  for (uint32_t i = 0; i < vertex.input_count; ++i) {
    const GuestVertexInput& input = vertex.inputs[i];
    if (input.usage >= kUsageCount) {
      Refuse(kRefuseVertexFormat, "a shader input signature carries a usage outside D3DDECLUSAGE");
      return nullptr;
    }

    const VertexElement* match = nullptr;
    for (uint32_t e = 0; e < element_count; ++e) {
      if (decl.elements[e].usage == input.usage &&
          decl.elements[e].usage_index == input.usage_index) {
        match = &decl.elements[e];
        break;
      }
    }

    if (match == nullptr) {
      // The declaration has no element for an input the shader declares. The
      // hardware fills it with a constant; the host has no such thing, so the
      // element is pointed at the null stream and the upload path binds a zero
      // buffer there. Declared with three components so w reads as 1.
      if (null_host_slot < 0) {
        null_host_slot = int32_t(pipeline->slot_count);
        pipeline->slot_streams[pipeline->slot_count] = kNullInputSlot;
        pipeline->slots[pipeline->slot_count++] = RenderInputSlot(uint32_t(null_host_slot), 12);
        pipeline->uses_null_slot = true;
      }
      elements.emplace_back(kUsageSemantics[input.usage], input.usage_index, i,
                            RenderFormat::R32G32B32_FLOAT, uint32_t(null_host_slot), 0);
      continue;
    }

    const RenderFormat format = MapVertexFormat(match->type);
    if (format == RenderFormat::UNKNOWN) {
      Refuse(kRefuseVertexFormat, "see the format warning above for which one");
      return nullptr;
    }
    if (match->stream >= kMaxPipelineStreams) {
      Refuse(kRefuseVertexFormat, "a declaration element names a stream above the 16 the guest has");
      return nullptr;
    }

    if (host_slot[match->stream] < 0) {
      host_slot[match->stream] = int32_t(pipeline->slot_count);
      pipeline->slot_streams[pipeline->slot_count] = match->stream;
      pipeline->slots[pipeline->slot_count++] =
          RenderInputSlot(uint32_t(host_slot[match->stream]), key.strides[match->stream]);
    }
    elements.emplace_back(kUsageSemantics[input.usage], input.usage_index, i, format,
                          uint32_t(host_slot[match->stream]), match->offset);
  }

  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout;
  desc.vertexShader = vertex_shader;
  desc.pixelShader = pixel_shader;
  desc.inputSlots = pipeline->slots;
  desc.inputSlotsCount = pipeline->slot_count;
  desc.inputElements = elements.data();
  desc.inputElementsCount = uint32_t(elements.size());
  desc.primitiveTopology = topology;

  if (request.has_color_target) {
    desc.renderTargetFormat[0] = FrameColorFormat();
    desc.renderTargetCount = 1;
  }

  // Render state, out of the guest's register shadows. Before this existed the
  // whole block was a placeholder -- depth on with LESS_EQUAL, no culling, no
  // blending -- which is why alpha blended UI came out opaque.
  const GuestRenderState& state = request.state;

  if (request.has_depth_target) {
    desc.depthTargetFormat = FrameDepthFormat();
    if (state.valid) {
      desc.depthEnabled = state.depth_enabled;
      desc.depthWriteEnabled = state.depth_write;
      desc.depthFunction = MapCompare(state.depth_func);
    } else {
      desc.depthEnabled = true;
      desc.depthWriteEnabled = true;
      desc.depthFunction = RenderComparisonFunction::LESS_EQUAL;
    }
  }

  // Stencil is read but not applied: the host reference and masks live in
  // RB_STENCILREFMASK, and nothing has checked which of the two faces this title
  // uses. Counted so a title that leans on it says so.
  if (state.valid && state.stencil_enabled)
    ++g_stencil_draws;

  // Culling. `face` says which winding is front, and the two cull bits are
  // independent, so "cull both" is expressible on the console and is not on the
  // host; it is a draw that produces nothing, and NONE is the wrong answer for
  // it, so it is counted and left uncalled rather than silently inverted.
  if (state.valid) {
    if (state.cull_front && state.cull_back) {
      ++g_cull_both_draws;
      desc.cullMode = RenderCullMode::NONE;
    } else if (state.cull_front) {
      desc.cullMode = RenderCullMode::FRONT;
    } else if (state.cull_back) {
      desc.cullMode = RenderCullMode::BACK;
    } else {
      desc.cullMode = RenderCullMode::NONE;
    }
    desc.frontFace = state.front_is_clockwise ? RenderFrontFace::CLOCKWISE
                                              : RenderFrontFace::COUNTER_CLOCKWISE;
  } else {
    desc.cullMode = RenderCullMode::NONE;
  }

  if (request.has_color_target && state.valid)
    desc.renderTargetBlend[0] = MapBlend(state);

  pipeline->pipeline = device->createGraphicsPipeline(desc);
  if (!pipeline->pipeline) {
    Refuse(kRefuseCreateFailed,
           "the driver rejected the state object; the input layout or the shader signatures "
           "disagree with what the blobs declare");
    return nullptr;
  }

  REXLOG_INFO(
      "native_renderer: pipeline #{} vs={} ps={} decl serial {} topology {} over {} input "
      "element(s), {} stream(s){}",
      g_pipelines.size(), request.vertex_slot, request.pixel_slot, key.declaration_serial,
      request.primitive_type, elements.size(), pipeline->slot_count,
      pipeline->uses_null_slot ? ", one of them the missing-attribute stream" : "");

  g_pipelines.push_back(std::move(pipeline));
  return g_pipelines.back().get();
}

void LogPipelineSummary() {
  uint64_t refused = 0;
  for (uint64_t count : g_refusals)
    refused += count;

  REXLOG_INFO(
      "native_renderer: pipelines={} requests={} hits={} refused={} (integer vertex inputs {})",
      g_pipelines.size(), g_requests, g_hits, refused, g_integer_inputs);

  for (uint32_t i = 0; i < kRefuseCount; ++i) {
    if (g_refusals[i] != 0)
      REXLOG_INFO("native_renderer:   refused {}x: {}", g_refusals[i], kRefusalNames[i]);
  }

  // Render state the host cannot express, all of it counted rather than
  // substituted for. Zeroes here are what say the mapping above is complete for
  // this title.
  if (g_stencil_draws != 0 || g_cull_both_draws != 0 || g_blend_factor_unknown != 0) {
    REXLOG_INFO(
        "native_renderer:   render state not applied: stencil enabled on {} pipeline(s), both "
        "faces culled on {}, unknown blend factor {}x",
        g_stencil_draws, g_cull_both_draws, g_blend_factor_unknown);
  }

  // The vertex formats the declarations actually use, which is what says whether
  // the packed-format gap above is a real problem for this title or a
  // hypothetical one.
  for (uint32_t format = 0; format < 64; ++format) {
    if (g_format_seen[format] != 0)
      REXLOG_INFO("native_renderer:   vertex format {} used {}x", format, g_format_seen[format]);
  }
}

void ShutdownGuestPipelines() {
  g_pipelines.clear();
  for (auto& shader : g_vertex_shaders)
    shader.reset();
  for (auto& shader : g_pixel_shaders)
    shader.reset();
  g_layout.reset();
  g_layout_failed = false;
}

}  // namespace eternalsonata
