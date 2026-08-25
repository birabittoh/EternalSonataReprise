// eternalsonata - ReXGlue Recompiled Project
//
// Host graphics pipelines for the guest's own shaders.
//
// The offline step (scripts/gen-guest-shaders.py) has already turned all 260
// guest shaders into compiled blobs; guest_shaders.h hands them out by table
// slot. What is missing between a blob and a draw is the rest of a pipeline
// state object: the input layout, the render target formats, and the topology.
// This is where those are assembled and cached.
//
// The cache key is the one the guest itself is keyed by, plus what the host
// bakes into a PSO that the guest keeps in registers:
//
//   * the vertex and pixel shader table slots
//   * the vertex declaration, by the serial number the guest assigns it -- this
//     is exactly half of the variant cache key that 0x82267D08 probes with
//   * the per-stream strides, because a host input slot carries its stride and
//     the guest's does not: SetStreamSource writes it into the fetch constant,
//     so the same declaration under two strides is two host pipelines
//   * the topology and the bound target formats
//
// The measured pipeline set is small -- 35 (VS, PS) pairs over 10 declarations
// in the opening hours -- which is what makes a cache with a linear probe the
// right shape here rather than something with a hash table in it.
//
// Vertex fetch is lifted out of the shader, so the input layout is built by
// matching the shader's own vertex input signature (which ships in the pack,
// decoded from the container's fetch patch table) against the bound
// declaration's elements by (D3DDECLUSAGE, usage index). That is the same match
// 0x82267218 performs when it patches the microcode; doing it here produces an
// input layout instead of a patched instruction stream.
//
// Plume types are deliberately absent from this header, the same way they are
// from native_renderer_frame.h: the guest-facing hooks include it, and Plume
// drags d3d12.h and windows.h in behind it. A TU that speaks Plume gets at the
// objects through native_renderer_pipeline_internal.h.

#pragma once

#include <cstdint>

#include "native_renderer_d3d.h"
#include "native_renderer_state.h"

namespace eternalsonata {

// The most streams a declaration is expected to reference. The guest's own
// per-stream fetch constant slot array (device+12392) is 16 bytes, one byte per
// stream, so 16 is the hardware bound rather than a guess.
inline constexpr uint32_t kMaxPipelineStreams = 16;

// The input slot the "missing attribute" fetch reads from. When a vertex shader
// declares an input the bound declaration has no element for, the hardware
// synthesises a constant rather than failing (0x82267218 writes a fixed fetch
// for it). A host input layout has no such thing, and D3D12 requires every
// shader input to be present, so the element is declared against this slot and
// the vertex upload path binds a zero-filled buffer there. The format chosen is
// three-component, so the missing w reads as 1.0 the way the hardware's
// constant fill does.
inline constexpr uint32_t kNullInputSlot = kMaxPipelineStreams;

// Signed k_2_10_10_10 has no host input layout format in any of the three
// backends: DXGI carries only the UNORM and UINT spellings, and Plume's own
// R10G10B10A2_UNORM (added for the unsigned case) is the whole of what is
// available. This title uses the signed spelling for most of its normals and
// tangents, so refusing it costs most of the geometry.
//
// The way out taken here is to repack it on upload into R8G8B8A8_SNORM, which
// is the same four bytes, so the vertex stride and every element offset stay
// exactly what the guest declared and the declaration stays out of the shader's
// key. It costs precision: 10 bits per component down to 8, and the 2 bit w down
// to the same 8. For unit length normals and tangents that is what a great many
// titles ship natively; if it ever shows, the fix is a widening conversion into
// a host layout of our own offsets, which the upload pass could do in the same
// loop but the pipeline's input layout would then have to be built from the host
// offsets rather than the guest's.
//
// The pipeline cache and the upload path have to agree about exactly which
// elements this applies to, so both ask here.
bool VertexFormatRepacksToSnorm8(uint32_t type);

// What a draw needs a pipeline for. Everything here is state the guest has
// already set by the time a draw entry point is reached.
struct PipelineRequest {
  int vertex_slot = -1;  // guest vertex shader table index, -1 if unresolved
  int pixel_slot = -1;
  const VertexDeclaration* declaration = nullptr;

  // Xenos PrimitiveType, not a D3D9 one: the traffic is 4 (TRIANGLE_LIST),
  // 6 (TRIANGLE_STRIP), 8 (RECTANGLE_LIST) and 1 (POINT_LIST).
  uint32_t primitive_type = 0;

  // Stride per stream, as SetStreamSource last set it. Only the streams the
  // declaration references are read.
  uint32_t strides[kMaxPipelineStreams] = {};

  bool has_color_target = false;
  bool has_depth_target = false;

  // The guest's depth, cull, blend and colour write state, read straight out of
  // the register shadows rather than mirrored from the setters. The raw register
  // values are part of the cache key, so a state change makes a new pipeline the
  // same way a shader change does. An invalid state (no device yet) keeps the
  // conservative defaults this used before the state was read at all.
  GuestRenderState state;
};

// An assembled pipeline. Opaque here; native_renderer_pipeline_internal.h turns
// one into the Plume objects a command list needs.
struct GuestPipeline;

// Look the request up, building the pipeline if it is new. Returns null when
// the pipeline cannot be built, which is always for a reason worth logging and
// always logged once per reason rather than per draw: an unresolved shader
// slot, a shader the pack does not carry, a declaration element in a vertex
// format with no host equivalent, or a pixel shader whose interpolator inputs
// are not a subset of the vertex shader's outputs.
const GuestPipeline* AcquireGuestPipeline(const PipelineRequest& request);

// Counters for the swap-time summary: how many distinct pipelines exist, how
// many requests were served, and every reason a request was refused.
void LogPipelineSummary();

// Drop every pipeline. The device is going away, so this has to run first.
void ShutdownGuestPipelines();

}  // namespace eternalsonata
