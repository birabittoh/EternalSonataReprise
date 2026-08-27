// eternalsonata - ReXGlue Recompiled Project
//
// The Plume side of the pipeline cache, for the translation units that already
// speak Plume. Everything else uses native_renderer_pipeline.h, which is free
// of d3d12.h and windows.h.

#pragma once

#include <plume_render_interface.h>

#include "native_renderer_pipeline.h"

namespace eternalsonata {

// The one pipeline layout every guest pipeline shares. Uniform by construction:
// the shader blobs all declare the same resource shape, so nothing about a
// particular shader changes it.
//
//   root descriptor 0  b0  vertex float constants,  float4 c[256]
//   root descriptor 1  b1  vertex bool/loop constants
//   root descriptor 2  b2  pixel float constants
//   root descriptor 3  b3  pixel bool/loop constants
//   descriptor set 0       t0..t15 textures, s0..s15 samplers
//
// The two float banks are separate registers because the guest's are separate
// banks (device+1792 and device+5888) while a host root signature makes one
// buffer visible to every stage. scripts/xenos_hlsl.py emits the matching
// registers per shader type.
//
// Null before the first pipeline is built, or if the device is not up.
plume::RenderPipelineLayout* GuestPipelineLayout();

// The register space the five constant buffers are declared in, which is also
// the descriptor set index they occupy under Vulkan. Not space 0, because that
// belongs to the textures and DXC maps a b register onto a SPIR-V binding
// number inside its own space, so b0..b4 there would collide with t0..t4. See
// CB_SPACE in scripts/xenos_hlsl.py, which has to agree with this.
inline constexpr uint32_t kConstantRegisterSpace = 2;

// Root descriptor indices, in the order the layout declares them.
inline constexpr uint32_t kRootVertexFloatConstants = 0;
inline constexpr uint32_t kRootVertexBoolConstants = 1;
inline constexpr uint32_t kRootPixelFloatConstants = 2;
inline constexpr uint32_t kRootPixelBoolConstants = 3;

// The alpha test, which is not a guest constant bank at all: it is render state
// (RB_COLORCONTROL's compare function and RB_ALPHA_REF), handed to the pixel
// shader because the console tests alpha in fixed function hardware and no host
// pipeline state object can express that. See PRELUDE_ALPHA_TEST in
// scripts/xenos_hlsl.py.
inline constexpr uint32_t kRootAlphaTest = 4;

// The descriptor set indices, which are also the HLSL register spaces the
// resources are declared in: textures are `t0..t15, space0` and samplers
// `s0..s15, space1`.
//
// Two sets rather than one because the heaps behind them are very differently
// sized. D3D12 gives a shader-visible sampler heap 1024 descriptors against the
// view heap's 65536, so a descriptor set carrying sixteen of each costs 1/64th
// of the sampler heap, and a cache of them runs out after sixty four sets. The
// title has many distinct texture combinations and only a handful of distinct
// sampler ones; splitting puts each in the heap that can hold it.
inline constexpr uint32_t kTextureDescriptorSet = 0;
inline constexpr uint32_t kSamplerDescriptorSet = 1;

// The number of texture and sampler slots the layout reserves. The pixel
// shaders in this title declare slots 0..15, so the whole range is bound
// whether or not a given shader uses all of it.
inline constexpr uint32_t kTextureSlots = 16;

// The pipeline state object, and the input slots the draw has to bind vertex
// buffers against. `slots` is indexed by stream, with `slot_used` saying which
// entries the layout actually references; a draw binds a vertex buffer view for
// each used stream and a zero buffer for kNullInputSlot when `uses_null_slot`.
const plume::RenderPipeline* GuestPipelineObject(const GuestPipeline* pipeline);
const plume::RenderInputSlot* GuestPipelineInputSlots(const GuestPipeline* pipeline,
                                                      uint32_t* count);
bool GuestPipelineUsesNullSlot(const GuestPipeline* pipeline);

// Which texture/sampler slots the bound pixel shader declares. The texture
// mirror visits only these: the remaining stages hold stale fetch constants
// that no shader names, and decoding one is at best wasted work and at worst a
// read through an address whose resource is long gone.
uint32_t GuestPipelineTextureMask(const GuestPipeline* pipeline);

// The bound shaders' literal constant pools, 64 bytes each, to overlay on
// constants 252..255 of their own banks. Null when the shader has none. See
// GuestShader::literals for why the guest's own shadows do not carry them.
const uint8_t* GuestPipelineVertexLiterals(const GuestPipeline* pipeline);
const uint8_t* GuestPipelinePixelLiterals(const GuestPipeline* pipeline);

// The shader table slots this pipeline was built from, so a counter in the draw
// path can name the shader it fired on rather than only how often.
void GuestPipelineShaderSlots(const GuestPipeline* pipeline, int* vertex_slot, int* pixel_slot);

// The guest register values this pipeline was built from, as the key kept them.
// Diagnostic: the draw path already has the pipeline in hand, so this avoids
// re-reading the register shadows to ask what state a draw runs under.
void GuestPipelineRenderRegisters(const GuestPipeline* pipeline, uint32_t* depth_control,
                                  uint32_t* blend_control, uint32_t* mode_cntl,
                                  uint32_t* color_mask);

// The guest stream each host slot reads from, parallel to the slot array. A
// value of kNullInputSlot means the missing-attribute stream, which the draw
// path binds a zero-filled buffer to rather than any guest memory.
const uint32_t* GuestPipelineSlotStreams(const GuestPipeline* pipeline, uint32_t* count);

}  // namespace eternalsonata
