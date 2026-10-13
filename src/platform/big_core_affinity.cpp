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

// The cores within 10% of the fastest one, so a prime core and its slightly
// slower siblings both count as big.
bool BigCoreSet(cpu_set_t* set) {
  const int count = int(sysconf(_SC_NPROCESSORS_CONF));
  uint64_t fastest = 0;
  for (int cpu = 0; cpu < count; ++cpu)
    fastest = std::max(fastest, ReadMaxFreq(cpu));
  if (fastest == 0)
    return false;
  CPU_ZERO(set);
  int big = 0;
  for (int cpu = 0; cpu < count; ++cpu) {
    if (ReadMaxFreq(cpu) * 10 >= fastest * 9) {
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
  return name.rfind("XThread", 0) == 0 || name.rfind("Main XThread", 0) == 0;
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
