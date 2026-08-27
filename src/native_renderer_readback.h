// eternalsonata - ReXGlue Recompiled Project
//
// Resolve readback: putting a resolved render target back into guest memory,
// for the guest's own CPU to read.
//
// The rest of this renderer never writes guest memory. A resolve produces a
// host texture keyed by the destination's base address and a later SetTexture
// at that address is handed the texture back (FrameResolveTextureByAddress), so
// everything the *GPU* does with a resolved surface works without guest memory
// ever holding the pixels. What that misses is the guest reading them itself,
// which this title does: the save menu's screenshot and the cross-fade sources
// come out of a resolve and are then touched by guest code. On the emulated
// Xenos backend that is the SDK's `readback_resolve` cvar, which this game ships
// set to "fast" (see kGameDefaults in settings.cpp); this is the same thing for
// the native renderer.
//
// The cost model is the point. Copying every resolve destination back into
// guest memory every frame would mean untiling and byte swapping a megabyte or
// two per frame on the guest thread for buffers nothing ever reads. So the GPU
// side is unconditional and cheap (a DMA into a readback buffer, recorded into
// the frame's own command list), and the expensive CPU side is driven by the
// guest asking for it: the destination's guest pages are left NOACCESS, and the
// guest's own first read of them faults into a handler that fills the memory in
// and lets the read proceed. A destination nothing reads costs three
// VirtualProtect calls a frame and nothing else. See native_renderer_readback.cpp.
//
// Like the frame layer's own header, this deliberately mentions no Plume types.

#pragma once

#include <cstdint>

#include "native_renderer_d3d.h"

namespace eternalsonata {

// Whether the readback path is on at all (the `native_readback_resolve` cvar).
// The frame layer asks before creating a readback buffer, so "off" costs
// nothing beyond the check.
bool ReadbackEnabled();

// Offer the guest the pixels the GPU last read back for a resolve destination.
//
// `dest` describes the guest side: where the destination texture lives, in what
// format, tiled or not, and at what pitch. `pixels` is the mapped readback
// buffer holding the host image at `row_bytes` per row, in the frame layer's
// colour format, and it has to stay mapped and valid until ReadbackForget.
//
// Nothing is written to guest memory here. This arms the destination's pages;
// the write happens if and when the guest reads them.
// `pixels_ready` is false on the very first resolve into a destination, when a
// copy has been recorded but none has ever completed. The destination is still
// registered and armed: a read of it then means making the GPU catch up rather
// than serving the buffer, which is the only way a surface that is resolved and
// read in the same frame -- the save screenshot -- can be answered at all.
// `frame` is the host frame this resolve belongs to. It bounds how long a
// destination stays writable: guest memory stops being a render target when the
// game frees the buffer and loads a texture into it, and only memory the GPU has
// just written is ever written again.
// `pixel_rows` is how many rows the readback buffer actually holds. It is not
// always the destination's height: a ResolvedTexture is looked up by address
// alone and keeps the extent it was first created with, so a later, taller
// resolve to the same address describes more rows than the buffer has.
void ReadbackPublish(uint8_t* memory_base, const TextureFetch& dest, const uint8_t* pixels,
                     uint32_t row_bytes, uint32_t pixel_rows, bool pixels_ready, uint64_t frame);

// This renderer is itself about to read guest memory at `address`, so if that
// address is a resolve destination its bytes have to be there first.
//
// The page trap only sees the *guest* reading, and the guest is not the only
// consumer: when a bind of a resolve destination misses the frame layer's own
// lookup -- a different extent, a different format, an address that has stopped
// being a render target -- the texture mirror falls through to decoding guest
// memory, which nothing has ever written. This is that case, and it is also what
// keeps the arming from blinding the mirror to memory it is allowed to read.
//
// Fills at most once between publishes, i.e. at most once a frame, and returns
// true when it wrote anything.
// `bound` is the fetch constant the read is about to be performed under, which
// is not always the one the resolve wrote: the destination's own resource and
// the stage it is later bound to can disagree about pitch or endianness, and
// laying the image out one way and reading it back the other is a silently
// scrambled texture. A disagreement is reported rather than reconciled.
bool ReadbackFillForRead(const TextureFetch& bound);

// The buffer behind `address` is going away. Disarms its pages, so guest memory
// is left as accessible as it was found.
void ReadbackForget(uint32_t address);
void ReadbackForgetAll();

void LogReadbackSummary();

}  // namespace eternalsonata
