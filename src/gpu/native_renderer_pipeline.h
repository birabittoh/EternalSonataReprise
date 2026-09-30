// eternalsonata - ReXGlue Recompiled Project
//
// Host graphics pipelines for the guest's own shaders.
//
// scripts/gen-guest-shaders.py compiles all 260 guest shaders ahead of time and
// guest_shaders.h hands them out by table slot. This layer assembles the rest of
// a pipeline state object (input layout, render target formats, topology) and
// caches it.
//
// The cache key is the one the guest itself is keyed by, plus what the host
// bakes into a PSO that the guest keeps in registers:
//
//   * the vertex and pixel shader table slots
//   * the vertex declaration, by a hash of its decoded elements. The guest's
//     variant cache (0x82267D08) probes with the serial number at declaration
//     +48, but that serial is assigned lazily and declarations created after
//     boot keep it at 0, so it identifies nothing. The host input layout is
//     built from the elements, so hashing them is stable and exactly as
//     discriminating as needed.
//   * the per-stream strides, because a host input slot carries its stride and
//     the guest's does not: SetStreamSource writes it into the fetch constant,
//     so one declaration under two strides is two host pipelines
//   * the topology and the bound target formats
//
// The pipeline set is small (35 (VS, PS) pairs over 10 declarations in the
// opening hours), so the cache is a linear probe.
//
// Vertex fetch is lifted out of the shader. The input layout is built by
// matching the shader's own vertex input signature (which ships in the pack,
// decoded from the container's fetch patch table) against the bound
// declaration's elements by (D3DDECLUSAGE, usage index), the same match
// 0x82267218 performs when it patches the microcode.
//
// No Plume types appear in this header; the guest facing hooks include it. A
// TU that speaks Plume gets at the objects through
// native_renderer_pipeline_internal.h.

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

// The input slot the widened attributes read from.
//
// A declaration element in an unnormalised integer format has no usable host
// input layout format. The *_UINT spelling carries the right data, but the
// emitted HLSL declares every vertex input as float4 and no host feeds an
// integer typed input layout format into a float register: the raw bits arrive,
// so a bone index of 3 reads as 4.2e-45 and every skinned vertex truncates to
// matrix 0. This title uses k_8_8_8_8 unnormalised for BLENDINDICES on every
// character.
//
// The elements are rebuilt into a stream of their own on upload, four halves
// each, and read as R16G16B16A16_FLOAT. A half holds every integer up to 2048
// exactly, so an 8 bit index survives untouched, and the guest's own stream
// keeps its stride and its other offsets. This is the widening conversion
// described on VertexFormatRepacksToSnorm8 below; it is cheaper here because the
// elements that need it are few and narrow.
inline constexpr uint32_t kWidenedInputSlot = kMaxPipelineStreams + 1;

// Bytes one widened element occupies in that stream: four halves.
inline constexpr uint32_t kWidenedElementBytes = 8;

// One element rebuilt into the widened stream: where the upload reads it out of
// the guest's vertex, and where the input layout expects it in the host one.
struct GuestWidenedElement {
  uint32_t guest_offset = 0;
  uint32_t host_offset = 0;
  uint32_t type = 0;
};

// The pipeline cache and the upload path have to agree about exactly which
// elements this applies to, so both ask here. Only k_8_8_8_8 is widened: it is
// the one this title declares, and it is also the one a half represents without
// loss. A wider integer format would need a float stream instead, and is still
// counted rather than silently mishandled.
bool VertexFormatWidensToHalf4(uint32_t type);

// Signed k_2_10_10_10 has no host input layout format in any of the three
// backends: DXGI carries only the UNORM and UINT spellings, and Plume's own
// R10G10B10A2_UNORM (added for the unsigned case) is all that is available.
// This title uses the signed spelling for most of its normals and tangents.
//
// It is repacked on upload into R8G8B8A8_SNORM, the same four bytes, so the
// vertex stride and every element offset stay what the guest declared and the
// declaration stays out of the shader's key. Precision drops from 10 bits per
// component to 8, and the 2 bit w to 8, which is what many titles ship natively
// for unit length normals and tangents. A widening conversion into a host layout
// with its own offsets would need the input layout built from the host offsets
// rather than the guest's.
//
// The pipeline cache and the upload path have to agree about which elements
// this applies to, so both ask here.
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

  // Build with the sprite batcher's shader pair instead of the slots' own. The
  // slots stay what the guest bound, so everything keyed on them still applies.
  bool sprite_batch = false;

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
