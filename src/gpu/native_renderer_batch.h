// eternalsonata - ReXGlue Recompiled Project
//
// Sprite batching for the effect system's quads.
//
// The effect system (sub_820C7538 and its parts) draws every particle and
// sprite as its own 4 vertex BeginVertices/EndVertices draw through vs_015 /
// ps_003: world matrix in c0..c3, view projection in c4..c7, a UV offset in c47
// and a colour in pixel c0. A village scene issues ~2500 of them a frame, and
// the host renderer's fixed cost per draw is what the frame is made of.
//
// Consecutive sprites that share texture, colour, render state and the
// projection differ only in c0..c3. Those are transformed on the host, c0..c3
// is replaced with the identity, and the run goes out as one triangle list.
// Draw order is preserved exactly: a sprite is only ever merged with the ones
// immediately before it, and anything else the guest does in between (another
// draw, a clear, a target or viewport change, a resolve) flushes first.

#pragma once

#include <cstdint>

namespace eternalsonata {

struct GuestDrawCall;
struct PipelineRequest;

// Absorbs the draw into the pending batch when it is a batchable sprite,
// flushing a pending batch it cannot join. Returns true if the draw was
// absorbed and must not be issued by the caller.
bool SpriteBatchAbsorb(const GuestDrawCall& call, const PipelineRequest& request);

// Issues the pending batch, if any. Called before anything that must be
// ordered after the sprites already absorbed.
void SpriteBatchFlush();

// Frame boundary: flushes and rolls the per frame counters.
void SpriteBatchEndFrame();

void LogSpriteBatchSummary();

}  // namespace eternalsonata
