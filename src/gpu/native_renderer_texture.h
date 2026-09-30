// eternalsonata - ReXGlue Recompiled Project
//
// The texture mirror: turning a guest texture fetch constant into a host
// texture.
//
// The pipeline cache supplies the programs, the draw layer the vertices and
// constants, the frame layer the render targets, and this the sampled images.
//
// The fetch constant's base address tells the two sources apart:
//
//   * a resolve destination: something the guest rendered this frame and then
//     resolved out of EDRAM. The frame layer owns those, keyed by the address a
//     fetch constant carries, so they are looked up rather than decoded. Roughly
//     7% of the working set.
//   * an asset in guest memory, uploaded by the game's own loader. These are
//     read out of guest memory, untiled, byte swapped and uploaded.
//
// Nothing here parses a file. By the time a texture is bound the game has
// decoded its container into GPU visible memory, and the fetch constant
// describes the result exactly: format, extent, pitch, tiling and endianness.
// That covers every texture the title can bind, not the ones a parser handles.
//
// No Plume types appear in this header; the guest facing code never pulls in
// d3d12.h. The lookup hands back an opaque pointer that is a
// plume::RenderTexture*.

#pragma once

#include <cstdint>
#include <vector>

#include "native_renderer_d3d.h"

namespace eternalsonata {

// The host texture for this fetch constant, or null when it could not be
// produced (an unsupported format, an address outside guest memory, an extent
// that does not fit its allocation). Null is the caller's cue to leave the
// white placeholder bound, so an unhandled texture shows as flat colour rather
// than as a missing draw.
//
// Cached by (address, format, extent), so the decode and upload happen once per
// distinct texture rather than once per bind. Guest thread only.
void* TextureMirrorLookup(uint8_t* memory_base, const TextureFetch& fetch);

// Counters for the swap-time summary: what was resolved from the frame layer,
// what was decoded from guest memory, and every reason a decode was refused.
// Tick the frame counter the content hash is throttled against. Called once per
// guest swap; without it every cached texture is hashed only once, ever, and a
// texture the guest rewrites in place is never noticed.
// Guest memory in [address, address + bytes) has just been overwritten by a
// resolve readback. Tell the mirror, so the cached textures living there keep
// the pixels they already hold instead of re-reading bytes that now belong to a
// render target.
//
// This is the other half of filling a resolve destination. The guest allocates a
// screenshot buffer out of a heap whose pages a cached texture still claims, so
// a faithful fill writes over that texture's source; the mirror would then
// notice the change (by write watch or content hash) and re-decode the texture
// out of render target pixels. Clipping the fill around those ranges instead
// leaves a save preview full of black boxes, since nothing else writes the
// holes.
//
// So the fill writes everything and this re-baselines what it crossed: each
// overlapping entry's content hash is recomputed from the bytes now in guest
// memory and its write watch is re-armed. The entry is self consistent again,
// its host texture still holds its last good pixels, and a genuine later write
// by the guest is still caught, because the hash it is compared against is the
// one this left behind.
//
// `expected_address` is the destination doing the asking, so a texture that IS
// that destination is not touched. Returns how many entries were re-baselined.
uint32_t TextureMirrorRebaselineSources(uint32_t address, uint64_t bytes,
                                        uint32_t expected_address, uint8_t* memory_base);

void TextureMirrorBeginFrame();

// Submit the pending texture copies. Must run before the frame's own command
// list is submitted, or that frame draws against textures never copied.
void TextureUploadFlush();

void LogTextureMirrorSummary();

// Drop every host texture. The device is going away, so this has to run first.
void ShutdownTextureMirror();

}  // namespace eternalsonata
