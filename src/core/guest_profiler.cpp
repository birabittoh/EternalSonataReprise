#include "guest_profiler.h"

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#include <imgui.h>
#include <rex/cvar.h>

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// The overlay turns sampling on by itself while it is open, so this is only for
// a headless run whose log you want to read afterwards.
REXCVAR_DEFINE_BOOL(guest_profile, false, "Eternal Sonata",
                    "Log the guest profiler's per-second summary (which guest functions own the "
                    "frame, and how long the frame spent blocked in each guest wait primitive)")
    .debug_only();

// 1 kHz is about 1% overhead against a 16.7 ms frame (a suspend/unwind/resume
// round trip costs a few microseconds) and gives ~16 samples per frame, which
// is enough to rank functions over a one-second window even though it says
// nothing about any individual frame.
REXCVAR_DEFINE_INT32(guest_profile_hz, 1000, "Eternal Sonata",
                     "Sampling rate of the guest profiler, in Hz")
    .range(50, 8000)
    .debug_only();

REXCVAR_DEFINE_INT32(guest_profile_top, 12, "Eternal Sonata",
                     "How many entries the guest profiler's inclusive and leaf tables keep")
    .range(1, 64)
    .debug_only();

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Blocking primitives
//
// The complete set. Found by taking every wait-shaped xboxkrnl import the xex
// pulls in and listing its call sites; there are eight, and they are all thin
// wrappers in the game's own threading layer:
//
//   KeWaitForSingleObject     sub_822722B0, sub_822A7D98, sub_822A7E60
//   KeWaitForMultipleObjects  sub_822A7F10
//   KeDelayExecutionThread    sub_822546E0
//   NtWaitForSingleObjectEx   sub_822548A0, sub_82252668, sub_822529E8
//
// The counters below are per frame, reset by the summary, and only accumulate
// on the present thread: the audio and IO threads block constantly and by
// design, and counting them would bury the one thread that has a deadline.
// ---------------------------------------------------------------------------

// One kind per wrapper, not per underlying import. Three separate call sites
// funnel into NtWaitForSingleObjectEx and they are not the same event; lumping
// them cost a round trip to find out which one was firing.
enum WaitKind : uint32_t {
  kWaitDelay = 0,   // sub_822546E0  KeDelayExecutionThread
  kWaitNt548A0,     // sub_822548A0  NtWaitForSingleObjectEx
  kWaitNt52668,     // sub_82252668  NtWaitForSingleObjectEx
  kWaitNt529E8,     // sub_822529E8  NtWaitForSingleObjectEx
  kWaitKe722B0,     // sub_822722B0  KeWaitForSingleObject
  kWaitKeA7D98,     // sub_822A7D98  KeWaitForSingleObject
  kWaitKeA7E60,     // sub_822A7E60  KeWaitForSingleObject
  kWaitKeMultiple,  // sub_822A7F10  KeWaitForMultipleObjects
  kWaitKindCount,
};

const char* const kWaitNames[kWaitKindCount] = {
    "delay_822546E0",  "nt_822548A0",     "nt_82252668",     "nt_822529E8",
    "ke_822722B0",     "ke_822A7D98",     "ke_822A7E60",     "kemulti_822A7F10"};

struct WaitCounters {
  uint64_t calls[kWaitKindCount] = {};
  uint64_t ns[kWaitKindCount] = {};
};

// ---------------------------------------------------------------------------
// Guest zones
//
// Named guest functions timed exactly, rather than sampled. The sampler ranks
// what is expensive but cannot say "per model" or "per animation entry"; a
// call count can, and the count is the number that settles whether a cost
// scales with what is on screen.
//
// The set below is the animation update and the render task above it, found by
// following the sampler's own inclusive chain:
//
//   sub_82132A08 (task scheduler)
//     sub_820EDC38 -> sub_82124C40 -> sub_82125378   the render task
//     sub_82123508 -> sub_82123470 -> sub_82125378   the auxiliary render task
//     sub_820C7538                                   per-model animation update
//       sub_820C8378                                 per animation entry
//       sub_820C9550                                 per model part
//
// sub_82125378 (gather and draw one render list) has exactly two callers:
// sub_82124C40, which calls it once, and sub_82123470. Addresses near
// 0x820AD680 that look like callers are .pdata RUNTIME_FUNCTION entries, not
// vtable slots.
//
// sub_82123470 is recursive. It draws one scene node, then walks that node's
// children of class 0x4F424A via sub_82106730/sub_82106790 and recurses into
// each, contributing one render list flush per node in the subtree. Its only
// entry point is sub_82123508, which has no code xrefs and is reached through a
// task vtable from the scheduler, gated on dword_82440618 and a flag at
// *(node+320)+24.
//
// auxnode's ms/frame double counts nested levels because of that recursion. Its
// call count is exact; auxtask's ms is the total for the subtree.
//
// sub_820C7538 is the one that reads byte_82465F90 (the declared frame rate)
// and derives its per-frame step from it as 300/rate then rate/(300/rate), so
// it is also the place where a frame-rate-dependent animation cost would live.
// ---------------------------------------------------------------------------

enum GuestZone : uint32_t {
  kZoneRenderTask = 0,  // sub_82125378
  kZoneRenderOuter,     // sub_82124C40
  kZoneAuxTask,         // sub_82123508
  kZoneAuxNode,         // sub_82123470
  kZoneAnimUpdate,      // sub_820C7538
  kZoneAnimEntry,       // sub_820C8378
  kZoneAnimPart,        // sub_820C9550
  kZoneCount,
};

const char* const kZoneNames[kZoneCount] = {"render_82125378",  "render_82124C40",
                                            "auxtask_82123508", "auxnode_82123470",
                                            "anim_820C7538",    "animentry_820C8378",
                                            "animpart_820C9550"};

struct ZoneCounters {
  uint64_t calls[kZoneCount] = {};
  uint64_t ns[kZoneCount] = {};
};

ZoneCounters g_zones;

// Written only from the present thread (see WaitScope) and read by the summary,
// which runs on the present thread too. No synchronisation needed.
WaitCounters g_waits;
uint64_t g_present_ns = 0;
uint64_t g_frames = 0;

// Set by the overlay for as long as it is on screen.
std::atomic<bool> g_overlay_open{false};

bool ProfilingEnabled() {
  return g_overlay_open.load(std::memory_order_relaxed) || REXCVAR_GET(guest_profile);
}

#ifdef _WIN32
DWORD g_target_tid = 0;
#endif

bool OnTargetThread() {
#ifdef _WIN32
  return g_target_tid != 0 && GetCurrentThreadId() == g_target_tid;
#else
  return false;
#endif
}

#ifdef _WIN32
// Defined with the sampler below; declared here because WaitScope needs them.
int CaptureStackSelf(uint64_t* out, int max_frames);
void RecordWaitStack(const uint64_t* frames, int n, uint64_t ns);
#endif

// Times a guest blocking call, but only on the thread with a deadline. The two
// clock reads cost ~40 ns against a wait that is by definition at least a
// scheduler round trip, so the instrument does not perturb what it measures.
//
// It also captures the calling stack and charges the blocked time to it. The
// per-kind totals say *how much* the frame blocked; only the stack says *who*
// blocked, which is the whole question when the answer is a task somewhere in
// the scheduler's per-task dispatch (sub_82132A08 calls each task through its
// vtable, so every task's wait looks identical from above). Unwinding costs a
// few microseconds and waits are a handful per frame, so this is affordable
// where doing it per sample would not be.
class WaitScope {
 public:
  explicit WaitScope(WaitKind kind) : kind_(kind), active_(OnTargetThread() && ProfilingEnabled()) {
    if (active_) {
      start_ = std::chrono::steady_clock::now();
    }
  }

  ~WaitScope() {
    if (!active_) {
      return;
    }
    const auto end = std::chrono::steady_clock::now();
    const uint64_t ns =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count());
    ++g_waits.calls[kind_];
    g_waits.ns[kind_] += ns;
#ifdef _WIN32
    // Sub-microsecond waits are an uncontended handshake, not a stall. Skipping
    // them keeps a chatty-but-free primitive from crowding the table.
    if (ns >= 1000) {
      uint64_t frames[48];
      const int n = CaptureStackSelf(frames, 48);
      if (n > 0) {
        RecordWaitStack(frames, n, ns);
      }
    }
#endif
  }

  WaitScope(const WaitScope&) = delete;
  WaitScope& operator=(const WaitScope&) = delete;

 private:
  WaitKind kind_;
  bool active_;
  std::chrono::steady_clock::time_point start_;
};

// Exact timing for one named guest function. Same thread rule as WaitScope:
// only the thread with a deadline counts. Nested zones (anim entry/part inside
// the anim update) each measure their own inclusive time, so the inner ones are
// a subset of the outer, not additive with it.
class ZoneScope {
 public:
  explicit ZoneScope(GuestZone zone)
      : zone_(zone), active_(OnTargetThread() && ProfilingEnabled()) {
    if (active_) {
      start_ = std::chrono::steady_clock::now();
    }
  }

  ~ZoneScope() {
    if (!active_) {
      return;
    }
    const auto end = std::chrono::steady_clock::now();
    ++g_zones.calls[zone_];
    g_zones.ns[zone_] +=
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count());
  }

  ZoneScope(const ZoneScope&) = delete;
  ZoneScope& operator=(const ZoneScope&) = delete;

 private:
  GuestZone zone_;
  bool active_;
  std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// The snapshot the overlay draws
//
// Rebuilt once a second by GuestProfilerReport on the present thread and read
// by the ImGui thread, so it is mutex guarded. Formatted eagerly into strings:
// symbolisation is expensive and the overlay redraws at display rate, so doing
// it per draw would cost far more than the profiler itself.
// ---------------------------------------------------------------------------

struct Row {
  std::string name;
  double percent = 0.0;
};

struct Snapshot {
  bool valid = false;
  double fps = 0.0;
  double present_ms = 0.0;
  double wait_calls[kWaitKindCount] = {};
  double wait_ms[kWaitKindCount] = {};
  double wait_ms_total = 0.0;
  double zone_calls[kZoneCount] = {};
  double zone_ms[kZoneCount] = {};
  uint64_t samples = 0;
  uint64_t bad_samples = 0;
  std::vector<Row> inclusive;
  std::vector<Row> leaf;
  // Ranked by blocked milliseconds per frame rather than by percent; `percent`
  // carries the ms so the table can reuse Row.
  std::vector<Row> blocked_by;
};

std::mutex g_snapshot_mutex;
Snapshot g_snapshot;

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

#ifdef _WIN32

constexpr int kMaxDepth = 48;

// Raw return addresses, one sample after another, each terminated by a 0. Kept
// raw and symbolised only by the summary: dbghelp takes a process-wide lock and
// is not reentrant, and the target thread is suspended while we walk it, so
// calling into dbghelp there can deadlock against a target that is itself
// inside dbghelp. Nothing but Rtl* unwinding runs under the suspend.
std::mutex g_samples_mutex;
std::vector<uint64_t> g_samples;
uint64_t g_sample_count = 0;
uint64_t g_failed_samples = 0;

// One second of headroom at the highest supported rate. If the summary stops
// running (the game is paused in a debugger, say) the buffer stops growing
// rather than eating the heap.
constexpr size_t kMaxSampleWords = 8000 * (kMaxDepth + 1);

HANDLE g_target_thread = nullptr;
std::atomic<bool> g_sampler_running{false};
std::thread g_sampler;

// Blocked stacks, in the same [frames..., 0] encoding as g_samples but with the
// blocked nanoseconds prepended to each record. Written from the present thread
// (inside WaitScope) and drained by the summary on that same thread, so no lock
// is needed; it is kept separate from g_samples precisely so it does not need
// the sampler's.
std::vector<uint64_t> g_wait_stacks;
constexpr size_t kMaxWaitStackWords = 64 * 1024;

// Unwinds the calling thread. RtlCaptureContext gives a context whose Rip is
// inside this function, which the shared walker then unwinds out of; the first
// frame or two are this profiler's own code and are harmless noise in a table
// that is read from the deepest guest frame anyway.
int CaptureStackSelf(uint64_t* out, int max_frames);

void RecordWaitStack(const uint64_t* frames, int n, uint64_t ns) {
  if (g_wait_stacks.size() + size_t(n) + 2 > kMaxWaitStackWords) {
    return;
  }
  g_wait_stacks.push_back(ns);
  g_wait_stacks.insert(g_wait_stacks.end(), frames, frames + n);
  g_wait_stacks.push_back(0);
}

// Unwinds a suspended thread with the same machinery the OS exception
// dispatcher uses. No symbols required: RtlLookupFunctionEntry reads the
// module's .pdata, which is present in every build including Release.
//
// A thread suspended inside a prologue or epilogue has a stack the unwind data
// does not yet describe, so a walk can end early or produce one bogus frame.
// That is acceptable for a statistical profiler and is why the summary prints
// `bad` next to the sample count: a large bad fraction means the ranking is not
// trustworthy.
int WalkFrom(CONTEXT& ctx, uint64_t* out, int max_frames) {
  int n = 0;
  while (n < max_frames && ctx.Rip) {
    out[n++] = ctx.Rip;

    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!entry) {
      // A leaf function: no unwind data because it never moves rsp, so by the
      // x64 convention its return address is sitting at [rsp]. Without this the
      // walk stops dead, and every sample taken inside a syscall stub collapses
      // to a single parentless frame - which is exactly what happened to the
      // kernel-wait samples, hiding ~11 ms/frame of caller behind an anonymous
      // `NtWaitForSingleObject`. Only tried once, at the top of the stack,
      // because deeper down a missing entry means the walk really has gone
      // wrong rather than that we are in a leaf.
      if (n != 1) {
        break;
      }
      const DWORD64 ret = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
      if (!ret || !RtlLookupFunctionEntry(ret, &image_base, nullptr)) {
        break;
      }
      ctx.Rip = ret;
      ctx.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    const DWORD64 prev_rsp = ctx.Rsp;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, entry, &ctx, &handler_data,
                     &establisher, nullptr);
    // The stack must strictly grow downward as we walk out; anything else means
    // the unwind data did not describe this frame and the walk has gone wild.
    if (ctx.Rsp <= prev_rsp) {
      break;
    }
  }
  return n;
}

int CaptureStack(HANDLE thread, uint64_t* out, int max_frames) {
  CONTEXT ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(thread, &ctx)) {
    return 0;
  }
  return WalkFrom(ctx, out, max_frames);
}

int CaptureStackSelf(uint64_t* out, int max_frames) {
  CONTEXT ctx;
  RtlCaptureContext(&ctx);
  return WalkFrom(ctx, out, max_frames);
}

void SampleOnce() {
  HANDLE thread = g_target_thread;
  if (!thread) {
    return;
  }

  uint64_t frames[kMaxDepth];
  if (SuspendThread(thread) == DWORD(-1)) {
    return;
  }
  const int n = CaptureStack(thread, frames, kMaxDepth);
  ResumeThread(thread);

  std::lock_guard<std::mutex> lock(g_samples_mutex);
  ++g_sample_count;
  if (n == 0) {
    ++g_failed_samples;
    return;
  }
  if (g_samples.size() + size_t(n) + 1 > kMaxSampleWords) {
    return;
  }
  g_samples.insert(g_samples.end(), frames, frames + n);
  g_samples.push_back(0);
}

void SamplerMain() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

  while (g_sampler_running.load(std::memory_order_relaxed)) {
    if (ProfilingEnabled()) {
      SampleOnce();
    }
    const int hz = std::clamp(REXCVAR_GET(guest_profile_hz), 50, 8000);
    const int64_t period_100ns = 10'000'000LL / hz;
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -period_100ns;
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        continue;
      }
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(period_100ns * 100));
  }

  if (timer) {
    CloseHandle(timer);
  }
}

// Symbolisation, done once per summary and cached forever. The recompiler emits
// `__imp__sub_82xxxxxx` per guest function, so a resolved host symbol usually
// *is* the guest function name; the prefix is stripped so a line can be pasted
// straight into IDA.
struct SymInfo {
  uint64_t func_start = 0;
  std::string name;
};

std::unordered_map<uint64_t, SymInfo> g_sym_cache;
bool g_sym_ready = false;

const SymInfo& Symbolise(uint64_t addr) {
  auto it = g_sym_cache.find(addr);
  if (it != g_sym_cache.end()) {
    return it->second;
  }

  if (!g_sym_ready) {
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    g_sym_ready = true;
  }

  SymInfo info;
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), addr, &displacement, symbol)) {
    info.func_start = symbol->Address;
    std::string name(symbol->Name);
    // `__imp__sub_82123456` is how the recompiler names a guest function; the
    // rest of this project (logs, IDA, the shader debugger) speaks bare
    // `sub_82123456`, so match that.
    constexpr char kPrefix[] = "__imp__";
    if (name.rfind(kPrefix, 0) == 0) {
      name.erase(0, sizeof(kPrefix) - 1);
    }
    info.name = std::move(name);
  } else {
    info.func_start = addr;
    info.name = "?";
  }
  return g_sym_cache.emplace(addr, std::move(info)).first->second;
}

struct Bucket {
  uint64_t inclusive = 0;
  uint64_t leaf = 0;
  const std::string* name = nullptr;
};

void RankSamples(const std::vector<uint64_t>& raw, uint64_t good, size_t top,
                 std::vector<Row>* inclusive, std::vector<Row>* leaf) {
  std::unordered_map<uint64_t, Bucket> buckets;
  std::unordered_set<uint64_t> seen;  // per sample, so recursion counts once

  size_t i = 0;
  while (i < raw.size()) {
    seen.clear();
    bool is_leaf = true;
    while (i < raw.size() && raw[i] != 0) {
      const SymInfo& sym = Symbolise(raw[i]);
      ++i;
      auto& bucket = buckets[sym.func_start];
      bucket.name = &sym.name;
      if (is_leaf) {
        ++bucket.leaf;
        is_leaf = false;
      }
      if (seen.insert(sym.func_start).second) {
        ++bucket.inclusive;
      }
    }
    ++i;  // step over the terminator
  }

  std::vector<const Bucket*> ranked;
  ranked.reserve(buckets.size());
  for (const auto& entry : buckets) {
    ranked.push_back(&entry.second);
  }
  const size_t keep = std::min(top, ranked.size());

  std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
                    [](const Bucket* a, const Bucket* b) { return a->inclusive > b->inclusive; });
  for (size_t n = 0; n < keep; ++n) {
    inclusive->push_back({ranked[n]->name ? *ranked[n]->name : "?",
                          100.0 * double(ranked[n]->inclusive) / double(good)});
  }

  std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
                    [](const Bucket* a, const Bucket* b) { return a->leaf > b->leaf; });
  for (size_t n = 0; n < keep && ranked[n]->leaf; ++n) {
    leaf->push_back(
        {ranked[n]->name ? *ranked[n]->name : "?", 100.0 * double(ranked[n]->leaf) / double(good)});
  }
}

// The same idea as RankSamples' inclusive half, but each record carries its own
// weight (blocked nanoseconds) instead of counting as one, and the records are
// [ns][frames...][0]. What comes out is "this function was on the stack for N
// milliseconds of blocking per frame", which is the number that identifies the
// caller responsible for a stall.
void RankWaitStacks(const std::vector<uint64_t>& raw, double frames, size_t top,
                    std::vector<Row>* out) {
  std::unordered_map<uint64_t, std::pair<uint64_t, const std::string*>> totals;
  std::unordered_set<uint64_t> seen;

  size_t i = 0;
  while (i < raw.size()) {
    const uint64_t ns = raw[i++];
    seen.clear();
    while (i < raw.size() && raw[i] != 0) {
      const SymInfo& sym = Symbolise(raw[i]);
      ++i;
      if (seen.insert(sym.func_start).second) {
        auto& entry = totals[sym.func_start];
        entry.first += ns;
        entry.second = &sym.name;
      }
    }
    ++i;  // step over the terminator
  }

  std::vector<std::pair<uint64_t, const std::string*>> ranked;
  ranked.reserve(totals.size());
  for (const auto& entry : totals) {
    ranked.push_back(entry.second);
  }
  const size_t keep = std::min(top, ranked.size());
  std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t n = 0; n < keep; ++n) {
    out->push_back({ranked[n].second ? *ranked[n].second : "?",
                    (double(ranked[n].first) / frames) / 1e6});
  }
}

#endif  // _WIN32

// Everything the overlay shows, in the log. Shared so that what you read on
// screen and what you paste out of the log are the same numbers, formatted the
// same way, and neither can drift from the other.
void LogSnapshot(const Snapshot& snap) {
  std::string waits;
  for (uint32_t k = 0; k < kWaitKindCount; ++k) {
    waits += fmt::format("{}={:.2f}/f,{:.3f}ms/f ", kWaitNames[k], snap.wait_calls[k],
                         snap.wait_ms[k]);
  }
  REXLOG_INFO("[guest-prof] fps={:.1f} present={:.2f}ms/f blocked={:.2f}ms/f samples={} bad={} {}",
              snap.fps, snap.present_ms, snap.wait_ms_total, snap.samples, snap.bad_samples, waits);
  std::string zones;
  for (uint32_t z = 0; z < kZoneCount; ++z) {
    zones += fmt::format("{}={:.1f}/f,{:.3f}ms/f ", kZoneNames[z], snap.zone_calls[z],
                         snap.zone_ms[z]);
  }
  REXLOG_INFO("[guest-prof] zones {}", zones);
  for (const Row& row : snap.blocked_by) {
    REXLOG_INFO("[guest-prof]   blocked {:7.3f}ms/f  {}", row.percent, row.name);
  }
  for (const Row& row : snap.inclusive) {
    REXLOG_INFO("[guest-prof]   incl {:5.1f}%  {}", row.percent, row.name);
  }
  for (const Row& row : snap.leaf) {
    REXLOG_INFO("[guest-prof]   leaf {:5.1f}%  {}", row.percent, row.name);
  }
}

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

class GuestProfilerOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit GuestProfilerOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

  ~GuestProfilerOverlay() override { g_overlay_open.store(false, std::memory_order_relaxed); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Rides F3 alongside the SDK's own debug overlay, so one key brings up both
    // halves of the same question: the SDK's panel says how fast the frame is,
    // this one says why. Taken from ImGui rather than GetAsyncKeyState so it
    // respects the overlay's own keyboard focus, and ignored while a text field
    // has the keyboard.
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false) && !io.WantTextInput) {
      open_ = !open_;
      g_overlay_open.store(open_, std::memory_order_relaxed);
    }
    if (!open_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (!ImGui::Begin("Guest Profiler##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    Snapshot snap;
    {
      std::lock_guard<std::mutex> lock(g_snapshot_mutex);
      snap = g_snapshot;
    }

    if (!snap.valid) {
      ImGui::TextUnformatted("Sampling... (the summary rolls once a second)");
      ImGui::End();
      return;
    }

    ImGui::Text("guest %.1f fps   present %.2f ms/frame", snap.fps, snap.present_ms);
    ImGui::Text("samples %llu (%llu bad)", static_cast<unsigned long long>(snap.samples),
                static_cast<unsigned long long>(snap.bad_samples));

    // Reading these numbers off the screen and retyping them is the slow part
    // of using this thing, so both ways out are one click. "Dump" writes the
    // snapshot already on screen; the checkbox keeps writing one a second,
    // which is what you want for a run you intend to diff afterwards.
    if (ImGui::Button("Dump to log")) {
      LogSnapshot(snap);
    }
    ImGui::SameLine();
    bool logging = REXCVAR_GET(guest_profile);
    if (ImGui::Checkbox("Log every second", &logging)) {
      rex::cvar::SetFlagByName("guest_profile", logging ? "true" : "false");
    }

    ImGui::Separator();
    // The headline number. A frame that misses 16.67 ms with this near zero is
    // out of CPU budget; a frame that misses it with several ms here is blocked
    // on something, and the inclusive table below names who is blocking.
    ImGui::Text("blocked %.2f ms/frame", snap.wait_ms_total);
    if (ImGui::BeginTable("waits", 3, ImGuiTableFlags_SizingStretchProp)) {
      for (uint32_t k = 0; k < kWaitKindCount; ++k) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kWaitNames[k]);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f calls/f", snap.wait_calls[k]);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f ms/f", snap.wait_ms[k]);
      }
      ImGui::EndTable();
    }

    ImGui::Separator();
    // Exact, not sampled. The calls/frame column is the one that answers "does
    // this scale with what is on screen": a count that tracks the enemy count
    // is a per-model cost, a flat one is not.
    ImGui::TextUnformatted("Guest zones (exact, nested)");
    if (ImGui::BeginTable("zones", 3, ImGuiTableFlags_SizingStretchProp)) {
      for (uint32_t z = 0; z < kZoneCount; ++z) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kZoneNames[z]);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f calls/f", snap.zone_calls[z]);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f ms/f", snap.zone_ms[z]);
      }
      ImGui::EndTable();
    }

    ImGui::Separator();
    // The one that names a culprit: blocked milliseconds per frame charged to
    // every function on the stack of the wait. Read it from the bottom, where
    // the entries stop being the scheduler and start being a particular task.
    ImGui::TextUnformatted("Blocked by (ms/frame on the stack of a wait)");
    DrawMsRows("blockedby", snap.blocked_by);

    ImGui::Separator();
    ImGui::TextUnformatted("Inclusive (which guest function owns the frame)");
    DrawRows("incl", snap.inclusive);

    ImGui::Separator();
    ImGui::TextUnformatted("Leaf (where the cycles land)");
    DrawRows("leaf", snap.leaf);

    ImGui::End();
  }

 private:
  static void DrawMsRows(const char* id, const std::vector<Row>& rows) {
    if (rows.empty()) {
      ImGui::TextUnformatted("  (nothing blocked)");
      return;
    }
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
      return;
    }
    for (const Row& row : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%7.3f ms", row.percent);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(row.name.c_str());
    }
    ImGui::EndTable();
  }

  static void DrawRows(const char* id, const std::vector<Row>& rows) {
    if (rows.empty()) {
      ImGui::TextUnformatted("  (no samples)");
      return;
    }
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
      return;
    }
    for (const Row& row : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%5.1f%%", row.percent);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(row.name.c_str());
    }
    ImGui::EndTable();
  }

  bool open_ = false;
};

}  // namespace

void GuestProfilerNotePresent(uint64_t present_ns) { g_present_ns += present_ns; }

void GuestProfilerFrameBoundary() {
  ++g_frames;
#ifdef _WIN32
  if (!g_target_tid) {
    g_target_tid = GetCurrentThreadId();
    // A pseudo handle is only valid on the thread that asked for it, so keep a
    // real one for the sampler.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                    0);
  }
  // Started on demand and then left running: it idles on its timer and samples
  // nothing while profiling is off, and stopping/restarting it would race the
  // overlay's open flag for no gain.
  if (ProfilingEnabled() && !g_sampler_running.load(std::memory_order_relaxed) &&
      g_target_thread) {
    g_sampler_running.store(true, std::memory_order_relaxed);
    g_sampler = std::thread(SamplerMain);
  }
#endif
}

void GuestProfilerReport() {
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start{};
  const auto now = clock::now();

  if (!ProfilingEnabled()) {
    // Keep the window from spanning the whole time the profiler was off, which
    // would divide a second's worth of counters by minutes of wall time.
    window_start = now;
    g_frames = 0;
    g_present_ns = 0;
    g_waits = WaitCounters{};
    return;
  }
  if (window_start == clock::time_point{}) {
    window_start = now;
    return;
  }
  if (now - window_start < std::chrono::seconds(1)) {
    return;
  }

  const double secs = std::chrono::duration<double>(now - window_start).count();
  const double frames = double(g_frames ? g_frames : 1);

  Snapshot snap;
  snap.valid = true;
  snap.fps = double(g_frames) / secs;
  snap.present_ms = (double(g_present_ns) / frames) / 1e6;
  // Per frame, because that is the budget being missed. A wait that costs 4 ms
  // per frame is fatal at 60 and invisible at 30, and only the per frame form
  // makes that comparison directly.
  for (uint32_t k = 0; k < kWaitKindCount; ++k) {
    snap.wait_calls[k] = double(g_waits.calls[k]) / frames;
    snap.wait_ms[k] = (double(g_waits.ns[k]) / frames) / 1e6;
    snap.wait_ms_total += snap.wait_ms[k];
  }
  for (uint32_t z = 0; z < kZoneCount; ++z) {
    snap.zone_calls[z] = double(g_zones.calls[z]) / frames;
    snap.zone_ms[z] = (double(g_zones.ns[z]) / frames) / 1e6;
  }

#ifdef _WIN32
  std::vector<uint64_t> raw;
  {
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    snap.samples = g_sample_count;
    snap.bad_samples = g_failed_samples;
    raw.swap(g_samples);
    g_sample_count = 0;
    g_failed_samples = 0;
  }
  const size_t top = size_t(std::clamp(REXCVAR_GET(guest_profile_top), 1, 64));
  const uint64_t good = snap.samples > snap.bad_samples ? snap.samples - snap.bad_samples : 0;
  if (good) {
    RankSamples(raw, good, top, &snap.inclusive, &snap.leaf);
  }
  if (!g_wait_stacks.empty()) {
    RankWaitStacks(g_wait_stacks, frames, top, &snap.blocked_by);
    g_wait_stacks.clear();
  }
#endif

  if (REXCVAR_GET(guest_profile)) {
    LogSnapshot(snap);
  }

  {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    g_snapshot = std::move(snap);
  }

  window_start = now;
  g_frames = 0;
  g_present_ns = 0;
  g_waits = WaitCounters{};
  g_zones = ZoneCounters{};
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateGuestProfilerOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<GuestProfilerOverlay>(drawer);
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Hooks
//
// One per blocking primitive, plus the guest D3D command buffer refill. Each is
// a pass-through: capture nothing, change nothing, just time the original.
// ---------------------------------------------------------------------------

#define ETERNALSONATA_WAIT_HOOK(fn, kind)          \
  REX_EXTERN(__imp__##fn);                         \
  REX_HOOK_RAW(fn) {                               \
    eternalsonata::WaitScope scope(kind);          \
    __imp__##fn(ctx, base);                        \
  }

ETERNALSONATA_WAIT_HOOK(sub_822546E0, eternalsonata::kWaitDelay)
ETERNALSONATA_WAIT_HOOK(sub_822548A0, eternalsonata::kWaitNt548A0)
ETERNALSONATA_WAIT_HOOK(sub_82252668, eternalsonata::kWaitNt52668)
ETERNALSONATA_WAIT_HOOK(sub_822529E8, eternalsonata::kWaitNt529E8)
ETERNALSONATA_WAIT_HOOK(sub_822722B0, eternalsonata::kWaitKe722B0)
ETERNALSONATA_WAIT_HOOK(sub_822A7D98, eternalsonata::kWaitKeA7D98)
ETERNALSONATA_WAIT_HOOK(sub_822A7E60, eternalsonata::kWaitKeA7E60)
ETERNALSONATA_WAIT_HOOK(sub_822A7F10, eternalsonata::kWaitKeMultiple)

#undef ETERNALSONATA_WAIT_HOOK

#define ETERNALSONATA_ZONE_HOOK(fn, zone)          \
  REX_EXTERN(__imp__##fn);                         \
  REX_HOOK_RAW(fn) {                               \
    eternalsonata::ZoneScope scope(zone);          \
    __imp__##fn(ctx, base);                        \
  }

ETERNALSONATA_ZONE_HOOK(sub_82125378, eternalsonata::kZoneRenderTask)
ETERNALSONATA_ZONE_HOOK(sub_82124C40, eternalsonata::kZoneRenderOuter)
ETERNALSONATA_ZONE_HOOK(sub_82123508, eternalsonata::kZoneAuxTask)
ETERNALSONATA_ZONE_HOOK(sub_82123470, eternalsonata::kZoneAuxNode)
ETERNALSONATA_ZONE_HOOK(sub_820C7538, eternalsonata::kZoneAnimUpdate)
ETERNALSONATA_ZONE_HOOK(sub_820C8378, eternalsonata::kZoneAnimEntry)
ETERNALSONATA_ZONE_HOOK(sub_820C9550, eternalsonata::kZoneAnimPart)

#undef ETERNALSONATA_ZONE_HOOK

// There is deliberately no hook on the guest D3D layer's command-segment refill
// (sub_82264980). It looked like a cheap per-draw proxy, but the recompiler does
// not emit it as a callable function, and it is the one candidate here that
// would have been a rendering cost anyway. The wait counters and the sampler
// are both backend agnostic, which is the property this whole file is for.
