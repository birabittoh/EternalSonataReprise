// eternalsonata - Piano Music: reading and changing which of the seven Chopin
// piano pieces the menu has unlocked, and the mod-facing API.
//
// Everything here was derived from the retail xex. The short version:
//
//   * byte_8255EE70 is a 100-entry byte array of collectible flags, 1 for
//     "obtained". It is saved and restored whole (sub_82241190 writes the 100
//     bytes, sub_82240AF8 reads them back, both only for save version >= 2)
//     and the script VM sets a single entry through sub_820E8030.
//
//   * dword_82016134 is the seven-entry table of flag ids the Piano Music
//     screen walks: 80, 81, ... 86, one per piece in menu order. The same
//     number names the piece's music, sound\cxs\MP1<n>.wav, which is why
//     sub_821FBBD0 can take a filename and light the matching flag.
//
//   * sub_8222B348 builds the list. For row i it draws the real title when
//     byte_8255EE70[dword_82016134[i]] is set or bit i of dword_8243F35C is,
//     and the "???" placeholder otherwise. sub_8222A168, the row activation,
//     tests exactly the same pair before letting the piece be played.
//
//   * dword_8243F35C is a session-only override: sub_8222B260 ORs bits into it
//     when the PIANO_CHECK screen (sub_82209758) reports pieces that are
//     available without having been collected. Nothing saves it, so it is
//     reported separately from the flag the save carries.
//
//     That dword is not the piano music's own storage, though: it is one slot
//     of the eight-dword scratch block at 0x8243F358 that every status menu
//     screen reuses, and each of them zeroes the block on entry. The Music menu
//     (music_system.cpp) parks a row index of -1 there, which as a bitmask
//     would read as every piece unlocked. So the mask is mirrored host side
//     from the two guest routines that own it and the mirror is what this file
//     reports; the guest dword is only written back while it still agrees with
//     the mirror, i.e. while the piano screen still owns the slot.
//
//   * Text ids into the packed UI blob at 0x8203DD60: the piece titles are ids
//     0..6 and the "???" placeholder is id 9. sub_8222B8D8 reads history page
//     p (1-based) of piece i as id 10 * i + 9 + p, so the pages run from
//     10 * (i + 1); byte_82029D30[i] is how many there are.
//
// Threading. The exported entry points are called from mods, i.e. usually from
// the ImGui draw thread. Everything here is a plain guest-memory load or store
// - no guest routine is called, so there is no queued-write path - and the
// events are published from the mod registry's frame tick.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_piano_music_api.h"
#include "piano_music_system.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// The collectible flag array. Only entries 80..86 belong to piano music; the
// rest are other collectibles and are left alone.
constexpr uint32_t kFlagsAddr = 0x8255EE70u;
constexpr uint32_t kFlagsCount = 100u;

// u32[7], the flag id per piece in menu order. Read rather than hard-coded so
// this file keeps agreeing with the table the screen actually walks.
constexpr uint32_t kFlagIdTableAddr = 0x82016134u;

// u8[7], how many history pages each piece has.
constexpr uint32_t kStoryPagesAddr = 0x82029D30u;

// The session-only "available anyway" bitmask, bit i per piece.
constexpr uint32_t kSessionMaskAddr = 0x8243F35Cu;

constexpr int kCount = ETERNALSONATA_PIANO_MUSIC_COUNT;

// Text id of the "???" the menu draws in place of a locked piece's title.
constexpr int32_t kLockedTitleTextId = 9;

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Last observed unlock state, for the event poll. `g_have_snapshot` is false
// before the first tick and after a load, which is what makes a restored save
// adopt silently instead of republishing everything it holds.
bool g_have_snapshot = false;
std::array<bool, kCount> g_snapshot{};

// Host mirror of the session-only mask. See the note at the top: the guest
// dword is shared scratch, so it is only trustworthy while the piano screen
// owns it.
uint32_t g_session_mask = 0;

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

template <typename T>
void WriteGuest(uint32_t address, T value) {
  auto* memory = Mem();
  if (!memory) {
    return;
  }
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (host) {
    rex::memory::store_and_swap<T>(host, value);
  }
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

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

bool Bound() { return g_runtime != nullptr; }

bool ListReadable() {
  return Bound() && Readable(kFlagsAddr, kFlagsCount) &&
         Readable(kFlagIdTableAddr, 4u * kCount) && Readable(kSessionMaskAddr, 4u);
}

// Flag id of the piece at `index`, or -1 if the table holds something outside
// the flag array (which would mean this file and the game have drifted apart).
int FlagId(int index) {
  const auto id = static_cast<int32_t>(
      ReadGuest<uint32_t>(kFlagIdTableAddr + static_cast<uint32_t>(index) * 4u));
  return (id < 0 || id >= static_cast<int32_t>(kFlagsCount)) ? -1 : id;
}

bool UnlockedSaved(int index) {
  const int flag = FlagId(index);
  return flag >= 0 && ReadGuestByte(kFlagsAddr + static_cast<uint32_t>(flag)) != 0;
}

bool UnlockedSession(int index) { return (g_session_mask & (1u << index)) != 0; }

// What the menu itself tests: either half is enough to draw the real title.
bool Unlocked(int index) {
  return UnlockedSaved(index) || UnlockedSession(index);
}

void ReadPiece(int index, EternalSonataPianoMusic* out) {
  std::memset(out, 0, sizeof(*out));

  out->index = index;
  out->flag_id = FlagId(index);

  out->unlocked_saved = UnlockedSaved(index) ? 1 : 0;
  out->unlocked_session = UnlockedSession(index) ? 1 : 0;
  out->unlocked = (out->unlocked_saved || out->unlocked_session) ? 1 : 0;

  out->title_text_id = index;
  out->locked_title_text_id = kLockedTitleTextId;
  out->first_story_text_id = 10 * (index + 1);
  out->story_page_count =
      Readable(kStoryPagesAddr, kCount)
          ? static_cast<int32_t>(ReadGuestByte(kStoryPagesAddr + static_cast<uint32_t>(index)))
          : 0;
}

// Locks or unlocks one piece, with g_mutex held. Returns false if it was
// already in the requested state, so a caller can report how many it actually
// changed. The event is left to the next tick, which sees the transition like
// any other and keeps every event on one thread.
bool SetUnlockedLocked(int index, bool unlocked) {
  const int flag = FlagId(index);
  if (flag < 0) {
    return false;
  }
  if (Unlocked(index) == unlocked) {
    return false;
  }

  WriteGuestByte(kFlagsAddr + static_cast<uint32_t>(flag), unlocked ? 1u : 0u);
  if (!unlocked) {
    // The saved flag alone is not enough to hide a piece again: the menu also
    // takes the session bit, and nothing clears that until the title screen.
    // Only write the guest copy back while it still matches the mirror, so a
    // screen that has since taken the scratch slot over is left alone.
    const bool owned = ReadGuest<uint32_t>(kSessionMaskAddr) == g_session_mask;
    g_session_mask &= ~(1u << index);
    if (owned) {
      WriteGuest<uint32_t>(kSessionMaskAddr, g_session_mask);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void PublishPianoMusicEvent(const char* event_name, int index, int flag_id) {
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
  payload.u64 = static_cast<uint64_t>(index);
  payload.f64 = static_cast<double>(flag_id);
  registry->Publish(event_name, payload);
}

// Runs once per guest frame off the mod registry's tick.
void Tick() {
  std::vector<std::pair<int, int>> unlocked;  // index, flag id
  std::vector<std::pair<int, int>> locked;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ListReadable()) {
      // Guest memory went away under us (shutdown); start clean next time.
      g_have_snapshot = false;
      return;
    }

    std::array<bool, kCount> state{};
    for (int i = 0; i < kCount; ++i) {
      state[static_cast<size_t>(i)] = Unlocked(i);
    }

    if (g_have_snapshot) {
      for (int i = 0; i < kCount; ++i) {
        const auto slot = static_cast<size_t>(i);
        if (state[slot] == g_snapshot[slot]) {
          continue;
        }
        (state[slot] ? unlocked : locked).emplace_back(i, FlagId(i));
      }
    }

    g_snapshot = state;
    g_have_snapshot = true;
  }

  for (const auto& [index, flag_id] : unlocked) {
    PublishPianoMusicEvent(ETERNALSONATA_PIANO_MUSIC_EVENT_UNLOCKED, index, flag_id);
  }
  for (const auto& [index, flag_id] : locked) {
    PublishPianoMusicEvent(ETERNALSONATA_PIANO_MUSIC_EVENT_LOCKED, index, flag_id);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindPianoMusicSystem(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

void NotifyPianoMusicSaveLoaded() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Session mask mirror
// ---------------------------------------------------------------------------

// The Piano Music screen's init, which zeroes the scratch block the mask lives
// in before rebuilding the list.
REX_EXTERN(__imp__sub_82229FC0);
REX_HOOK_RAW(sub_82229FC0) {
  __imp__sub_82229FC0(ctx, base);
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_session_mask = 0;
}

// The only routine that adds to the mask, from what the PIANO_CHECK screen
// found.
REX_EXTERN(__imp__sub_8222B260);
REX_HOOK_RAW(sub_8222B260) {
  const u32 added = ctx.r3.u32;
  __imp__sub_8222B260(ctx, base);
  if (added) {
    using namespace eternalsonata;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session_mask |= added;
  }
}

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_piano_music_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataPianoMusicAbiVersion(void) {
  return ETERNALSONATA_PIANO_MUSIC_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsPianoMusicAvailable(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return ListReadable() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPianoMusicCount(void) {
  return ETERNALSONATA_PIANO_MUSIC_COUNT;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetUnlockedPianoMusicCount(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE;
  }
  int count = 0;
  for (int i = 0; i < kCount; ++i) {
    if (Unlocked(i)) {
      ++count;
    }
  }
  return count;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPianoMusic(
    int index, EternalSonataPianoMusic* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_INVALID_ARGUMENT;
  }
  if (index < 0 || index >= ETERNALSONATA_PIANO_MUSIC_COUNT) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_NO_SUCH_PIECE;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE;
  }
  ReadPiece(index, out);
  return ETERNALSONATA_PIANO_MUSIC_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetAllPianoMusic(
    EternalSonataPianoMusic* out, int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE;
  }
  if (max == 0) {
    return kCount;
  }
  const int written = std::min(max, kCount);
  for (int i = 0; i < written; ++i) {
    ReadPiece(i, &out[i]);
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsPianoMusicUnlocked(int index) {
  using namespace eternalsonata;
  if (index < 0 || index >= ETERNALSONATA_PIANO_MUSIC_COUNT) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_NO_SUCH_PIECE;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE;
  }
  return Unlocked(index) ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetPianoMusicUnlocked(int index,
                                                                        int unlocked) {
  using namespace eternalsonata;
  if (index < 0 || index >= ETERNALSONATA_PIANO_MUSIC_COUNT) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_NO_SUCH_PIECE;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE;
  }
  SetUnlockedLocked(index, unlocked != 0);
  return ETERNALSONATA_PIANO_MUSIC_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataSetAllPianoMusicUnlocked(int unlocked) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ListReadable()) {
    return ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE;
  }
  int changed = 0;
  for (int i = 0; i < kCount; ++i) {
    if (SetUnlockedLocked(i, unlocked != 0)) {
      ++changed;
    }
  }
  return changed;
}
