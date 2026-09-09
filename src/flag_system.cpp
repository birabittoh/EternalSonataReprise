// eternalsonata - The game's flag system: the script progress bits, and the
// mod-facing API over them.
//
// Everything here was derived from the retail xex and from save files;
// docs/flags.md is the long form. The short version:
//
//   * The `.e` scripts do not reach globals by address. The loader resolves
//     numbered symbols out of tables the exe registers with sub_820FF028,
//     and sub_820F91A8 registers off_8240C6E0 as ids 500..548: 49 pointers
//     into the block at 0x8243C230. Entry 47 is 0x8243C369, and the next
//     entry is 0x8243CB69, so that entry owns exactly 2048 bytes.
//
//   * Nothing in default.xex reads or writes those 2048 bytes. They have no
//     xrefs at all, because only script bytecode addresses them. They are
//     zeroed by sub_820F91A8 at startup and by sub_82240D40 on the way back
//     to the title, both as part of the whole 4412-byte block, and the block
//     is written to the save by sub_82241190 and restored by sub_82240AF8.
//
//   * Three saves at different points in the story show the region holding
//     sparse bytes whose set bits only ever accumulate: 0x07 -> 0x3F,
//     0x3E -> 0xFE, 0x40 -> 0xC0. That is a bit array filled from the low bit
//     up, one bit per remembered event, which is also what the debug room
//     (zzz01.e, "which flag shall I toggle? specify 0 and every flag resets")
//     says it is.
//
//   * The last two bytes are the odd ones out: a big-endian u16 that does not
//     read as bits (0x046A, 0x0834) and grows with progress. That is the
//     debug room's scenario counter, so the flag bank proper is 2046 bytes.
//
// Threading. The exported entry points are plain guest-memory loads and
// stores and are safe from any thread. The events are published from the mod
// registry's frame tick.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_flag_api.h"
#include "flag_system.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// Script symbol 547: the flag bank.
constexpr uint32_t kFlagsAddr = 0x8243C369u;
constexpr uint32_t kFlagBytes = ETERNALSONATA_FLAG_BYTES;
// The two bytes after it, still inside symbol 547's 2048.
constexpr uint32_t kScenarioAddr = kFlagsAddr + kFlagBytes;

constexpr int kFlagCount = ETERNALSONATA_FLAG_COUNT;

// A new game, a title-screen reset and a loaded save all rewrite the whole
// bank. Reporting that one flag at a time would be thousands of events for
// something a subscriber can only sanely handle as "re-read everything", so
// past this many changes in one tick we publish the bulk event instead.
constexpr size_t kBulkThreshold = 64;

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Last observed bank, for the event poll. `g_have_snapshot` is false before
// the first tick and after a load, which is what makes a restored save adopt
// silently instead of republishing everything it holds.
bool g_have_snapshot = false;
std::vector<uint8_t> g_snapshot;
int g_scenario_snapshot = 0;

rex::memory::Memory* Mem() { return g_runtime ? g_runtime->memory() : nullptr; }

bool BankReadable() {
  auto* memory = Mem();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(kFlagsAddr);
  return heap && heap->QueryRangeAccess(kFlagsAddr, kScenarioAddr + 1u) !=
                     rex::memory::PageAccess::kNoAccess;
}

// Caller holds g_mutex and has checked BankReadable().
uint8_t* BankHost() {
  auto* memory = Mem();
  return memory ? memory->TranslateVirtual<uint8_t*>(kFlagsAddr) : nullptr;
}

bool ValidIndex(int index) { return index >= 0 && index < kFlagCount; }

int ReadScenario(const uint8_t* bank) {
  const uint8_t* p = bank + kFlagBytes;
  return (static_cast<int>(p[0]) << 8) | static_cast<int>(p[1]);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void Publish(const char* event, uint64_t u64, double f64) {
  rex::Runtime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    runtime = g_runtime;
  }
  if (!runtime) {
    return;
  }
  auto* registry = runtime->mod_registry();
  if (!registry) {
    return;
  }
  rex::system::ModRegistry::EventPayload payload;
  payload.u64 = u64;
  payload.f64 = f64;
  registry->Publish(event, payload);
}

// Runs once per guest frame off the mod registry's tick.
void Tick() {
  std::vector<std::pair<int, int>> changed;
  size_t changed_count = 0;
  bool scenario_changed = false;
  int scenario = 0;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!BankReadable()) {
      // Guest memory went away under us (shutdown); start clean next time.
      g_have_snapshot = false;
      return;
    }
    const uint8_t* bank = BankHost();
    if (!bank) {
      g_have_snapshot = false;
      return;
    }

    scenario = ReadScenario(bank);

    if (g_have_snapshot) {
      for (uint32_t byte = 0; byte < kFlagBytes; ++byte) {
        const uint8_t diff = bank[byte] ^ g_snapshot[byte];
        if (!diff) {
          continue;
        }
        for (int bit = 0; bit < 8; ++bit) {
          if (!(diff & (1u << bit))) {
            continue;
          }
          ++changed_count;
          if (changed_count <= kBulkThreshold) {
            const int index = static_cast<int>(byte) * 8 + bit;
            changed.emplace_back(index, (bank[byte] >> bit) & 1);
          }
        }
      }
      scenario_changed = scenario != g_scenario_snapshot;
    }

    g_snapshot.assign(bank, bank + kFlagBytes);
    g_scenario_snapshot = scenario;
    g_have_snapshot = true;
  }

  if (changed_count > kBulkThreshold) {
    Publish(ETERNALSONATA_FLAG_EVENT_BULK_CHANGED,
            static_cast<uint64_t>(changed_count), 0.0);
  } else {
    for (const auto& [index, value] : changed) {
      Publish(ETERNALSONATA_FLAG_EVENT_CHANGED, static_cast<uint64_t>(index),
              static_cast<double>(value));
    }
  }
  if (scenario_changed) {
    Publish(ETERNALSONATA_FLAG_EVENT_SCENARIO_CHANGED, 0,
            static_cast<double>(scenario));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindFlagSystem(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
    g_snapshot.assign(kFlagBytes, 0);
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

void NotifyFlagsSaveLoaded() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_flag_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataFlagsAbiVersion(void) {
  return ETERNALSONATA_FLAGS_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataAreFlagsAvailable(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return BankReadable() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetFlagCount(void) {
  return ETERNALSONATA_FLAG_COUNT;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetFlag(int index) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ValidIndex(index)) {
    return ETERNALSONATA_FLAG_ERR_INVALID_INDEX;
  }
  if (!BankReadable()) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  const uint8_t* bank = BankHost();
  if (!bank) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  return (bank[index >> 3] >> (index & 7)) & 1;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetFlag(int index, int value) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ValidIndex(index)) {
    return ETERNALSONATA_FLAG_ERR_INVALID_INDEX;
  }
  if (value != 0 && value != 1) {
    return ETERNALSONATA_FLAG_ERR_INVALID_VALUE;
  }
  if (!BankReadable()) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  uint8_t* bank = BankHost();
  if (!bank) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
  uint8_t& byte = bank[index >> 3];
  byte = value ? static_cast<uint8_t>(byte | mask)
               : static_cast<uint8_t>(byte & ~mask);
  return ETERNALSONATA_FLAG_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataCopyFlags(uint8_t* dst,
                                                            int first_byte,
                                                            int byte_count) {
  using namespace eternalsonata;
  if (!dst || byte_count < 0 || first_byte < 0 ||
      first_byte > static_cast<int>(kFlagBytes) ||
      byte_count > static_cast<int>(kFlagBytes) - first_byte) {
    return ETERNALSONATA_FLAG_ERR_INVALID_INDEX;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!BankReadable()) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  const uint8_t* bank = BankHost();
  if (!bank) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  std::memcpy(dst, bank + first_byte, static_cast<size_t>(byte_count));
  return ETERNALSONATA_FLAG_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataResetFlags(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!BankReadable()) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  uint8_t* bank = BankHost();
  if (!bank) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  std::memset(bank, 0, kFlagBytes);
  return ETERNALSONATA_FLAG_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetScenarioCounter(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!BankReadable()) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  const uint8_t* bank = BankHost();
  if (!bank) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  return ReadScenario(bank);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetScenarioCounter(int value) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (value < 0 || value > 0xFFFF) {
    return ETERNALSONATA_FLAG_ERR_INVALID_VALUE;
  }
  if (!BankReadable()) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  uint8_t* bank = BankHost();
  if (!bank) {
    return ETERNALSONATA_FLAG_ERR_UNAVAILABLE;
  }
  bank[kFlagBytes] = static_cast<uint8_t>((value >> 8) & 0xFF);
  bank[kFlagBytes + 1] = static_cast<uint8_t>(value & 0xFF);
  return ETERNALSONATA_FLAG_OK;
}
