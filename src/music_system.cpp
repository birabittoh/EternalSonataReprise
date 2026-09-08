// eternalsonata - Music menu: reading and changing which of the game's OST
// tracks the three tab gallery has unlocked, and the mod-facing API.
//
// Everything here was derived from the retail xex; docs/music.md is the long
// form. The short version:
//
//   * byte_8255EE70 is the same 100-entry collectible flag array the piano
//     music uses (see piano_music_system.cpp). Entries 1..66 are the OST
//     tracks, so this file only ever touches that range: 80..86 belong to the
//     Chopin pieces and the rest to other collectibles.
//
//   * byte_8238E128 is the tab table, three rows of 32 bytes holding the track
//     ids of each tab in menu order, and byte_822FF594[3] = {22, 31, 9} is how
//     many of each row are real. Both are read rather than hard-coded so this
//     file keeps agreeing with what the screen walks.
//
//   * sub_821FEBC8 builds one row and makes the whole locked/unlocked
//     decision: byte_8255EE70[track_id] picks between title id `track_id` and
//     id 0, "???", in the BTX blob at 0x82053E10. sub_82226858 tests the same
//     flag before playing sound\cxs\MP1<nn>.cxs.
//
// Threading. The exported entry points are called from mods, i.e. usually from
// the ImGui draw thread. Everything is a plain guest-memory load or store
// except playback, which runs guest code and so goes through
// guest_main_thread.h and answers QUEUED. The events are published from the
// mod registry's frame tick.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_music_api.h"
#include "guest_main_thread.h"
#include "music_system.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// The shared collectible flag array. Entries 1..66 are the OST tracks.
constexpr uint32_t kFlagsAddr = 0x8255EE70u;
constexpr uint32_t kFlagsCount = 100u;

// u8[3][32]: the track ids of each tab, in menu order, zero padded.
constexpr uint32_t kTabTableAddr = 0x8238E128u;
constexpr uint32_t kTabStride = 32u;

// u8[3]: how many entries of each tab row are real.
constexpr uint32_t kTabCountsAddr = 0x822FF594u;

// The menu's own BTX blob. Text id == track id; id 0 is the "???" placeholder.
constexpr uint32_t kTitleBlockAddr = 0x82053E10u;

// Which language block of a BTX blob the game is reading.
constexpr uint32_t kLanguageIndexAddr = 0x8243D370u;

constexpr int kTabCount = ETERNALSONATA_MUSIC_TAB_COUNT;
constexpr int kTrackIdMin = ETERNALSONATA_MUSIC_TRACK_ID_MIN;
constexpr int kTrackIdMax = ETERNALSONATA_MUSIC_TRACK_ID_MAX;
constexpr int kTrackCount = ETERNALSONATA_MUSIC_TRACK_COUNT;

// sub_821F77F0(unused, filename) prepends sound\cxs\ and starts the stream.
REX_IMPORT(__imp__sub_821F77F0, g_play_bgm, u32(u32, u32));

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Last observed unlock state, for the event poll. `g_have_snapshot` is false
// before the first tick and after a load, which is what makes a restored save
// adopt silently instead of republishing everything it holds.
bool g_have_snapshot = false;
std::array<bool, kTrackCount + 1> g_snapshot{};  // indexed by track id

// The guest-side filename buffer playback hands to the game. Allocated once,
// on the guest thread, and reused.
uint32_t g_filename_guest = 0;

std::mutex g_text_mutex;  // guards g_text only
std::map<int, std::string> g_text;

rex::memory::Memory* Mem() { return g_runtime ? g_runtime->memory() : nullptr; }

// ---------------------------------------------------------------------------
// Guest memory access
// ---------------------------------------------------------------------------

bool Readable(uint32_t address, uint32_t span) {
  auto* memory = Mem();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + span - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

template <typename T>
T ReadGuest(uint32_t address, T fallback = T{}) {
  auto* memory = Mem();
  if (!memory) {
    return fallback;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? rex::memory::load_and_swap<T>(host) : fallback;
}

uint8_t ReadGuestByte(uint32_t address) {
  auto* memory = Mem();
  if (!memory) {
    return 0;
  }
  auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : uint8_t{0};
}

void WriteGuestByte(uint32_t address, uint8_t value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    *host = value;
  }
}

std::string ReadGuestString(uint32_t address, size_t limit = 256) {
  auto* memory = Mem();
  if (!memory || !Readable(address, 1u)) {
    return {};
  }
  const auto* host = memory->TranslateVirtual<const char*>(address);
  if (!host) {
    return {};
  }
  size_t length = 0;
  while (length < limit && host[length] != '\0') {
    ++length;
  }
  return std::string(host, length);
}

// ---------------------------------------------------------------------------
// BTX text
// ---------------------------------------------------------------------------
//
// Same layout item_system.cpp documents at length: a 'BTX ' header pointing at
// a chain of per-language blocks, each with its own {id, offset} entry table.
// Duplicated rather than shared because the item API's exported readers only
// answer for item ids.
std::string ReadBtxString(uint32_t block, int text_id) {
  if (!Readable(block, 0x10u) || ReadGuest<uint32_t>(block) != 0x42545820u /* 'BTX ' */) {
    return {};
  }
  const uint32_t block_count = ReadGuest<uint32_t>(block + 0x0Cu);
  if (block_count == 0 || block_count > 16u) {
    return {};
  }

  uint32_t language = ReadGuest<uint32_t>(kLanguageIndexAddr);
  if (language >= block_count) {
    language = 0;
  }

  uint32_t entry_block = block + ReadGuest<uint32_t>(block + 0x04u);
  for (uint32_t i = 0; i < language; ++i) {
    if (!Readable(entry_block, 0x14u)) {
      return {};
    }
    entry_block += ReadGuest<uint32_t>(entry_block + 0x08u);
  }
  if (!Readable(entry_block, 0x14u)) {
    return {};
  }

  const uint32_t table = entry_block + ReadGuest<uint32_t>(entry_block + 0x04u);
  const uint32_t count = ReadGuest<uint32_t>(entry_block + 0x10u);
  if (count == 0 || count > 0x10000u || !Readable(table, 8u * count)) {
    return {};
  }

  auto entry_at = [&](uint32_t index) { return table + 8u * index; };
  const auto wanted = static_cast<uint32_t>(text_id);
  if (wanted < count && ReadGuest<uint32_t>(entry_at(wanted)) == wanted) {
    return ReadGuestString(entry_block + ReadGuest<uint32_t>(entry_at(wanted) + 4u));
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (ReadGuest<uint32_t>(entry_at(i)) == wanted) {
      return ReadGuestString(entry_block + ReadGuest<uint32_t>(entry_at(i) + 4u));
    }
  }
  return {};
}

// Never null. The string is owned by g_text and outlives every caller.
const char* CachedTitle(int text_id) {
  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    const auto it = g_text.find(text_id);
    if (it != g_text.end()) {
      return it->second.c_str();
    }
  }
  std::string value = ReadBtxString(kTitleBlockAddr, text_id);
  std::lock_guard<std::mutex> lock(g_text_mutex);
  return g_text.emplace(text_id, std::move(value)).first->second.c_str();
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

bool Bound() { return g_runtime != nullptr; }

bool ListReadable() {
  return Bound() && Readable(kFlagsAddr, kFlagsCount) &&
         Readable(kTabTableAddr, kTabStride * kTabCount) && Readable(kTabCountsAddr, kTabCount);
}

bool ValidId(int track_id) { return track_id >= kTrackIdMin && track_id <= kTrackIdMax; }

int TabRowCount(int tab) {
  if (tab < 0 || tab >= kTabCount) {
    return 0;
  }
  const int count = ReadGuestByte(kTabCountsAddr + static_cast<uint32_t>(tab));
  return std::min<int>(count, static_cast<int>(kTabStride));
}

int TrackAt(int tab, int row) {
  if (row < 0 || row >= TabRowCount(tab)) {
    return 0;
  }
  return ReadGuestByte(kTabTableAddr + static_cast<uint32_t>(tab) * kTabStride +
                       static_cast<uint32_t>(row));
}

// Where the menu lists `track_id`, or {-1, -1} for the four ids no tab holds.
std::pair<int, int> Placement(int track_id) {
  for (int tab = 0; tab < kTabCount; ++tab) {
    const int rows = TabRowCount(tab);
    for (int row = 0; row < rows; ++row) {
      if (TrackAt(tab, row) == track_id) {
        return {tab, row};
      }
    }
  }
  return {-1, -1};
}

bool Unlocked(int track_id) {
  return ReadGuestByte(kFlagsAddr + static_cast<uint32_t>(track_id)) != 0;
}

void ReadTrack(int track_id, EternalSonataMusicTrack* out) {
  std::memset(out, 0, sizeof(*out));

  const auto [tab, row] = Placement(track_id);
  out->track_id = track_id;
  out->tab = tab;
  out->row = row;
  out->number = row >= 0 ? row + 1 : 0;
  out->unlocked = Unlocked(track_id) ? 1 : 0;
  out->title_text_id = track_id;
}

// Returns false if the track was already in the requested state, so a caller
// can report how many it actually changed. The event is left to the next tick,
// which sees the transition like any other and keeps every event on one thread.
bool SetUnlockedLocked(int track_id, bool unlocked) {
  if (Unlocked(track_id) == unlocked) {
    return false;
  }
  WriteGuestByte(kFlagsAddr + static_cast<uint32_t>(track_id), unlocked ? 1u : 0u);
  return true;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

// Runs `work` where guest calls are legal: right here if we are already on the
// guest main thread, otherwise on its next frame. See guest_main_thread.h.
int RunOnGuestThread(std::function<int()> work) {
  if (OnGuestMainThread()) {
    return work();
  }
  PostToGuestMainThread([work] { work(); });
  return ETERNALSONATA_MUSIC_QUEUED;
}

// Guest thread only.
int PlayOnGuestThread(int track_id) {
  auto* memory = Mem();
  if (!memory) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_filename_guest) {
      g_filename_guest = memory->SystemHeapAlloc(32, 0x20);
    }
  }
  if (!g_filename_guest) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }

  char name[32] = {};
  std::snprintf(name, sizeof(name), "MP1%02d.cxs", track_id);
  auto* host = memory->TranslateVirtual<char*>(g_filename_guest);
  if (!host) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  std::memcpy(host, name, std::strlen(name) + 1);

  g_play_bgm(0, g_filename_guest);
  return ETERNALSONATA_MUSIC_OK;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void PublishMusicEvent(const char* event_name, int track_id, int tab) {
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
  payload.u64 = static_cast<uint64_t>(track_id);
  payload.f64 = static_cast<double>(tab);
  registry->Publish(event_name, payload);
}

// Runs once per guest frame off the mod registry's tick.
void Tick() {
  std::vector<std::pair<int, int>> unlocked;  // track id, tab
  std::vector<std::pair<int, int>> locked;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ListReadable()) {
      // Guest memory went away under us (shutdown); start clean next time.
      g_have_snapshot = false;
      return;
    }

    std::array<bool, kTrackCount + 1> state{};
    for (int id = kTrackIdMin; id <= kTrackIdMax; ++id) {
      state[static_cast<size_t>(id)] = Unlocked(id);
    }

    if (g_have_snapshot) {
      for (int id = kTrackIdMin; id <= kTrackIdMax; ++id) {
        const auto slot = static_cast<size_t>(id);
        if (state[slot] == g_snapshot[slot]) {
          continue;
        }
        (state[slot] ? unlocked : locked).emplace_back(id, Placement(id).first);
      }
    }

    g_snapshot = state;
    g_have_snapshot = true;
  }

  for (const auto& [track_id, tab] : unlocked) {
    PublishMusicEvent(ETERNALSONATA_MUSIC_EVENT_UNLOCKED, track_id, tab);
  }
  for (const auto& [track_id, tab] : locked) {
    PublishMusicEvent(ETERNALSONATA_MUSIC_EVENT_LOCKED, track_id, tab);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindMusicSystem(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

void NotifyMusicSaveLoaded() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_music_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataMusicAbiVersion(void) {
  return ETERNALSONATA_MUSIC_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsMusicAvailable(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return ListReadable() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetMusicTrackCount(void) {
  return ETERNALSONATA_MUSIC_TRACK_COUNT;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetMusicTabTrackCount(int tab) {
  using namespace eternalsonata;
  if (tab < ETERNALSONATA_MUSIC_TAB_NONE || tab >= kTabCount) {
    return ETERNALSONATA_MUSIC_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  if (tab >= 0) {
    return TabRowCount(tab);
  }
  int unlisted = 0;
  for (int id = kTrackIdMin; id <= kTrackIdMax; ++id) {
    if (Placement(id).first < 0) {
      ++unlisted;
    }
  }
  return unlisted;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetUnlockedMusicTrackCount(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  int count = 0;
  for (int id = kTrackIdMin; id <= kTrackIdMax; ++id) {
    if (Unlocked(id)) {
      ++count;
    }
  }
  return count;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetMusicTrack(
    int track_id, EternalSonataMusicTrack* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_MUSIC_ERR_INVALID_ARGUMENT;
  }
  if (!ValidId(track_id)) {
    return ETERNALSONATA_MUSIC_ERR_NO_SUCH_TRACK;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  ReadTrack(track_id, out);
  return ETERNALSONATA_MUSIC_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetMusicTracks(
    EternalSonataMusicTrack* out, int max, int tab) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_MUSIC_ERR_INVALID_ARGUMENT;
  }
  if (tab < ETERNALSONATA_MUSIC_TAB_ALL || tab >= kTabCount) {
    return ETERNALSONATA_MUSIC_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }

  const int available = tab >= 0 ? TabRowCount(tab) : kTrackCount;
  if (max == 0) {
    return available;
  }
  const int written = std::min(max, available);
  for (int i = 0; i < written; ++i) {
    ReadTrack(tab >= 0 ? TrackAt(tab, i) : kTrackIdMin + i, &out[i]);
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsMusicTrackUnlocked(int track_id) {
  using namespace eternalsonata;
  if (!ValidId(track_id)) {
    return ETERNALSONATA_MUSIC_ERR_NO_SUCH_TRACK;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  return Unlocked(track_id) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT const char* EternalSonataGetMusicTrackName(int track_id) {
  using namespace eternalsonata;
  if (!ValidId(track_id)) {
    return "";
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Bound()) {
    return "";
  }
  return CachedTitle(track_id);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetMusicTrackUnlocked(int track_id,
                                                                       int unlocked) {
  using namespace eternalsonata;
  if (!ValidId(track_id)) {
    return ETERNALSONATA_MUSIC_ERR_NO_SUCH_TRACK;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  SetUnlockedLocked(track_id, unlocked != 0);
  return ETERNALSONATA_MUSIC_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetAllMusicTracksUnlocked(int unlocked) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
  }
  int changed = 0;
  for (int id = kTrackIdMin; id <= kTrackIdMax; ++id) {
    if (SetUnlockedLocked(id, unlocked != 0)) {
      ++changed;
    }
  }
  return changed;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataPlayMusicTrack(int track_id) {
  using namespace eternalsonata;
  if (!ValidId(track_id)) {
    return ETERNALSONATA_MUSIC_ERR_NO_SUCH_TRACK;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ListReadable()) {
      return ETERNALSONATA_MUSIC_ERR_UNAVAILABLE;
    }
    if (Placement(track_id).first < 0) {
      return ETERNALSONATA_MUSIC_ERR_NOT_LISTED;
    }
    if (!Unlocked(track_id)) {
      return ETERNALSONATA_MUSIC_ERR_LOCKED;
    }
  }
  return RunOnGuestThread([track_id] { return PlayOnGuestThread(track_id); });
}
