// eternalsonata - ReXGlue Recompiled Project
//
// See big_core_affinity.h.

#include "big_core_affinity.h"

#if defined(__ANDROID__)

#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>

#include <rex/logging.h>

namespace eternalsonata {
namespace {

uint64_t ReadMaxFreq(int cpu) {
  std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                   "/cpufreq/cpuinfo_max_freq");
  uint64_t khz = 0;
  in >> khz;
  return khz;
}

// Every core faster than the slowest cluster. A prime core can clock well
// ahead of its big siblings (Snapdragon 865: 2.84 against 2.42 GHz), so the
// line is drawn just above the little cores.
bool BigCoreSet(cpu_set_t* set) {
  const int count = int(sysconf(_SC_NPROCESSORS_CONF));
  uint64_t slowest = UINT64_MAX;
  for (int cpu = 0; cpu < count; ++cpu) {
    // An offline core has no cpufreq node and reads as 0.
    if (const uint64_t khz = ReadMaxFreq(cpu))
      slowest = std::min(slowest, khz);
  }
  if (slowest == UINT64_MAX)
    return false;
  CPU_ZERO(set);
  int big = 0;
  for (int cpu = 0; cpu < count; ++cpu) {
    if (ReadMaxFreq(cpu) > slowest) {
      CPU_SET(cpu, set);
      ++big;
    }
  }
  // Nothing to gain on a phone whose cores are all alike.
  return big > 0 && big < count;
}

bool IsGuestThread(pid_t tid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(tid));
  std::ifstream in(path);
  std::string name;
  std::getline(in, name);
  // The SDK's audio threads run the guest mixer callback and XMA decode, and
  // on a little core they can fall behind the output device.
  for (const char* prefix : {"XThread", "Main XThread", "Audio Worker", "XMA Decoder"}) {
    if (name.rfind(prefix, 0) == 0)
      return true;
  }
  return false;
}

}  // namespace

void PinGuestThreadsToBigCores() {
  using clock = std::chrono::steady_clock;
  static clock::time_point next_scan{};
  static bool usable = true;
  static cpu_set_t big;
  static std::unordered_set<pid_t> pinned;

  if (!usable || clock::now() < next_scan)
    return;
  next_scan = clock::now() + std::chrono::seconds(2);

  if (pinned.empty() && !BigCoreSet(&big)) {
    usable = false;
    return;
  }

  // The render thread itself has no XThread name, so it is pinned by tid.
  const pid_t self = pid_t(syscall(SYS_gettid));
  size_t added = 0;
  if (pinned.insert(self).second && sched_setaffinity(self, sizeof(big), &big) == 0)
    ++added;

  DIR* dir = opendir("/proc/self/task");
  if (dir == nullptr)
    return;
  while (dirent* entry = readdir(dir)) {
    const pid_t tid = pid_t(atoi(entry->d_name));
    if (tid <= 0 || pinned.count(tid) || !IsGuestThread(tid))
      continue;
    pinned.insert(tid);
    if (sched_setaffinity(tid, sizeof(big), &big) == 0)
      ++added;
  }
  closedir(dir);
  if (added)
    REXLOG_INFO("affinity: {} more guest thread(s) kept on the big cores", added);
}

void PinThisThreadToBigCores() {
  cpu_set_t big;
  if (BigCoreSet(&big))
    sched_setaffinity(0, sizeof(big), &big);
}

}  // namespace eternalsonata

#else

namespace eternalsonata {
void PinGuestThreadsToBigCores() {}
void PinThisThreadToBigCores() {}
}  // namespace eternalsonata

#endif
