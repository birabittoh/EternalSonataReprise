// eternalsonata - ReXGlue Recompiled Project
//
// The guest frame, expressed as host render targets.
//
// This is the first layer that *produces* something rather than observing it.
// The mirror in native_renderer_d3d.cpp decodes what the guest asked for; this
// turns that into Plume resources and records work into the frame's command
// list.
//
// The model follows the console rather than D3D9. A 360 title renders into
// EDRAM, which the surface objects carve up by tile, and then resolves a
// rectangle of it out to main memory where it becomes sampleable. So a host
// render target here is keyed by the EDRAM region a surface occupies, not by
// the surface object's address: the guest is free to create and destroy surface
// objects over the same tiles, and two surfaces over the same tiles are the same
// image as far as the hardware is concerned.
//
// Plume types are deliberately absent from this header. The guest-facing hooks
// include it, and Plume drags in d3d12.h and windows.h behind it.

#pragma once

#include <cstdint>

#include "native_renderer_d3d.h"

namespace eternalsonata {

// Bind state, mirrored from the guest's own. Passing the decoded surface rather
// than its guest address keeps the decode (and its validity check) on the mirror
// side, where the layout is documented.
void FrameSetColorSurface(uint32_t index, const Surface* surface);
void FrameSetDepthSurface(const Surface* surface);

// D3DDevice__Clear, applied to whatever is bound. `flags` is D3DCLEAR_TARGET,
// _ZBUFFER and _STENCIL; `argb` is the packed D3DCOLOR the guest passed.
void FrameClear(uint32_t flags, uint32_t argb, float z, uint32_t stencil);

// D3DDevice__Resolve. `source` is the low 3 bits of the resolve flags: 0..3 are
// the colour targets, 4 is the depth stencil.
//
// The destination is described by the guest resource it names: its base address
// (which is what a later texture bind is recognised by) and its size. The
// rectangle is the region of the render target being resolved and (dest_x,
// dest_y) is where it lands in the destination, which is not a formality: this
// title renders 720p through EDRAM in horizontal bands and resolves each band
// to its own row range of the same destination texture, so a resolve that
// ignored the offset would show one band stretched over the whole screen.
//
// `dest_fetch` is the destination resource's own fetch constant rather than
// just its address and extent, because the readback path has to lay the image
// back out the way the guest expects to read it: format, tiling, pitch and
// endianness all come from there. `memory_base` is the guest memory base the
// same path writes through.
void FrameResolve(uint32_t source, uint8_t* memory_base, const TextureFetch& dest_fetch,
                  int32_t src_x1, int32_t src_y1, int32_t src_x2, int32_t src_y2, int32_t dest_x,
                  int32_t dest_y);

// Counters for the swap-time summary.
// Look up a resolved texture by guest base address. Returns nullptr if the
// address is not a resolve destination. This is what connects a SetTexture
// bind to the render target the frame layer produced.
//
// The extent the caller is sampling at has to match the one that was resolved.
// An address is not a render target forever: the guest can free the buffer and
// load an ordinary texture into it, and without this an old render target would
// keep being served in its place.
void* FrameResolveTextureByAddress(uint32_t address, uint32_t width, uint32_t height);

// The host texture currently bound as colour target 0, for the read-write
// hazard check in the draw layer: a resolve destination handed back by
// FrameResolveTextureByAddress is the render target itself rather than a copy
// of it, so a shader that samples the scene behind itself can end up reading
// the very image it is writing.
const void* FrameCurrentColorTexture();

// The host frame currently being recorded, counted at the present. The readback
// path needs it to know whether a copy it recorded has actually run yet: a
// resolve in an earlier frame has completed by definition, because the present
// waits on its fence.
uint64_t FrameIndex();

void LogFrameSummary();

}  // namespace eternalsonata
