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

// A declaration element also carries the fetch's destination swizzle, in `type`
// bits 10..21: four 3 bit selectors, low to high, where 0..3 pick a fetched
// component, 4 and 5 are the constants 0 and 1, and 6 or 7 leave the
// destination alone. `sub_82267218` shifts it straight into the vfetch word it
// patches, and the emitter then applies it (`_fetch_destination` in
// scripts/xenos_hlsl.py).
//
// That matters here because vertex fetch is lifted to the host input assembler:
// the swizzle baked into the extracted microcode is the identity one, so a
// declaration asking for a permutation gets none unless the input layout
// supplies it. Picking a host format whose component order already matches is
// the only permutation the input assembler can express, which covers the one
// case this title uses, D3DCOLOR.
constexpr uint32_t VertexSwizzle(uint32_t type) { return (type >> 10) & 0xFFFu; }

// zyxw: how a D3DCOLOR element arrives, because the hardware's k_8_8_8_8 fetch
// delivers the low byte in x while D3DCOLOR stores blue there.
constexpr uint32_t kSwizzleBgraToRgba = 0x60Au;

// Whether a swizzle actually moves a component, as opposed to only substituting
// constants for the components the format does not carry. A float2 element
// declares `xy01`, which is not the identity but asks for no permutation: the
// input assembler already fills the components a two component format leaves
// out. Only a selector naming a different component needs the layout's help,
// which is what keeps the counter below from firing on every texture coordinate
// in the title.
constexpr bool VertexSwizzlePermutes(uint32_t type) {
  const uint32_t swizzle = VertexSwizzle(type);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t selector = (swizzle >> (i * 3)) & 7u;
    if (selector < 4 && selector != i)
      return true;
  }
  return false;
}

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
  kRefuseNoGeometryShader,
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
    "point sprites need a geometry shader this backend has not got",
};
uint64_t g_refusals[kRefuseCount] = {};
bool g_refusal_reported[kRefuseCount] = {};

// A refusal loses a draw outright, so the one thing worth knowing about it is
// *which* draw. The counters say how many; this says which (vertex shader,
// pixel shader) pairs, once per distinct triple and capped, so a refusal that
// fires every frame costs one line for the run rather than one per draw.
uint32_t g_refusal_pairs[32] = {};
uint32_t g_refusal_pair_count = 0;

void ReportRefusalPair(RefusalReason reason, int vertex_slot, int pixel_slot) {
  if (vertex_slot < 0 || pixel_slot < 0)
    return;
  const uint32_t triple =
      (uint32_t(reason) << 24) | (uint32_t(vertex_slot) << 12) | uint32_t(pixel_slot);
  for (uint32_t i = 0; i < g_refusal_pair_count; ++i) {
    if (g_refusal_pairs[i] == triple)
      return;
  }
  if (g_refusal_pair_count >= std::size(g_refusal_pairs))
    return;
  g_refusal_pairs[g_refusal_pair_count++] = triple;
  REXLOG_WARN("native_renderer: no pipeline for vs={} ps={}: {}", vertex_slot, pixel_slot,
              kRefusalNames[reason]);
}

// A topology refusal is the one case where the reason alone says nothing: what
// is needed is the type number, so it joins the pair key and gets its own line.
uint32_t g_refused_topologies[16] = {};
uint32_t g_refused_topology_count = 0;

void ReportRefusedTopology(uint32_t primitive_type, int vertex_slot, int pixel_slot) {
  const uint32_t key =
      (primitive_type << 24) | (uint32_t(vertex_slot & 0xFFF) << 12) | uint32_t(pixel_slot & 0xFFF);
  for (uint32_t i = 0; i < g_refused_topology_count; ++i) {
    if (g_refused_topologies[i] == key)
      return;
  }
  if (g_refused_topology_count >= std::size(g_refused_topologies))
    return;
  g_refused_topologies[g_refused_topology_count++] = key;
  REXLOG_WARN("native_renderer: no host topology for Xenos primitive type {} (vs={} ps={})",
              primitive_type, vertex_slot, pixel_slot);
}

void Refuse(RefusalReason reason, const char* detail, int vertex_slot = -1, int pixel_slot = -1) {
  ++g_refusals[reason];
  ReportRefusalPair(reason, vertex_slot, pixel_slot);
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

// Elements whose declaration asks for a component permutation the input layout
// cannot express, which is every permuting swizzle except the D3DCOLOR one
// handled below. Those elements reach the shader with their components in the
// wrong places, so this counter is what says whether substituting a host format
// is enough for this title or whether the swizzle has to be reproduced properly
// -- by repacking on upload, the way VertexFormatRepacksToSnorm8 already does,
// rather than by putting the declaration into the shader key.
uint64_t g_swizzle_unhandled = 0;
bool g_swizzle_reported = false;

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

  // The one permutation the switch below can serve, by picking a host format
  // whose components are already in that order. Anything else is counted.
  const bool swizzle_handled = format == kVf_8_8_8_8 && !is_signed && !is_integer &&
                               VertexSwizzle(type) == kSwizzleBgraToRgba;
  if (VertexSwizzlePermutes(type) && !swizzle_handled) {
    ++g_swizzle_unhandled;
    if (!g_swizzle_reported) {
      g_swizzle_reported = true;
      REXLOG_WARN(
          "native_renderer: vertex format {} declares destination swizzle {:#05x}, which permutes "
          "components. The host input assembler cannot express it, so the element reaches the "
          "shader unswizzled and its components are in the wrong places.",
          format, VertexSwizzle(type));
    }
  }

  // Widened into a stream of its own rather than declared as an integer format
  // the shader cannot read. See kWidenedInputSlot; the layout below routes the
  // element to that slot, and this is only the format it is read back as.
  if (VertexFormatWidensToHalf4(type))
    return RenderFormat::R16G16B16A16_FLOAT;

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
      // A D3DCOLOR element: same four bytes at the same offset, read in the
      // order the declaration's zyxw swizzle asks for. Without this the vertex
      // colour reaches the shader with red and blue transposed, which is a
      // light brown glyph drawn light blue. There is no signed spelling of the
      // host format, so a signed one falls through to be counted instead.
      if (!is_signed && VertexSwizzle(type) == kSwizzleBgraToRgba)
        return RenderFormat::B8G8R8A8_UNORM;
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
constexpr uint32_t kPrimitivePointList = 1;

RenderPrimitiveTopology MapTopology(uint32_t primitive_type) {
  switch (primitive_type) {
    case kPrimitivePointList:
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

    // QUAD_LIST is four corners in winding order per quad, and like
    // RECTANGLE_LIST it has no host topology. It is the easier of the two: all
    // four vertices are supplied, so the expansion is a reorder rather than a
    // synthesis. The vertex path splits each quad into two triangles, so what
    // this has to agree with is again an ordinary triangle list.
    case 13:
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

RenderStencilOp MapStencilOp(GuestStencilOp op) {
  switch (op) {
    case GuestStencilOp::kZero:
      return RenderStencilOp::ZERO;
    case GuestStencilOp::kReplace:
      return RenderStencilOp::REPLACE;
    case GuestStencilOp::kIncrementClamp:
      return RenderStencilOp::INCREMENT_AND_CLAMP;
    case GuestStencilOp::kDecrementClamp:
      return RenderStencilOp::DECREMENT_AND_CLAMP;
    case GuestStencilOp::kInvert:
      return RenderStencilOp::INVERT;
    case GuestStencilOp::kIncrementWrap:
      return RenderStencilOp::INCREMENT_AND_WRAP;
    case GuestStencilOp::kDecrementWrap:
      return RenderStencilOp::DECREMENT_AND_WRAP;
    case GuestStencilOp::kKeep:
    default:
      return RenderStencilOp::KEEP;
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
  uint64_t declaration_identity = 0;
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
  uint32_t stencil_ref_mask = 0;

  bool operator==(const PipelineKey& other) const {
    if (vertex_slot != other.vertex_slot || pixel_slot != other.pixel_slot ||
        topology != other.topology || targets != other.targets ||
        declaration_identity != other.declaration_identity ||
        depth_control != other.depth_control || blend_control != other.blend_control ||
        mode_cntl != other.mode_cntl || color_mask != other.color_mask ||
        stencil_ref_mask != other.stencil_ref_mask) {
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

// The other half of the same decision, for the elements that are widened into a
// stream of their own instead. Bit 9 is `num_format_all`, which reads "not
// normalised", i.e. the fetch delivers the stored integer rather than a
// fraction of its range.
bool VertexFormatWidensToHalf4(uint32_t type) {
  return (type & 0x3Fu) == kVf_8_8_8_8 && ((type >> 9) & 1u) != 0;
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
  RenderInputSlot slots[kMaxPipelineStreams + 2];
  uint32_t slot_streams[kMaxPipelineStreams + 2] = {};
  uint32_t slot_count = 0;
  bool uses_null_slot = false;

  // The elements rebuilt into the widened stream, and the guest stream they are
  // read out of. All of them come from one guest stream: a declaration that
  // spread them over two would need one widened stream each, and nothing in
  // this title does. See kWidenedInputSlot.
  GuestWidenedElement widened[kMaxVertexElements];
  uint32_t widened_count = 0;
  uint32_t widened_stream = 0;
  uint32_t widened_stride = 0;

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

// The point sprite expansion shader, which is one object for the whole title
// rather than one per slot: every vertex shader declares the same output
// signature, so the same geometry shader links against all of them.
std::unique_ptr<RenderShader> g_point_sprite_shader;

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
  //
  // One range per slot, not a single range sixteen descriptors wide. The two
  // spell the same thing to D3D12, where a table range at t0 of sixteen
  // descriptors is t0..t15 either way, but not to Vulkan. Plume turns each range
  // into one VkDescriptorSetLayoutBinding whose descriptorCount is the range's
  // count, so a single range of sixteen declares *one* binding, number zero,
  // that is a sixteen element array. The emitted HLSL declares sixteen separate
  // objects at t0..t15, which DXC compiles to sixteen separate bindings, 0..15.
  // Bindings one upwards then do not exist in the layout at all, and a driver
  // that actually checks rejects the pipeline. Adreno fails to link with "Bind
  // group info mismatches the shader source for symbol xe_texture12" for the
  // first shader that samples any slot other than zero, which is most of them.
  // Desktop drivers happen not to complain, which is why this survived.
  //
  // See the overlay's layout in native_renderer_overlay.cpp for the same shape
  // written out by hand: one range per binding.
  RenderDescriptorRange texture_ranges[kTextureSlots];
  RenderDescriptorRange sampler_ranges[kTextureSlots];
  for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
    texture_ranges[slot] = RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, slot, 1);
    sampler_ranges[slot] = RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, slot, 1);
  }
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

void GuestPipelineShaderSlots(const GuestPipeline* pipeline, int* vertex_slot, int* pixel_slot) {
  if (vertex_slot)
    *vertex_slot = pipeline != nullptr ? int(pipeline->key.vertex_slot) : -1;
  if (pixel_slot)
    *pixel_slot = pipeline != nullptr ? int(pipeline->key.pixel_slot) : -1;
}

void GuestPipelineRenderRegisters(const GuestPipeline* pipeline, uint32_t* depth_control,
                                  uint32_t* blend_control, uint32_t* mode_cntl,
                                  uint32_t* color_mask) {
  if (depth_control)
    *depth_control = pipeline != nullptr ? pipeline->key.depth_control : 0;
  if (blend_control)
    *blend_control = pipeline != nullptr ? pipeline->key.blend_control : 0;
  if (mode_cntl)
    *mode_cntl = pipeline != nullptr ? pipeline->key.mode_cntl : 0;
  if (color_mask)
    *color_mask = pipeline != nullptr ? pipeline->key.color_mask : 0;
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

const GuestWidenedElement* GuestPipelineWidenedElements(const GuestPipeline* pipeline,
                                                        uint32_t* count, uint32_t* stream,
                                                        uint32_t* stride) {
  if (pipeline == nullptr || pipeline->widened_count == 0) {
    if (count)
      *count = 0;
    if (stream)
      *stream = 0;
    if (stride)
      *stride = 0;
    return nullptr;
  }
  if (count)
    *count = pipeline->widened_count;
  if (stream)
    *stream = pipeline->widened_stream;
  if (stride)
    *stride = pipeline->widened_stride;
  return pipeline->widened;
}

const GuestPipeline* AcquireGuestPipeline(const PipelineRequest& request) {
  ++g_requests;

  if (request.vertex_slot < 0 || request.pixel_slot < 0) {
    Refuse(kRefuseUnresolvedSlot, "a bound shader object is not in the guest's own shader table");
    return nullptr;
  }
  if (request.declaration == nullptr) {
    Refuse(kRefuseNoDeclaration, "the draw has no decoded vertex declaration", request.vertex_slot,
           request.pixel_slot);
    return nullptr;
  }

  const RenderPrimitiveTopology topology = MapTopology(request.primitive_type);
  if (topology == RenderPrimitiveTopology::UNKNOWN) {
    // The primitive type is the whole content of this refusal, so report it
    // rather than pointing at a histogram that was never written. Distinct
    // types get distinct lines because the pair key carries the type.
    ReportRefusedTopology(request.primitive_type, request.vertex_slot, request.pixel_slot);
    Refuse(kRefuseTopology, "the type is named per (vs, ps) pair above");
    return nullptr;
  }

  PipelineKey key;
  key.vertex_slot = uint8_t(request.vertex_slot);
  key.pixel_slot = uint8_t(request.pixel_slot);
  key.topology = uint8_t(topology);
  key.targets = uint8_t((request.has_color_target ? 1u : 0u) | (request.has_depth_target ? 2u : 0u));
  key.declaration_identity = request.declaration->identity;
  if (request.state.valid) {
    key.depth_control = request.state.depth_control;
    key.blend_control = request.state.blend_control;
    key.mode_cntl = request.state.mode_cntl;
    key.color_mask = request.state.color_mask;
    // Only when the test is on: RB_STENCILREFMASK keeps changing underneath
    // draws that ignore it, and keying on it unconditionally would split the
    // cache for nothing.
    if (request.state.stencil_enabled)
      key.stencil_ref_mask = request.state.stencil_ref_mask;
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
    Refuse(kRefuseTooManyElements, "raise kMaxVertexElements, or the declaration layout is wrong", request.vertex_slot, request.pixel_slot);
    return nullptr;
  }

  RenderDevice* device = PlumeDevice();
  if (device == nullptr) {
    Refuse(kRefuseDeviceDown, "the backend is not up yet", request.vertex_slot, request.pixel_slot);
    return nullptr;
  }
  RenderPipelineLayout* layout = EnsureLayout(device);
  if (layout == nullptr)
    return nullptr;

  const GuestShader& vertex = GuestVertexShader(uint32_t(request.vertex_slot));
  const GuestShader& pixel = GuestPixelShader(uint32_t(request.pixel_slot));
  if (!vertex.valid() || !pixel.valid()) {
    Refuse(kRefuseNoBlob, "guest_shaders.bin is missing, stale, or was built without this format", request.vertex_slot, request.pixel_slot);
    return nullptr;
  }
  if (!InterpolatorsSubset(vertex, pixel)) {
    Refuse(kRefuseInterpolatorMismatch,
           "the hardware would have synthesised a constant for the unmatched input; the host will "
           "not link the pair", request.vertex_slot, request.pixel_slot);
    return nullptr;
  }

  RenderShader* vertex_shader = EnsureShader(device, vertex, g_vertex_shaders[request.vertex_slot]);
  RenderShader* pixel_shader = EnsureShader(device, pixel, g_pixel_shaders[request.pixel_slot]);
  if (vertex_shader == nullptr || pixel_shader == nullptr) {
    Refuse(kRefuseNoBlob, "the pack carries no blob in the format this render interface wants", request.vertex_slot, request.pixel_slot);
    return nullptr;
  }

  // Point sprites. The console rasterises a POINT_LIST vertex as a screen
  // aligned quad and hands the pixel shader a coordinate across it; a host point
  // list draws one pixel and generates nothing, so without the expansion shader
  // every particle and flower draw in the title is invisible rather than wrong.
  // Metal has no geometry shaders at all, hence the capability check rather than
  // an assumption.
  RenderShader* geometry_shader = nullptr;
  if (request.primitive_type == kPrimitivePointList) {
    if (!device->getCapabilities().geometryShader) {
      Refuse(kRefuseNoGeometryShader, "the point sprite quads cannot be built",
             request.vertex_slot, request.pixel_slot);
      return nullptr;
    }
    geometry_shader = EnsureShader(device, GuestPointSpriteShader(), g_point_sprite_shader);
    if (geometry_shader == nullptr) {
      Refuse(kRefuseNoBlob, "the pack carries no point sprite geometry shader",
             request.vertex_slot, request.pixel_slot);
      return nullptr;
    }
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
  int32_t widened_host_slot = -1;

  for (uint32_t i = 0; i < vertex.input_count; ++i) {
    const GuestVertexInput& input = vertex.inputs[i];
    if (input.usage >= kUsageCount) {
      Refuse(kRefuseVertexFormat, "a shader input signature carries a usage outside D3DDECLUSAGE", request.vertex_slot, request.pixel_slot);
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
      Refuse(kRefuseVertexFormat, "see the format warning above for which one", request.vertex_slot, request.pixel_slot);
      return nullptr;
    }
    if (match->stream >= kMaxPipelineStreams) {
      Refuse(kRefuseVertexFormat, "a declaration element names a stream above the 16 the guest has", request.vertex_slot, request.pixel_slot);
      return nullptr;
    }

    // An unnormalised integer element does not read from the guest's stream at
    // all: the upload rebuilds it, widened, into a stream of its own. Two
    // shader inputs matching the same declaration element share one widened
    // element, which is why this looks for the guest offset first.
    if (VertexFormatWidensToHalf4(match->type)) {
      if (widened_host_slot < 0) {
        widened_host_slot = int32_t(pipeline->slot_count);
        pipeline->widened_stream = match->stream;
        pipeline->slot_streams[pipeline->slot_count] = kWidenedInputSlot;
        pipeline->slots[pipeline->slot_count++] = RenderInputSlot(uint32_t(widened_host_slot), 0);
      } else if (pipeline->widened_stream != match->stream) {
        Refuse(kRefuseVertexFormat,
               "a declaration widens integer elements out of two different streams, and only one "
               "widened stream is built", request.vertex_slot, request.pixel_slot);
        return nullptr;
      }

      uint32_t host_offset = 0;
      bool found = false;
      for (uint32_t w = 0; w < pipeline->widened_count && !found; ++w) {
        if (pipeline->widened[w].guest_offset == match->offset) {
          host_offset = pipeline->widened[w].host_offset;
          found = true;
        }
      }
      if (!found) {
        if (pipeline->widened_count == kMaxVertexElements) {
          Refuse(kRefuseVertexFormat, "more widened elements than a declaration can hold",
                 request.vertex_slot, request.pixel_slot);
          return nullptr;
        }
        host_offset = pipeline->widened_count * kWidenedElementBytes;
        pipeline->widened[pipeline->widened_count++] =
            GuestWidenedElement{match->offset, host_offset, match->type};
        pipeline->widened_stride = pipeline->widened_count * kWidenedElementBytes;
      }
      elements.emplace_back(kUsageSemantics[input.usage], input.usage_index, i, format,
                            uint32_t(widened_host_slot), host_offset);
      continue;
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

  // The widened slot's stride is only known once every element that lands in it
  // has been seen, so it is filled in here rather than where the slot was
  // claimed.
  if (widened_host_slot >= 0) {
    pipeline->slots[widened_host_slot] =
        RenderInputSlot(uint32_t(widened_host_slot), pipeline->widened_stride);
  }

  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout;
  desc.vertexShader = vertex_shader;
  desc.geometryShader = geometry_shader;
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

  // Stencil. This title masks whole passes with it: the outline pass (ps_101)
  // tests NOT_EQUAL against ref 0 through read mask 0x02, and the scene's own
  // meshes tag that bit by writing the reference on zpass. Dropping it drew the
  // outline over the entire screen, silhouetting flat ground and every mesh
  // border, which is what this exists for.
  //
  // The reference is part of the pipeline rather than a command, so a draw that
  // only changes it gets its own PSO; RB_STENCILREFMASK is in the key for that
  // reason.
  if (request.has_depth_target && state.valid && state.stencil_enabled) {
    desc.stencilEnabled = true;
    desc.stencilReadMask = state.stencil_read_mask;
    desc.stencilWriteMask = state.stencil_write_mask;
    desc.stencilReference = state.stencil_ref;
    desc.stencilFrontFace.compareFunction = MapCompare(state.stencil_func);
    desc.stencilFrontFace.failOp = MapStencilOp(state.stencil_fail_op);
    desc.stencilFrontFace.passOp = MapStencilOp(state.stencil_zpass_op);
    desc.stencilFrontFace.depthFailOp = MapStencilOp(state.stencil_zfail_op);
    desc.stencilBackFace.compareFunction = MapCompare(state.stencil_func_bf);
    desc.stencilBackFace.failOp = MapStencilOp(state.stencil_fail_op_bf);
    desc.stencilBackFace.passOp = MapStencilOp(state.stencil_zpass_op_bf);
    desc.stencilBackFace.depthFailOp = MapStencilOp(state.stencil_zfail_op_bf);
    ++g_stencil_draws;
  }

  // Culling. `face` says which winding is front, and the two cull bits are
  // independent, so "cull both" is expressible on the console and is not on the
  // host; it is a draw that produces nothing, and NONE is the wrong answer for
  // it, so it is counted and left uncalled rather than silently inverted.
  // A point has no winding for the setup unit to cull against, so the console
  // ignores the cull bits for a point list. The quads the geometry shader builds
  // do have one, and it would be culled by whatever the scene last set.
  if (request.primitive_type == kPrimitivePointList) {
    desc.cullMode = RenderCullMode::NONE;
  } else if (state.valid) {
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

  // Clip against the near and far planes rather than clamping to them. Plume
  // defaults this off, which is D3D12's DepthClipEnable=FALSE, and the console
  // clips unless PA_CL_CLIP_CNTL says otherwise. Without it a draw the guest
  // has deliberately pushed outside the frustum is not discarded: its vertices
  // are merely clamped, and one with a near zero or negative w smears across
  // the whole target. That is what covered the in-game photo's render pass in
  // flat brown while the identical draw was harmless on screen.
  desc.depthClipEnabled = true;

  if (request.has_color_target && state.valid)
    desc.renderTargetBlend[0] = MapBlend(state);

  pipeline->pipeline = device->createGraphicsPipeline(desc);
  if (!pipeline->pipeline) {
    Refuse(kRefuseCreateFailed,
           "the driver rejected the state object; the input layout or the shader signatures "
           "disagree with what the blobs declare", request.vertex_slot, request.pixel_slot);
    return nullptr;
  }

  REXLOG_INFO(
      "native_renderer: pipeline #{} vs={} ps={} decl {:016X} topology {} over {} input "
      "element(s), {} stream(s){}",
      g_pipelines.size(), request.vertex_slot, request.pixel_slot, key.declaration_identity,
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
      "native_renderer: pipelines={} requests={} hits={} refused={} (integer vertex inputs {}, "
      "unhandled vertex swizzles {})",
      g_pipelines.size(), g_requests, g_hits, refused, g_integer_inputs, g_swizzle_unhandled);

  for (uint32_t i = 0; i < kRefuseCount; ++i) {
    if (g_refusals[i] != 0)
      REXLOG_INFO("native_renderer:   refused {}x: {}", g_refusals[i], kRefusalNames[i]);
  }

  // Render state the host cannot express, all of it counted rather than
  // substituted for. Zeroes here are what say the mapping above is complete for
  // this title.
  if (g_cull_both_draws != 0 || g_blend_factor_unknown != 0) {
    REXLOG_INFO(
        "native_renderer:   render state not applied: both faces culled on {}, unknown blend "
        "factor {}x",
        g_cull_both_draws, g_blend_factor_unknown);
  }
  if (g_stencil_draws != 0)
    REXLOG_INFO("native_renderer:   stencil enabled on {} pipeline(s)", g_stencil_draws);

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
  g_point_sprite_shader.reset();
  g_layout.reset();
  g_layout_failed = false;
}

}  // namespace eternalsonata
