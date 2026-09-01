// eternalsonata - ReXGlue Recompiled Project
//
// Issuing the guest's draws on the host.
//
// This is the layer that closes the loop the previous three opened. The mirror
// decodes what the guest bound, the frame layer owns the render targets the
// guest renders into, and the pipeline cache turns a (shader, declaration) pair
// into a host pipeline state object. What was still missing is the data: the
// vertices themselves, the indices, and the constant banks the shaders read.
//
// Everything here happens on the guest thread, inside a guest D3D entry point,
// and records into the same command list the clears and resolves record into.
//
// Three properties of the console shape this and are worth stating once:
//
//   * guest memory is big endian. Vertex data, index data and the constant
//     shadows are all stored the way the PowerPC wrote them, so every upload
//     swaps on the way out. The swap width is the *component* width, not the
//     word width, which is what the fetch constant's endian field means when a
//     stream holds 16 bit components.
//   * the vertex data for the dominant draw path does not exist when the draw
//     is announced. BeginVertices hands the game a pointer and the game fills
//     it in; the data is complete only at EndVertices, which is where the draw
//     is issued from.
//   * there is no host equivalent of RECTANGLE_LIST. Each three vertex triple
//     is a quad whose fourth corner the hardware synthesises, so the expansion
//     is done here, on the CPU, while the vertices are being uploaded anyway.
//
// Plume types are deliberately absent from this header, the same way they are
// from the frame and pipeline headers: the guest facing hooks include it.

#pragma once

#include <cstdint>

#include "native_renderer_d3d.h"
#include "native_renderer_pipeline.h"

namespace eternalsonata {

// One bound vertex stream, already resolved to a host pointer into guest
// memory. `size` is what is left of the buffer from `data`, which is what
// SetStreamSource itself computes (the resource's size field minus the offset
// it was given).
struct GuestDrawStream {
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t stride = 0;
};

// Everything a draw needs that is not already in the pipeline.
struct GuestDrawCall {
  // Host pointer to the guest device object. The constant banks are read
  // straight out of it at the offsets in d3d::, rather than from the mirror's
  // own copies, so what is uploaded is what the draw will actually read.
  const uint8_t* device = nullptr;

  // Guest memory base. Needed because guest *addresses* (a texture's base
  // address, say) can only be turned into host pointers with it, and the host
  // pointer above cannot be walked back to it. Anything reading guest memory by
  // address at draw time goes through this.
  uint8_t* memory_base = nullptr;

  const VertexDeclaration* declaration = nullptr;
  const GuestPipeline* pipeline = nullptr;

  // Xenos PrimitiveType, kept raw because the RECTANGLE_LIST expansion needs to
  // know and the topology in the pipeline no longer says.
  uint32_t primitive_type = 0;

  // Vertices, or indices when `indexed`.
  uint32_t count = 0;

  bool indexed = false;
  const uint8_t* indices = nullptr;  // already advanced past start_index
  bool index_32bit = false;
  int32_t base_vertex = 0;

  GuestDrawStream streams[kMaxPipelineStreams];

  // The same state the pipeline was keyed on. The pipeline consumed the depth,
  // cull and blend half of it; what is left for the draw is the alpha test,
  // which is fixed function on the console and has to reach the pixel shader as
  // constants because no host pipeline state can express it.
  GuestRenderState state;
};

// Record the draw. False when it could not be issued, which is always counted
// and reported once per reason.
bool IssueGuestDraw(const GuestDrawCall& call);

// D3DDevice__SetViewport, mirrored. The rectangle is in the bound surface's own
// space and is clamped to it at draw time.
void DrawSetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float min_z,
                     float max_z);

// Hand the draw layer the frame slot it is about to record into, recycling that
// slot's upload arena and the descriptor sets it handed out.
//
// Called once per frame from the Plume layer, and only after that slot's fence
// has been waited on: the arena is what the GPU reads its vertices, indices and
// constants out of, so the frame that last used this slot has to have retired
// before its bytes are handed out again. There is one arena per slot precisely
// so that wait is for a frame `kFramesInFlight` back rather than for the one
// just submitted; see PlumeFramesInFlight.
void BeginGuestDrawFrame(uint32_t slot);

// Counters for the swap-time summary: draws issued, what was uploaded, and
// every reason a draw was dropped.
void LogGuestDrawSummary();

// Drop the arena and the placeholder resources. The device is going away.
void ShutdownGuestDraws();

}  // namespace eternalsonata
