// eternalsonata - Photo album: reading the camera's album, and the mod-facing
// API.
//
// Everything here was derived from the retail xex. The short version:
//
//   * The album is a fixed twelve records of 184 bytes at 0x8255E508, owned by
//     the object at 0x8255E500 (constructed by sub_822E5310, which runs
//     sub_821F9CD8 over exactly twelve of them). There is no growth path: the
//     display-order table, the per-slot texture handles and the album screen's
//     own widget arrays are all twelve wide, and sub_821E6FD8 refuses to add a
//     thirteenth.
//
//   * byte_8255EDB4 is the photograph count and byte_8255EDB5[0..11] is the
//     display order, holding record slots with -1 for an empty entry. New
//     photographs are unshifted onto the front by sub_821E6FD8, so the newest
//     is always display index 0. sub_821FA300 compacts the table after a sale
//     or a trashing.
//
//   * Within a record: +12 is the guest handle of the 416x256 R5G6B5 surface
//     holding the picture, +36 and +96 are two groups of three 20-byte subject
//     entries (a u16 id of -1 for an empty one), +156 is a flag word whose bit
//     1 means "no picture in this record", +164 is the quality score 0..1 that
//     the album prints as a percentage and picks its icon tier from, +168 is
//     the sale price before development scaling, +172 is the value of the
//     clock at 0x82565780 when the photograph was taken, and +176 is the party
//     progress counter at the same moment. sub_821C4248 is the reset that sets
//     the initial -1s, and sub_821FA570 / sub_821FA6C0 are the save and load
//     of exactly those fields.
//
//   * Development is not a countdown. sub_82209478 evaluates it from the two
//     snapshots each time the album binds a slot, caches the result in
//     flt_8255EE1C[record] (which the save carries), and the album draws the
//     picture under that much haze - 1 freshly taken, 0 fully developed. The
//     formula is reproduced below in Development(); it is the one thing here
//     that is arithmetic rather than a field read, so it is spelled out
//     against the disassembly line by line.
//
// Because development is only evaluated when the album screen is open, a poll
// of the game's cached copy would report a photograph as finished whenever the
// player next happened to look at it. So the per-frame tick reruns the formula
// itself, which is what lets the developed event land when development
// actually completes.
//
// Threading. The exported entry points are called from mods, i.e. usually from
// the ImGui draw thread. Everything here is a plain guest-memory load or store
// - no guest routine is called, so there is no queued-write path - and the
// events are published from the mod registry's frame tick.

#include "generated/eternalsonata_init.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include "eternalsonata_photo_api.h"
#include "photo_system.h"

namespace eternalsonata {
namespace {

// ---------------------------------------------------------------------------
// Guest addresses
// ---------------------------------------------------------------------------

// The twelve photograph records. The owning object is eight bytes earlier at
// 0x8255E500, which is why the game's own code addresses these as
// 0x8255E500[46 * slot] in places and 0x8255E508 + 184 * slot in others.
constexpr uint32_t kRecordsAddr = 0x8255E508u;
constexpr uint32_t kRecordStride = 184u;
constexpr int kCapacity = ETERNALSONATA_PHOTO_CAPACITY;

// u8 photograph count, then the twelve-entry display order (record slots, -1
// for empty) immediately after it.
constexpr uint32_t kCountAddr = 0x8255EDB4u;
constexpr uint32_t kOrderAddr = 0x8255EDB5u;

// float[12] indexed by record slot: the game's own cached development, saved
// and restored with the album. Written by sub_82209478 only.
constexpr uint32_t kDevelopmentCacheAddr = 0x8255EE1Cu;

// The clock a photograph's +172 snapshot is taken from, and which the
// development formula measures elapsed time against. Restored on load by
// sub_82240AF8 from the save's own copy.
constexpr uint32_t kClockAddr = 0x82565780u;

// The party's per-character stat blocks. The development formula uses the
// highest level in the party as its "how far has the player got" term, which
// is the first u32 of each 48-byte block. Same table party_system.cpp reads.
constexpr uint32_t kStatsAddr = 0x8243FEE8u;
constexpr uint32_t kStatsStride = 48u;
constexpr int kStatsCount = 10;

// Offsets within a record.
constexpr uint32_t kRecImageHandle = 12u;
constexpr uint32_t kRecSubjectsA = 36u;   // 3 entries
constexpr uint32_t kRecSubjectsB = 96u;   // 3 more
constexpr uint32_t kRecSubjectStride = 20u;
constexpr uint32_t kRecFlags = 156u;
constexpr uint32_t kRecScore = 164u;
constexpr uint32_t kRecPriceScale = 168u;
constexpr uint32_t kRecTakenTime = 172u;
constexpr uint32_t kRecTakenProgress = 176u;

// "This record holds no picture", the bit sub_82207A88 tests to decide whether
// to draw the photograph or a black plate with a caption.
constexpr uint32_t kFlagBlank = 0x2u;

// The constants of the development formula, in the order sub_82209478 applies
// them. Named rather than inlined because there is no way to tell them apart
// once they are bare literals.
constexpr float kTimeRate = -0.03f;      // flt_820AA9C8
constexpr float kTimeWeight = 80.0f;     // flt_820AA9C0
constexpr float kProgressRate = 0.4f;    // flt_820AA9C4
constexpr float kProgressWeight = 100.0f;  // flt_820AA8AC
constexpr float kProgressShare = 0.7f;   // flt_820AA9BC
constexpr float kAmplitude = 0.9f;       // flt_820AA8F4
constexpr float kSnapToZero = 0.05f;     // flt_820AA8E8
// The clock is divided down before it reaches the formula.
constexpr uint32_t kClockDivisor = 300u;

// The range sub_822CDE38 clamps its argument to before exponentiating. Applied
// here too so a forced snapshot cannot push the host reimplementation into an
// infinity the guest would never have produced.
constexpr double kExpMin = -708.396418532265;
constexpr double kExpMax = 709.782712893385;

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards everything below
rex::Runtime* g_runtime = nullptr;

// Records EternalSonataDevelopPhoto has forced. The rewritten snapshots get a
// photograph as close to developed as the formula allows, but on a young save
// the elapsed-time term has not had time to decay, so the result is pinned
// here as well and the sub_82209478 hook writes it through.
std::array<bool, kCapacity> g_forced{};

// Last observed album, for the event poll. `g_have_snapshot` is false before
// the first tick and after a load, which is what makes a restored album adopt
// silently instead of republishing every photograph it holds.
bool g_have_snapshot = false;
int g_snapshot_count = 0;
std::array<int8_t, kCapacity> g_snapshot_order{};
std::array<bool, kCapacity> g_snapshot_developed{};

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

// Guest floats are stored big-endian like everything else, so they go through
// the same swapping loads as the integers with a bit pattern in between.
float ReadGuestFloat(uint32_t address) {
  const uint32_t bits = ReadGuest<uint32_t>(address);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteGuestFloat(uint32_t address, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteGuest<uint32_t>(address, bits);
}

// ---------------------------------------------------------------------------
// The album
// ---------------------------------------------------------------------------

bool Bound() { return g_runtime != nullptr; }

bool AlbumReadable() {
  return Bound() && Readable(kRecordsAddr, kRecordStride * kCapacity) &&
         Readable(kCountAddr, 1u + kCapacity);
}

uint32_t RecordAddr(int record) {
  return kRecordsAddr + static_cast<uint32_t>(record) * kRecordStride;
}

int PhotoCount() {
  const int count = static_cast<int>(ReadGuestByte(kCountAddr));
  return std::clamp(count, 0, kCapacity);
}

// Record slot shown at 0-based display `index`, or -1. The table stores slots
// as signed bytes and uses -1 for an entry past the end of the album.
int RecordAt(int index) {
  if (index < 0 || index >= kCapacity) {
    return -1;
  }
  const auto slot = static_cast<int8_t>(ReadGuestByte(kOrderAddr + static_cast<uint32_t>(index)));
  return (slot < 0 || slot >= kCapacity) ? -1 : slot;
}

int IndexOfRecord(int record) {
  const int count = PhotoCount();
  for (int i = 0; i < count; ++i) {
    if (RecordAt(i) == record) {
      return i;
    }
  }
  return -1;
}

// The formula's "how far has the player got" term: the highest character level
// in the party. sub_82209478 walks the ten stat blocks taking the maximum of
// their first field.
int32_t PartyProgress() {
  int32_t best = 0;
  for (int i = 0; i < kStatsCount; ++i) {
    const auto level =
        ReadGuest<int32_t>(kStatsAddr + static_cast<uint32_t>(i) * kStatsStride);
    best = std::max(best, level);
  }
  return best;
}

float ClampedExp(float x) {
  const double clamped = std::clamp(static_cast<double>(x), kExpMin, kExpMax);
  return static_cast<float>(std::exp(clamped));
}

// sub_82209478, reproduced. Both terms are exponential decays that start near
// 1 and fall towards 0: one on clock time elapsed since the photograph was
// taken, one on how much the party has advanced since. Their product is scaled
// by kAmplitude and snapped to a clean zero once it is small enough, which is
// the moment the album stops drawing haze at all.
float Development(int record) {
  const uint32_t base = RecordAddr(record);
  const uint32_t now = ReadGuest<uint32_t>(kClockAddr) / kClockDivisor;
  const uint32_t taken = ReadGuest<uint32_t>(base + kRecTakenTime) / kClockDivisor;
  const int32_t progress = ReadGuest<int32_t>(base + kRecTakenProgress);

  const float elapsed = static_cast<float>(now) - static_cast<float>(taken);
  const float time_term =
      1.0f - 1.0f / (kTimeWeight * ClampedExp(kTimeRate * elapsed) + 1.0f);

  const float advanced = static_cast<float>(PartyProgress()) - static_cast<float>(progress);
  const float progress_term =
      1.0f - kProgressShare / (kProgressWeight * ClampedExp(kProgressRate * advanced) + 1.0f);

  const float haze = progress_term * time_term * kAmplitude;
  return haze < kSnapToZero ? 0.0f : haze;
}

// Development as the API reports it: the formula, except on a record the mod
// has forced, where it is zero by definition.
float EffectiveDevelopment(int record) {
  if (record >= 0 && record < kCapacity && g_forced[static_cast<size_t>(record)]) {
    return 0.0f;
  }
  return Development(record);
}

void FillSubjects(uint32_t base, EternalSonataPhoto* out) {
  int filled = 0;
  for (int i = 0; i < ETERNALSONATA_PHOTO_SUBJECT_SLOTS; ++i) {
    const uint32_t group = (i < 3) ? kRecSubjectsA : kRecSubjectsB;
    const uint32_t entry =
        base + group + static_cast<uint32_t>(i % 3) * kRecSubjectStride;

    auto& subject = out->subjects[i];
    subject.id = static_cast<int16_t>(ReadGuest<uint16_t>(entry));
    subject.attribute = static_cast<int16_t>(ReadGuest<uint16_t>(entry + 2));
    for (int v = 0; v < 4; ++v) {
      subject.values[v] = ReadGuest<int32_t>(entry + 4 + static_cast<uint32_t>(v) * 4);
    }
    if (subject.id >= 0) {
      ++filled;
    }
  }
  out->subject_count = filled;
}

// Fills `out` for a record known to be in range. `index` is its display
// position, or -1 when the caller asked by record slot and it is not listed.
void ReadPhoto(int record, int index, EternalSonataPhoto* out) {
  const uint32_t base = RecordAddr(record);
  std::memset(out, 0, sizeof(*out));

  out->index = index;
  out->record = record;

  const uint32_t flags = ReadGuest<uint32_t>(base + kRecFlags);
  out->flags = static_cast<int32_t>(flags);
  out->is_blank = (flags & kFlagBlank) != 0 ? 1 : 0;

  out->score = ReadGuestFloat(base + kRecScore);
  out->score_percent = static_cast<int32_t>(out->score * 100.0f);
  out->rank = out->score > 0.5f    ? ETERNALSONATA_PHOTO_RANK_BEST
              : (out->score > 0.2f ? ETERNALSONATA_PHOTO_RANK_MIDDLE
                                   : ETERNALSONATA_PHOTO_RANK_WORST);

  out->price_scale = ReadGuestFloat(base + kRecPriceScale);
  out->taken_time = ReadGuest<uint32_t>(base + kRecTakenTime);
  out->taken_progress = ReadGuest<int32_t>(base + kRecTakenProgress);

  out->development = EffectiveDevelopment(record);
  out->is_developed = out->development == 0.0f ? 1 : 0;
  out->image_handle = ReadGuest<uint32_t>(base + kRecImageHandle);

  FillSubjects(base, out);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Published on the shared mod registry bus rather than through a callback list
// of our own, so a mod subscribes by name with nothing linked. Called with
// g_mutex NOT held: a subscriber may call straight back into this file from
// its handler.
void PublishPhotoEvent(const char* event_name, int record, int index) {
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
  payload.u64 = static_cast<uint64_t>(record);
  payload.f64 = static_cast<double>(index);
  registry->Publish(event_name, payload);
}

// One photograph was added if the album grew by exactly one and everything
// that was already in it just slid one place towards the back, which is what
// sub_821E6FD8's unshift leaves behind. Anything else - a sale, a trashing, a
// save being loaded, the album being reset - is a resync, not an event.
bool LooksLikeAddition(int count, const std::array<int8_t, kCapacity>& order) {
  if (count != g_snapshot_count + 1 || count > kCapacity) {
    return false;
  }
  for (int i = 0; i < g_snapshot_count; ++i) {
    if (order[static_cast<size_t>(i + 1)] != g_snapshot_order[static_cast<size_t>(i)]) {
      return false;
    }
  }
  return true;
}

// Runs once per guest frame off the mod registry's tick.
void Tick() {
  int added_record = -1;
  std::vector<std::pair<int, int>> developed;  // record, display index

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!AlbumReadable()) {
      // Guest memory went away under us (shutdown); start clean next time.
      g_have_snapshot = false;
      return;
    }

    const int count = PhotoCount();
    std::array<int8_t, kCapacity> order{};
    for (int i = 0; i < kCapacity; ++i) {
      order[static_cast<size_t>(i)] = static_cast<int8_t>(RecordAt(i));
    }

    std::array<bool, kCapacity> done{};
    for (int i = 0; i < count; ++i) {
      const int record = order[static_cast<size_t>(i)];
      if (record >= 0) {
        done[static_cast<size_t>(record)] = EffectiveDevelopment(record) == 0.0f;
      }
    }

    if (g_have_snapshot) {
      if (LooksLikeAddition(count, order)) {
        added_record = order[0];
      }
      // A development that finished while the record stayed in the album. A
      // record that was not listed last time has no previous state to have
      // crossed from, so it is adopted rather than announced.
      for (int i = 0; i < count; ++i) {
        const int record = order[static_cast<size_t>(i)];
        if (record < 0 || record == added_record) {
          continue;
        }
        const auto slot = static_cast<size_t>(record);
        if (done[slot] && !g_snapshot_developed[slot]) {
          developed.emplace_back(record, i);
        }
      }
    }

    g_snapshot_count = count;
    g_snapshot_order = order;
    g_snapshot_developed = done;
    g_have_snapshot = true;
  }

  if (added_record >= 0) {
    PublishPhotoEvent(ETERNALSONATA_PHOTO_EVENT_ADDED, added_record, 0);
  }
  for (const auto& [record, index] : developed) {
    PublishPhotoEvent(ETERNALSONATA_PHOTO_EVENT_DEVELOPED, record, index);
  }
}

// Forces one record, with g_mutex held. Returns false if it was already
// developed, so a caller can report how many photographs it actually changed.
// The developed event is left to the next tick, which sees the transition like
// any other and keeps every event on one thread.
bool ForceDevelopedLocked(int record) {
  if (EffectiveDevelopment(record) == 0.0f) {
    g_forced[static_cast<size_t>(record)] = true;
    return false;
  }

  const uint32_t base = RecordAddr(record);
  // The oldest possible snapshots: taken at clock zero, and at a party
  // progress the party can never catch up to. That drives both terms of the
  // formula as low as they go and, unlike the pin below, is carried by the
  // save. It is not quite zero on its own early in a playthrough, because the
  // elapsed-time term needs clock to have run.
  WriteGuest<uint32_t>(base + kRecTakenTime, 0u);
  WriteGuest<int32_t>(base + kRecTakenProgress, INT32_MAX);
  // The album's cached copy, so a screen that reads it without rebinding the
  // slot agrees.
  WriteGuestFloat(kDevelopmentCacheAddr + static_cast<uint32_t>(record) * 4u, 0.0f);

  g_forced[static_cast<size_t>(record)] = true;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal interface
// ---------------------------------------------------------------------------

void BindPhotoSystem(rex::Runtime* runtime) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_runtime = runtime;
    g_have_snapshot = false;
    g_forced.fill(false);
  }
  if (runtime && runtime->mod_registry()) {
    runtime->mod_registry()->RegisterTick([] { Tick(); });
  }
}

}  // namespace eternalsonata

// ---------------------------------------------------------------------------
// Guest hooks
// ---------------------------------------------------------------------------

// sub_82209478(item, index) -> void: evaluate one photograph's development and
// leave it in flt_8255EE1C[record] and in the album widget's own field. Hooked
// so a photograph EternalSonataDevelopPhoto forced stays forced: the rewritten
// snapshots get the formula close to zero but cannot reach it on a save whose
// clock has barely run, and this is the one place the result is produced.
REX_EXTERN(__imp__sub_82209478);
REX_HOOK_RAW(sub_82209478) {
  const u32 index = ctx.r4.u32;
  __imp__sub_82209478(ctx, base);

  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  const int record = RecordAt(static_cast<int>(index));
  if (record < 0 || !g_forced[static_cast<size_t>(record)]) {
    return;
  }
  WriteGuestFloat(kDevelopmentCacheAddr + static_cast<uint32_t>(record) * 4u, 0.0f);
  // The widget field the album draws the haze from, written by the original
  // right before it returned. ctx.r3 still holds the widget.
  WriteGuestFloat(ctx.r3.u32 + 168u, 0.0f);
}

// sub_82240AF8(save) -> void: restore the live globals from a loaded save,
// album included. The album that comes back is whatever the save held, so the
// poll adopts it instead of reporting a dozen additions.
REX_EXTERN(__imp__sub_82240AF8);
REX_HOOK_RAW(sub_82240AF8) {
  __imp__sub_82240AF8(ctx, base);

  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_have_snapshot = false;
  g_forced.fill(false);
}

// ---------------------------------------------------------------------------
// Public C ABI (eternalsonata_photo_api.h)
// ---------------------------------------------------------------------------

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t EternalSonataPhotoAbiVersion(void) {
  return ETERNALSONATA_PHOTO_ABI_VERSION;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataIsPhotoAlbumAvailable(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  return AlbumReadable() ? 1 : 0;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPhotoCount(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return ETERNALSONATA_PHOTO_ERR_UNAVAILABLE;
  }
  return PhotoCount();
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPhotoCapacity(void) {
  return ETERNALSONATA_PHOTO_CAPACITY;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPhoto(int index,
                                                           EternalSonataPhoto* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_PHOTO_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return ETERNALSONATA_PHOTO_ERR_UNAVAILABLE;
  }
  if (index < 0 || index >= PhotoCount()) {
    return ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO;
  }
  const int record = RecordAt(index);
  if (record < 0) {
    return ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO;
  }
  ReadPhoto(record, index, out);
  return ETERNALSONATA_PHOTO_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPhotoByRecord(
    int record, EternalSonataPhoto* out) {
  using namespace eternalsonata;
  if (!out) {
    return ETERNALSONATA_PHOTO_ERR_INVALID_ARGUMENT;
  }
  if (record < 0 || record >= ETERNALSONATA_PHOTO_CAPACITY) {
    return ETERNALSONATA_PHOTO_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return ETERNALSONATA_PHOTO_ERR_UNAVAILABLE;
  }
  const int index = IndexOfRecord(record);
  if (index < 0) {
    return ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO;
  }
  ReadPhoto(record, index, out);
  return ETERNALSONATA_PHOTO_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataGetPhotos(EternalSonataPhoto* out,
                                                            int max) {
  using namespace eternalsonata;
  if (max < 0 || (max > 0 && !out)) {
    return ETERNALSONATA_PHOTO_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return ETERNALSONATA_PHOTO_ERR_UNAVAILABLE;
  }
  const int count = PhotoCount();
  if (max == 0) {
    return count;
  }
  int written = 0;
  for (int i = 0; i < count && written < max; ++i) {
    const int record = RecordAt(i);
    if (record < 0) {
      continue;
    }
    ReadPhoto(record, i, &out[written]);
    ++written;
  }
  return written;
}

extern "C" REX_MOD_PLUGIN_EXPORT float EternalSonataGetPhotoDevelopment(int index) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return static_cast<float>(ETERNALSONATA_PHOTO_ERR_UNAVAILABLE);
  }
  if (index < 0 || index >= PhotoCount()) {
    return static_cast<float>(ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO);
  }
  const int record = RecordAt(index);
  if (record < 0) {
    return static_cast<float>(ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO);
  }
  return EffectiveDevelopment(record);
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataDevelopPhoto(int index) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return ETERNALSONATA_PHOTO_ERR_UNAVAILABLE;
  }
  if (index < 0 || index >= PhotoCount()) {
    return ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO;
  }
  const int record = RecordAt(index);
  if (record < 0) {
    return ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO;
  }
  ForceDevelopedLocked(record);
  return ETERNALSONATA_PHOTO_OK;
}

extern "C" REX_MOD_PLUGIN_EXPORT int EternalSonataDevelopAllPhotos(void) {
  using namespace eternalsonata;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!AlbumReadable()) {
    return ETERNALSONATA_PHOTO_ERR_UNAVAILABLE;
  }
  int changed = 0;
  const int count = PhotoCount();
  for (int i = 0; i < count; ++i) {
    const int record = RecordAt(i);
    if (record >= 0 && ForceDevelopedLocked(record)) {
      ++changed;
    }
  }
  return changed;
}
