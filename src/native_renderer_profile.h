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

#include <atomic>
#include <chrono>
#include <cstdint>

namespace eternalsonata {

// Refreshed once a frame from the `native_profile_zones` cvar (see
// ApplyProfileZonesCvar in native_renderer_draw.cpp) rather than read directly
// by ProfileZone, so turning zones off actually removes the clock reads
// instead of trading them for a cvar string lookup per zone.
extern std::atomic<bool> g_profile_zones_enabled;

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
  kPhasePacerWait,       // LimitFrame's own sleep/spin to hit the declared fps
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
      : phase_(phase),
        active_(g_profile_zones_enabled.load(std::memory_order_relaxed)),
        start_(active_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

  ~ProfileZone() {
    if (!active_)
      return;
    const auto end = std::chrono::steady_clock::now();
    g_profile_ns[phase_] +=
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count());
    ++g_profile_hits[phase_];
  }

  ProfileZone(const ProfileZone&) = delete;
  ProfileZone& operator=(const ProfileZone&) = delete;

 private:
  ProfilePhase phase_;
  bool active_;
  std::chrono::steady_clock::time_point start_;
};

// Prints one line per phase, then resets the window. Called from the swap hook
// alongside the other summaries.
void LogProfileSummary();

// The one question the summary above answers only if you read every line of it:
// is this frame waiting on us or on the GPU? Both numbers come from instruments
// that already exist -- the GPU timestamp pair and the fence wait zone -- over a
// short trailing window, so the verdict tracks the scene rather than the run.
//
//   busy  = wall clock minus the fence wait, i.e. time the CPU spent working
//           rather than blocked on the GPU. This is the CPU cost of the frame.
//   gpu   = the queue's own measurement of the same frame.
//
// Whichever is larger is what the frame is waiting on. If neither is close to
// the frame time, something outside both is pacing us (vsync, the guest's own
// throttle, or a sleep), and saying "CPU bound" there would be a lie.
struct FrameBoundStats {
  double frame_ms = 0.0;  // swap to swap wall clock
  double cpu_ms = 0.0;    // of which the CPU was busy
  double gpu_ms = 0.0;    // of which the GPU was busy
  double wait_ms = 0.0;   // of which the CPU sat on the fence
  double pacer_ms = 0.0;  // of which the CPU sat in LimitFrame pacing to the fps cap
  bool gpu_valid = false;
  // "CPU bound" / "GPU bound" / "Present bound" / "measuring". Never empty.
  const char* verdict = "measuring";
};

// Rolls the trailing window forward by one guest swap. Call once per present,
// after the present, so the fence wait and the GPU timestamps of the frame that
// just retired are already in the accumulators.
void ProfileEndFrame();

// Safe to call from any thread; the window is only written by ProfileEndFrame
// on the render thread and a torn read costs one stale frame of display.
FrameBoundStats GetFrameBoundStats();

}  // namespace eternalsonata

#endif  // ETERNALSONATA_NATIVE_RENDERER_PROFILE_H
