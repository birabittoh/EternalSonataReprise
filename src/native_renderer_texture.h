// eternalsonata - ReXGlue Recompiled Project
//
// The texture mirror: turning a guest texture fetch constant into a host
// texture.
//
// This is the last of the four resource kinds a draw needs. The pipeline cache
// supplies the programs, the draw layer the vertices and constants, the frame
// layer the render targets; what was still a 1x1 white placeholder is the
// sampled image.
//
// There are two sources for one, and the fetch constant's base address is what
// tells them apart:
//
//   * a resolve destination, i.e. something the guest rendered this frame and
//     then resolved out of EDRAM. The frame layer already owns those, keyed by
//     the very address a fetch constant carries, so they are looked up rather
//     than decoded. Roughly 7% of the working set by the mirror's own count.
//   * an asset in guest memory, uploaded by the game's own loader. These have
//     to be read out of guest memory, untiled, byte swapped and uploaded.
//
// Nothing here parses a file. By the time a texture is bound, the game has
// already decoded its container into GPU-visible memory, and the fetch constant
// describes the result exactly: format, extent, pitch, tiling and endianness.
// That is a far better source than the on-disk format, and it covers every
// texture the title can bind rather than the ones a parser happens to handle.
//
// Plume types are kept out of this header, the same way they are out of the
// frame and draw headers, so the guest-facing code never pulls in d3d12.h. The
// lookup hands back an opaque pointer that is a plume::RenderTexture*.

#pragma once

#include <cstdint>

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
void TextureMirrorBeginFrame();

void LogTextureMirrorSummary();

// Drop every host texture. The device is going away, so this has to run first.
void ShutdownTextureMirror();

}  // namespace eternalsonata
