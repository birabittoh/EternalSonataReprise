// eternalsonata - ReXGlue Recompiled Project
//
// Where the frame time goes.
//
// The design rule the rest of the native renderer follows is that anything not
// implemented is counted rather than silently substituted. This is the same idea
// applied to time: every phase that could plausibly own the frame gets an
// accumulator, and the summary prints what is left over as `other`, so a phase
// that is not instrumented shows up as a gap rather than as nothing.
//
// Everything here runs on the guest's render thread, inside guest D3D entry
// points, and the present runs there too. That is the only reason plain
// non-atomic counters are sound; if any of this ever moves off that thread the
// accumulators have to become atomics.
//
// The clock is read twice per zone. At the innermost zone (the per draw ones)
// that is roughly 40 ns of `steady_clock` against a draw that costs a few
// microseconds, so the instrument perturbs the measurement by about a percent.
// Zones inside the per vertex and per texel loops would not be; do not put any
// there.

#ifndef ETERNALSONATA_NATIVE_RENDERER_PROFILE_H
#define ETERNALSONATA_NATIVE_RENDERER_PROFILE_H

#include <chrono>
#include <cstdint>

namespace eternalsonata {

// Ordered roughly outermost first, which is also the order the summary prints.
// `kPhaseCount` is the array size; keep `kPhaseNames` in step with it.
enum ProfilePhase : uint32_t {
  kPhasePresent = 0,     // PlumePresentFrame, everything in it
  kPhaseFenceWait,       // the CPU/GPU serialisation inside that present
  kPhaseDraw,            // IssueGuestDraw, everything in it
  kPhaseVertexUpload,    // stream swap and copy into the arena
  kPhaseIndexUpload,     // index swap and copy
  kPhaseConstantUpload,  // the four constant banks
  kPhaseTextureBind,     // the fetch decode and sampler loop, hashing included
  kPhaseFetchDecode,     // GetBoundTextureFetch alone, per slot
  kPhaseMirrorLookup,    // TextureMirrorLookup alone, per slot
  kPhaseAcquireSampler,  // AcquireSampler alone, per slot
  kPhaseTextureHash,     // the content hash alone, a subset of the above
  kPhaseTextureUpload,   // decode, untile and upload of a texture that changed
  kPhaseGuestPointer,    // the VirtualQuery aperture walk behind a source read
  kPhaseDescriptorSet,   // binding set lookup and creation
  // The five below account for the rest of IssueGuestDraw. Without them the
  // summary charged roughly 3 us per draw to `draw` itself with no breakdown,
  // which at a couple of thousand draws a frame was the largest single term.
  kPhaseBindTargets,     // FrameBindDrawTargets, per draw
  kPhaseProjectionProbe, // CaptureProjectionProbe, per draw, debug only
  kPhaseStreamSetup,     // the whole vertex stream and view loop, per draw
  kPhaseSwapPlan,        // BuildSwapPlan alone, per stream slot
  kPhaseWaterHash,       // the water probe's per draw vertex hash, debug only
  kPhaseSubmit,          // the command list calls that actually record the draw
  kPhaseDeclDecode,      // vertex declaration decode on the hook thread
  kPhaseReadbackPublish,  // arming a resolve destination for the guest to read
  kPhaseCount,
};

// Nanoseconds accumulated in each phase since the last summary, and the number
// of times each was entered. Defined in native_renderer_draw.cpp.
extern uint64_t g_profile_ns[kPhaseCount];
extern uint64_t g_profile_hits[kPhaseCount];

// GPU time, which none of the zones above can see: they all measure CPU. Two
// timestamps bracket the frame's single command list, so this is the wall time
// the queue spent between the first and the last thing the frame recorded,
// including any bubble inside it. Accumulated over the window in
// native_renderer_plume.cpp and printed by the summary next to the CPU phases.
//
// This is the number that says whether the frame is CPU bound or GPU bound: if
// it is close to the frame time, cutting CPU work moves nothing.
extern uint64_t g_gpu_frame_ns;
extern uint64_t g_gpu_frame_count;

// Wall time the window covers, so every phase can be printed as a percentage of
// something real rather than of the sum of the phases. Started at the first
// swap and restarted by the summary.
extern std::chrono::steady_clock::time_point g_profile_window_start;

class ProfileZone {
 public:
  explicit ProfileZone(ProfilePhase phase)
      : phase_(phase), start_(std::chrono::steady_clock::now()) {}

  ~ProfileZone() {
    const auto end = std::chrono::steady_clock::now();
    g_profile_ns[phase_] +=
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count());
    ++g_profile_hits[phase_];
  }

  ProfileZone(const ProfileZone&) = delete;
  ProfileZone& operator=(const ProfileZone&) = delete;

 private:
  ProfilePhase phase_;
  std::chrono::steady_clock::time_point start_;
};

// Prints one line per phase, then resets the window. Called from the swap hook
// alongside the other summaries.
void LogProfileSummary();

}  // namespace eternalsonata

#endif  // ETERNALSONATA_NATIVE_RENDERER_PROFILE_H
