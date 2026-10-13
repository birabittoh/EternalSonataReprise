#include "guest_profiler.h"

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#if defined(__linux__) && defined(__aarch64__)
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cxxabi.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

// Where a stack sampler exists. Android has no SuspendThread, so there it is a
// signal to the target thread plus a walk of the AAPCS64 frame records.
#if defined(_WIN32) || (defined(__linux__) && defined(__aarch64__))
#define ES_GUEST_SAMPLER 1
#endif

#include <imgui.h>
#include <rex/cvar.h>

#include "eternalsonata_hooks_internal.h"
#include "ui_text.h"

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

const char* const kZoneNames[kZoneCount] = {
    "render_82125378",  "render_82124C40", "auxtask_82123508", "auxnode_82123470",
    "anim_820C7538",    "animentry_820C8378", "animpart_820C9550", "constflush_8212CDE0",
    "d3d_drawidx",      "d3d_drawv",       "d3d_beginv",       "d3d_endv",
    "d3d_vsconstf",     "d3d_psconstf",    "d3d_settex"};

struct ZoneCounters {
  uint64_t calls[kZoneCount] = {};
  uint64_t ns[kZoneCount] = {};
};

ZoneCounters g_zones;

// Written only from the present thread (see WaitScope) and read by the summary,
// which runs on the present thread too. No synchronisation needed.
WaitCounters g_waits;

// Blocked time per (kind, guest return address). The wrappers are shared by
// every wait in the game, so the caller is what names the event; it is also the
// only attribution there is where the stack sampler is not.
struct CallerCounters {
  uint64_t calls = 0;
  uint64_t ns = 0;
};
std::unordered_map<uint64_t, CallerCounters> g_wait_callers;

// The last ReleaseSemaphore on each handle, from any thread, so a wait that
// returns can be charged to whoever let it go.
struct Releaser {
  uint32_t caller = 0;
  std::string thread;
};
std::mutex g_release_mutex;
std::unordered_map<uint32_t, Releaser> g_last_release;
std::unordered_map<std::string, CallerCounters> g_wait_releasers;

const std::string& CurrentThreadLabel() {
  thread_local std::string label;
  if (label.empty()) {
#ifdef _WIN32
    label = fmt::format("tid {}", GetCurrentThreadId());
#else
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    label = name;
#endif
  }
  return label;
}
uint64_t g_present_ns = 0;
uint64_t g_frames = 0;

// Set by the overlay for as long as it is on screen.
std::atomic<bool> g_overlay_open{false};

bool ProfilingEnabled() {
  return g_overlay_open.load(std::memory_order_relaxed) || REXCVAR_GET(guest_profile);
}

#ifdef _WIN32
DWORD g_target_tid = 0;
#else
thread_local bool g_is_target_thread = false;
std::atomic<bool> g_target_claimed{false};
#endif

bool OnTargetThread() {
#ifdef _WIN32
  return g_target_tid != 0 && GetCurrentThreadId() == g_target_tid;
#else
  return g_is_target_thread;
#endif
}

#ifdef ES_GUEST_SAMPLER
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
  WaitScope(WaitKind kind, uint32_t caller, uint32_t handle)
      : kind_(kind), caller_(caller), handle_(handle),
        active_(OnTargetThread() && ProfilingEnabled()) {
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
    CallerCounters& caller = g_wait_callers[(uint64_t(kind_) << 32) | caller_];
    ++caller.calls;
    caller.ns += ns;
    if (kind_ == kWaitNt548A0) {
      std::string releaser = "unknown";
      {
        std::lock_guard<std::mutex> lock(g_release_mutex);
        auto found = g_last_release.find(handle_);
        if (found != g_last_release.end())
          releaser = fmt::format("{} from 0x{:08X}", found->second.thread, found->second.caller);
      }
      CallerCounters& by = g_wait_releasers[releaser];
      ++by.calls;
      by.ns += ns;
    }
#ifdef ES_GUEST_SAMPLER
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
  uint32_t caller_;
  uint32_t handle_;
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

}  // namespace

uint64_t GuestZoneStart() {
  if (!(OnTargetThread() && ProfilingEnabled()))
    return 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  return ns ? ns : 1;
}

void GuestZoneEnd(GuestZone zone, uint64_t start) {
  if (!start)
    return;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  ++g_zones.calls[zone];
  g_zones.ns[zone] += ns - start;
}

namespace {

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
  std::vector<Row> leaf_callers;
  std::vector<Row> leaf_offsets;
  // Ranked by blocked milliseconds per frame rather than by percent; `percent`
  // carries the ms so the table can reuse Row.
  std::vector<Row> blocked_by;
};

std::mutex g_snapshot_mutex;
Snapshot g_snapshot;

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

#ifdef ES_GUEST_SAMPLER

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

#ifdef _WIN32
HANDLE g_target_thread = nullptr;
#endif
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

// Symbolisation, done once per summary and cached forever. The recompiler emits
// `__imp__sub_82xxxxxx` per guest function, so a resolved host symbol usually
// *is* the guest function name; the prefix is stripped so a line can be pasted
// straight into IDA.
struct SymInfo {
  uint64_t func_start = 0;
  std::string name;
  // For code dladdr cannot name: "module+0xoffset", to symbolise offline
  // against the unstripped library.
  std::string offset;
};

std::unordered_map<uint64_t, SymInfo> g_sym_cache;

// `__imp__sub_82123456` is how the recompiler names a guest function; the rest
// of this project (logs, IDA, the shader debugger) speaks bare `sub_82123456`.
void StripImpPrefix(std::string& name) {
  constexpr char kPrefix[] = "__imp__";
  if (name.rfind(kPrefix, 0) == 0) {
    name.erase(0, sizeof(kPrefix) - 1);
  }
}

#ifdef _WIN32

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

// A bogus frame can send RtlVirtualUnwind or the leaf read to an unmapped
// address, which would take the whole process down from the sampler thread.
int WalkFromGuarded(CONTEXT& ctx, uint64_t* out, int max_frames) {
  __try {
    return WalkFrom(ctx, out, max_frames);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    return 0;
  }
}

int CaptureStack(HANDLE thread, uint64_t* out, int max_frames) {
  CONTEXT ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(thread, &ctx)) {
    return 0;
  }
  return WalkFromGuarded(ctx, out, max_frames);
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
    info.name = symbol->Name;
    StripImpPrefix(info.name);
  } else {
    info.func_start = addr;
    info.name = "?";
  }
  return g_sym_cache.emplace(addr, std::move(info)).first->second;
}

#else  // Linux, aarch64

// Written by GuestProfilerFrameBoundary before the sampler thread starts, which
// orders them before every signal.
pid_t g_target_tid = 0;
uintptr_t g_stack_lo = 0;
uintptr_t g_stack_hi = 0;

// The sampler asks with a sequence number and the handler answers with it, so
// the sampler only reads frames the handler finished writing for its request.
std::atomic<uint32_t> g_signal_request{0};
std::atomic<uint32_t> g_signal_done{0};
uint64_t g_signal_frames[kMaxDepth];
int g_signal_count = 0;

// Return addresses carry no tag or PAC bits in a userspace pointer this wide.
constexpr uint64_t kAddressMask = 0x0000FFFFFFFFFFFFull;

// Follows the x29 chain of {previous x29, x30} records. Every record has to sit
// inside the target's own stack and above the last one, so a stale x29 ends the
// walk instead of faulting inside the signal handler.
int WalkFrameRecords(uintptr_t fp, uint64_t* out, int n, int max_frames) {
  while (n < max_frames) {
    if (fp < g_stack_lo || fp + 16 > g_stack_hi || (fp & 7) != 0) {
      break;
    }
    const auto* record = reinterpret_cast<const uintptr_t*>(fp);
    const uint64_t ret = uint64_t(record[1]) & kAddressMask;
    if (!ret) {
      break;
    }
    out[n++] = ret;
    if (record[0] <= fp) {
      break;
    }
    fp = record[0];
  }
  return n;
}

int CaptureStackSelf(uint64_t* out, int max_frames) {
  return WalkFrameRecords(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)), out, 0,
                          max_frames);
}

void OnProfileSignal(int, siginfo_t*, void* context) {
  const int saved_errno = errno;
  const uint32_t request = g_signal_request.load(std::memory_order_acquire);
  const mcontext_t& mc = static_cast<const ucontext_t*>(context)->uc_mcontext;
  int n = 0;
  g_signal_frames[n++] = mc.pc & kAddressMask;
  // A leaf never stores x30 in a record, so this is the only place its caller
  // appears. Elsewhere it repeats a function the walk finds anyway, and each
  // sample counts a function once.
  if (mc.regs[30]) {
    g_signal_frames[n++] = mc.regs[30] & kAddressMask;
  }
  g_signal_count = WalkFrameRecords(uintptr_t(mc.regs[29]), g_signal_frames, n, kMaxDepth);
  g_signal_done.store(request, std::memory_order_release);
  errno = saved_errno;
}

void SampleOnce() {
  const pid_t tid = g_target_tid;
  if (!tid) {
    return;
  }
  const uint32_t request = g_signal_request.load(std::memory_order_relaxed) + 1;
  g_signal_request.store(request, std::memory_order_release);
  if (syscall(SYS_tgkill, getpid(), tid, SIGPROF) != 0) {
    return;
  }

  // The handler runs as soon as the kernel next schedules the target, which is
  // immediate unless the big cores are all busy.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
  while (g_signal_done.load(std::memory_order_acquire) != request) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::lock_guard<std::mutex> lock(g_samples_mutex);
      ++g_sample_count;
      ++g_failed_samples;
      return;
    }
    sched_yield();
  }

  uint64_t frames[kMaxDepth];
  const int n = g_signal_count;
  std::memcpy(frames, g_signal_frames, sizeof(uint64_t) * size_t(n));

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
  timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  while (g_sampler_running.load(std::memory_order_relaxed)) {
    if (ProfilingEnabled()) {
      SampleOnce();
    }
    const int hz = std::clamp(REXCVAR_GET(guest_profile_hz), 50, 8000);
    next.tv_nsec += 1'000'000'000L / hz;
    while (next.tv_nsec >= 1'000'000'000L) {
      next.tv_nsec -= 1'000'000'000L;
      ++next.tv_sec;
    }
    // Absolute, so the rate holds however long the sample took.
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr) == EINTR) {
    }
  }
}

void StartLinuxSampler() {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* stack = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &stack, &size) == 0) {
      g_stack_lo = reinterpret_cast<uintptr_t>(stack);
      g_stack_hi = g_stack_lo + size;
    }
    pthread_attr_destroy(&attr);
  }
  if (!g_stack_hi) {
    return;
  }

  struct sigaction action = {};
  action.sa_sigaction = OnProfileSignal;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPROF, &action, nullptr) != 0) {
    return;
  }
  g_target_tid = pid_t(syscall(SYS_gettid));
  g_sampler_running.store(true, std::memory_order_relaxed);
  g_sampler = std::thread(SamplerMain);
}

// Guest functions are exported from the game library, so dladdr names them
// without the symbol files. Unexported code is grouped by module.
const SymInfo& Symbolise(uint64_t addr) {
  auto it = g_sym_cache.find(addr);
  if (it != g_sym_cache.end()) {
    return it->second;
  }

  SymInfo info;
  Dl_info dl = {};
  if (dladdr(reinterpret_cast<void*>(addr), &dl) != 0 && dl.dli_sname) {
    info.func_start = reinterpret_cast<uint64_t>(dl.dli_saddr);
    int status = 0;
    char* demangled = abi::__cxa_demangle(dl.dli_sname, nullptr, nullptr, &status);
    info.name = status == 0 && demangled ? demangled : dl.dli_sname;
    std::free(demangled);
    StripImpPrefix(info.name);
  } else if (dl.dli_fname) {
    info.func_start = reinterpret_cast<uint64_t>(dl.dli_fbase);
    const char* slash = std::strrchr(dl.dli_fname, '/');
    info.name = fmt::format("{} (unexported)", slash ? slash + 1 : dl.dli_fname);
    info.offset = fmt::format("{}+0x{:x}", slash ? slash + 1 : dl.dli_fname,
                              addr - reinterpret_cast<uint64_t>(dl.dli_fbase));
  } else {
    info.name = "?";
  }
  return g_sym_cache.emplace(addr, std::move(info)).first->second;
}

#endif  // _WIN32

struct Bucket {
  uint64_t inclusive = 0;
  uint64_t leaf = 0;
  const std::string* name = nullptr;
};

// Keyed by name rather than address: a hooked function is two host symbols,
// the hook `sub_X` and the original `__imp__sub_X`, and both strip to one name.
// Names live in g_sym_cache, whose nodes never move, so views into them hold.
void RankSamples(const std::vector<uint64_t>& raw, uint64_t good, size_t top,
                 std::vector<Row>* inclusive, std::vector<Row>* leaf,
                 std::vector<Row>* leaf_callers, std::vector<Row>* leaf_offsets) {
  std::unordered_map<std::string_view, Bucket> buckets;
  std::unordered_set<std::string_view> seen;  // per sample, so recursion counts once
  // A leaf alone says `syscall` or `clock_gettime`; the next distinct names
  // above it say who is paying for it.
  constexpr int kChainNames = 4;
  std::unordered_map<std::string, uint64_t> pairs;
  std::unordered_map<std::string_view, uint64_t> offsets;

  size_t i = 0;
  while (i < raw.size()) {
    seen.clear();
    std::string chain;
    const std::string* last_name = nullptr;
    int chain_names = 0;
    while (i < raw.size() && raw[i] != 0) {
      const SymInfo& sym = Symbolise(raw[i]);
      ++i;
      auto& bucket = buckets[sym.name];
      bucket.name = &sym.name;
      if (!last_name) {
        ++bucket.leaf;
        if (!sym.offset.empty()) {
          ++offsets[sym.offset];
        }
      }
      if (chain_names < kChainNames && (!last_name || sym.name != *last_name)) {
        chain += last_name ? " <- " + sym.name : sym.name;
        last_name = &sym.name;
        ++chain_names;
      }
      if (seen.insert(sym.name).second) {
        ++bucket.inclusive;
      }
    }
    ++i;  // step over the terminator
    if (chain_names > 1) {
      ++pairs[chain];
    }
  }

  std::vector<std::pair<std::string_view, uint64_t>> ranked_offsets(offsets.begin(),
                                                                    offsets.end());
  const size_t keep_offsets = std::min(top, ranked_offsets.size());
  std::partial_sort(ranked_offsets.begin(), ranked_offsets.begin() + keep_offsets,
                    ranked_offsets.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });
  for (size_t n = 0; n < keep_offsets; ++n) {
    leaf_offsets->push_back({std::string(ranked_offsets[n].first),
                             100.0 * double(ranked_offsets[n].second) / double(good)});
  }

  std::vector<std::pair<const std::string*, uint64_t>> ranked_pairs;
  ranked_pairs.reserve(pairs.size());
  for (const auto& entry : pairs) {
    ranked_pairs.emplace_back(&entry.first, entry.second);
  }
  const size_t keep_pairs = std::min(top, ranked_pairs.size());
  std::partial_sort(ranked_pairs.begin(), ranked_pairs.begin() + keep_pairs, ranked_pairs.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });
  for (size_t n = 0; n < keep_pairs; ++n) {
    leaf_callers->push_back({*ranked_pairs[n].first, 100.0 * double(ranked_pairs[n].second) /
                                                         double(good)});
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
  std::unordered_map<std::string_view, std::pair<uint64_t, const std::string*>> totals;
  std::unordered_set<std::string_view> seen;

  size_t i = 0;
  while (i < raw.size()) {
    const uint64_t ns = raw[i++];
    seen.clear();
    while (i < raw.size() && raw[i] != 0) {
      const SymInfo& sym = Symbolise(raw[i]);
      ++i;
      if (seen.insert(sym.name).second) {
        auto& entry = totals[sym.name];
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

#endif  // ES_GUEST_SAMPLER

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
  for (const Row& row : snap.leaf_callers) {
    REXLOG_INFO("[guest-prof]   from {:5.1f}%  {}", row.percent, row.name);
  }
  for (const Row& row : snap.leaf_offsets) {
    REXLOG_INFO("[guest-prof]   pc {:5.1f}%  {}", row.percent, row.name);
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
    //
    // guest_profile is debug_only, i.e. hidden from the normal F4 cvar list, so
    // requiring it to already be true is what keeps this heavy (thread suspend
    // and stack walk per sample) window from popping up for a player who just
    // wanted the FPS counter. Someone who wants it sets --guest_profile=true
    // (or the config equivalent) before launch, same as any other debug_only
    // cvar.
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false) && !io.WantTextInput &&
        (open_ || REXCVAR_GET(guest_profile))) {
      open_ = !open_;
      g_overlay_open.store(open_, std::memory_order_relaxed);
    }
    if (!open_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (!ImGui::Begin((std::string(Tr("profiler", "title")) + "##rex").c_str(), nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    Snapshot snap;
    {
      std::lock_guard<std::mutex> lock(g_snapshot_mutex);
      snap = g_snapshot;
    }

    if (!snap.valid) {
      ImGui::TextUnformatted(Tr("profiler", "sampling"));
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
    if (ImGui::Button(Tr("profiler", "dump"))) {
      LogSnapshot(snap);
    }
    ImGui::SameLine();
    bool logging = REXCVAR_GET(guest_profile);
    if (ImGui::Checkbox(Tr("profiler", "log_every_second"), &logging)) {
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
    ImGui::TextUnformatted(Tr("profiler", "zones"));
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
    ImGui::TextUnformatted(Tr("profiler", "blocked_by"));
    DrawMsRows("blockedby", snap.blocked_by);

    ImGui::Separator();
    ImGui::TextUnformatted(Tr("profiler", "inclusive"));
    DrawRows("incl", snap.inclusive);

    ImGui::Separator();
    ImGui::TextUnformatted(Tr("profiler", "leaf"));
    DrawRows("leaf", snap.leaf);

    ImGui::End();
  }

 private:
  static void DrawMsRows(const char* id, const std::vector<Row>& rows) {
    if (rows.empty()) {
      ImGui::TextUnformatted(Tr("profiler", "nothing_blocked"));
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
      ImGui::TextUnformatted(Tr("profiler", "no_samples"));
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
#else
  if (!g_target_claimed.exchange(true, std::memory_order_relaxed))
    g_is_target_thread = true;
#ifdef ES_GUEST_SAMPLER
  if (g_is_target_thread && ProfilingEnabled() &&
      !g_sampler_running.load(std::memory_order_relaxed)) {
    StartLinuxSampler();
  }
#endif
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
    g_wait_callers.clear();
    g_wait_releasers.clear();
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

#ifdef ES_GUEST_SAMPLER
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
    RankSamples(raw, good, top, &snap.inclusive, &snap.leaf, &snap.leaf_callers,
                &snap.leaf_offsets);
  }
  if (!g_wait_stacks.empty()) {
    RankWaitStacks(g_wait_stacks, frames, top, &snap.blocked_by);
    g_wait_stacks.clear();
  }
#endif

  if (REXCVAR_GET(guest_profile)) {
    LogSnapshot(snap);
    std::vector<std::pair<uint64_t, CallerCounters>> callers(g_wait_callers.begin(),
                                                             g_wait_callers.end());
    std::sort(callers.begin(), callers.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    for (size_t i = 0; i < callers.size() && i < 6; ++i) {
      REXLOG_INFO("[guest-prof]   waiter {} from 0x{:08X}: {:.2f}/f {:.3f}ms/f",
                  kWaitNames[callers[i].first >> 32], uint32_t(callers[i].first),
                  double(callers[i].second.calls) / frames,
                  double(callers[i].second.ns) / frames / 1e6);
    }
    std::vector<std::pair<std::string, CallerCounters>> releasers(g_wait_releasers.begin(),
                                                                  g_wait_releasers.end());
    std::sort(releasers.begin(), releasers.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    for (size_t i = 0; i < releasers.size() && i < 4; ++i) {
      REXLOG_INFO("[guest-prof]   released by {}: {:.2f}/f {:.3f}ms/f", releasers[i].first,
                  double(releasers[i].second.calls) / frames,
                  double(releasers[i].second.ns) / frames / 1e6);
    }
  }
  g_wait_callers.clear();
  g_wait_releasers.clear();

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
    eternalsonata::WaitScope scope(kind, uint32_t(ctx.lr), ctx.r3.u32); \
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

// ReleaseSemaphore, recorded so the waits above can name who released them.
REX_EXTERN(__imp__sub_822535B0);
REX_HOOK_RAW(sub_822535B0) {
  if (eternalsonata::ProfilingEnabled()) {
    std::lock_guard<std::mutex> lock(eternalsonata::g_release_mutex);
    eternalsonata::g_last_release[ctx.r3.u32] = {uint32_t(ctx.lr),
                                                 eternalsonata::CurrentThreadLabel()};
  }
  __imp__sub_822535B0(ctx, base);
}

#define ETERNALSONATA_ZONE_HOOK(fn, zone)          \
  REX_EXTERN(__imp__##fn);                         \
  REX_HOOK_RAW(fn) {                               \
    eternalsonata::ZoneScope scope(zone);          \
    __imp__##fn(ctx, base);                        \
  }

ETERNALSONATA_ZONE_HOOK(sub_82125378, eternalsonata::kZoneRenderTask)
ETERNALSONATA_ZONE_HOOK(sub_82123508, eternalsonata::kZoneAuxTask)
ETERNALSONATA_ZONE_HOOK(sub_82123470, eternalsonata::kZoneAuxNode)

ETERNALSONATA_ZONE_HOOK(sub_82124C40, eternalsonata::kZoneRenderOuter)
ETERNALSONATA_ZONE_HOOK(sub_820C7538, eternalsonata::kZoneAnimUpdate)

// These two also carry the wall-clock frame stepping's argument fixups, since
// a guest function can only be hooked once; see eternalsonata_hooks_internal.h.
REX_EXTERN(__imp__sub_820C8378);
REX_HOOK_RAW(sub_820C8378) {
  eternalsonata::ZoneScope scope(eternalsonata::kZoneAnimEntry);
  eternalsonata_hooks::AnimEntryFixup(ctx, base);
  __imp__sub_820C8378(ctx, base);
}

REX_EXTERN(__imp__sub_820C9550);
REX_HOOK_RAW(sub_820C9550) {
  eternalsonata::ZoneScope scope(eternalsonata::kZoneAnimPart);
  eternalsonata_hooks::AnimPartFixup(ctx, base);
  __imp__sub_820C9550(ctx, base);
}
ETERNALSONATA_ZONE_HOOK(sub_8212CDE0, eternalsonata::kZoneConstFlush)

#undef ETERNALSONATA_ZONE_HOOK

// There is deliberately no hook on the guest D3D layer's command-segment refill
// (sub_82264980). It looked like a cheap per-draw proxy, but the recompiler does
// not emit it as a callable function, and it is the one candidate here that
// would have been a rendering cost anyway. The wait counters and the sampler
// are both backend agnostic, which is the property this whole file is for.
