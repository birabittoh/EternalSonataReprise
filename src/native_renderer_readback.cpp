// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_readback.h.

#include "native_renderer_readback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#ifdef _WIN32
#include <windows.h>
#endif

// Whether this platform can answer a guest read of a resolve destination with a
// page fault. It needs two things: a way to make a page inaccessible (portable,
// rex::memory::Protect) and a fault handler that can say whether the access that
// trapped was a read or a write. Filling a buffer the guest is in the middle of
// *writing* destroys what it wrote, so a handler that cannot tell the two apart
// is worse than no handler.
//
// Both platforms install *in front of* the SDK's own handler, and that ordering
// is load-bearing rather than incidental.
//
// The memory system watches guest physical pages by protecting them and catching
// the fault (MMIOHandler). Its handler, for any address inside the guest mapping
// that is not an MMIO range, reads the current protection and -- on kNoAccess --
// falls through into its write-watch callback (mmio_handler.cpp, "recheck if the
// pages are still protected"). It cannot tell one of our armed pages from one of
// its own watches, so whichever handler runs first owns the fault.
//
// Windows gets this for free: AddVectoredExceptionHandler(1, ...) puts this at
// the head of the chain, ahead of the SDK's. On Linux, arch::ExceptionHandler
// appends and the memory system installs at startup, so registering there puts
// this last -- the memory system claimed every fault, this handler was never
// reached (one fault in a whole run against 2829 arms), and its watch state was
// being driven by pages it never watched. Hence a sigaction of our own, which
// forwards anything that is not ours to whatever was installed before it.
//
// Reading the fault as a read or a write is what confines this to x86-64: the
// bit lives in the page-fault error code at REG_ERR. Filling a buffer the guest
// is mid-write into destroys what it wrote, so a platform that cannot tell the
// two apart must not arm. Android also stays on the eager fill for that reason.
#if defined(_WIN32)
#define ETERNALSONATA_READBACK_PAGE_TRAP 1
#elif defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)
#define ETERNALSONATA_READBACK_PAGE_TRAP 1
#include <signal.h>
#include <ucontext.h>
#endif

#include "native_renderer_frame.h"
#include "native_renderer_plume.h"
#include "native_renderer_profile.h"
#include "native_renderer_texture.h"

// How a resolved render target gets back into guest memory.
//
//  auto  - the default. Guest memory is filled in on demand: the destination's
//          pages are left inaccessible and the guest's own read faults into a
//          handler that writes the pixels and lets the read through. A
//          destination nothing reads costs nothing beyond arming it.
//  eager - fill every destination in every frame it is resolved, with no page
//          protection anywhere. What "auto" degrades to if the trap turns out to
//          be the wrong instrument on some machine, and the thing to compare
//          against when a readback looks stale or missing.
//  off   - never write guest memory, which is what this renderer did before.
REXCVAR_DEFINE_STRING(native_readback_resolve, "auto", "Eternal Sonata",
                      "Native renderer: how a resolved render target is written back into guest "
                      "memory for the game's own CPU to read (auto, eager, off)")
    .allowed({"auto", "eager", "off"});

namespace eternalsonata {
namespace {

// Guest pages are armed at host page granularity, so the tail of an extent
// shares a page with whatever the guest put after it. A fault there is not
// evidence about this destination, and only the extent itself counts.
// The host page size, which is what a protection call rounds to. 4096 on every
// platform this has run on, but asked for rather than assumed: arming a range
// rounded to the wrong granularity either leaves the first page of a destination
// unprotected or protects memory in front of it.
uintptr_t GuestPageBytes() {
  const uintptr_t reported = uintptr_t(rex::memory::page_size());
  return reported != 0 ? reported : 4096u;
}

// The apertures guest physical memory is reachable through. All three alias the
// same physical pages, but they are three separate host mappings: protecting one
// does not protect the others, and guest code could be reading through any of
// them. Writing through one is enough, though, which is why the fill only needs
// a single pointer. See GuestPhysicalPointer in native_renderer_texture.cpp.
constexpr uint32_t kApertures[] = {0xA0000000u, 0xC0000000u, 0xE0000000u};
constexpr uint32_t kApertureCount = 3;

// A bound on what will be believed out of a destination's own fetch constant,
// matching the texture mirror's.
constexpr uint64_t kMaxDestinationBytes = 64ull * 1024 * 1024;

enum class Mode { kOff, kAuto, kEager };

Mode CurrentMode() {
  const std::string& value = REXCVAR_GET(native_readback_resolve);
  if (value == "off")
    return Mode::kOff;
  if (value == "eager")
    return Mode::kEager;
  return Mode::kAuto;
}

// Where inside an aperture a destination's bytes actually are.
//
// The **fixed up** address, not the raw field, and this is the difference
// between a correct readback and one that is off by exactly one 4 KB page.
//
// A resolve destination is decoded from the guest's own resource object, whose
// base field has not been through the address fixup yet: it reads as something
// like 0xE5FE7000, and the fixup takes that to 0x05FE8000 by adding a page for
// the 0xE0000000 aperture. Everything that later reads those pixels -- the
// guest, and the texture mirror through the device's fetch constant, which
// SetTexture has already fixed up -- uses the fixed address. Writing at the raw
// one puts the image a page early: at 1280 wide that is 0.8 of a row and looks
// like a horizontal translation, at 320 wide it is 3.2 rows and looks like a
// vertical one with a black strip left at the bottom, and either way the page
// that falls off the front lands on whatever guest allocation sits below, which
// is unrelated textures being corrupted.
//
// The texture mirror's GuestPhysicalPointer takes the raw field instead, and is
// right to: it is handed the *device's* fetch, which is already fixed, and
// applying the fixup twice is a no-op only because the result is masked below
// 0x20000000. Here the fetch comes from the resource, so the fixup has to be
// respected.
uint32_t GuestOffset(const TextureFetch& fetch) { return fetch.base_address & 0x1FFFFFFFu; }

// One host mapping of a destination's guest bytes, page aligned outward.
struct ArmedRange {
  uint8_t* page_base = nullptr;
  size_t page_bytes = 0;
  uint8_t* start = nullptr;  // the destination's own first byte in this mapping
  uint64_t bytes = 0;
  rex::memory::PageAccess old_access = rex::memory::PageAccess::kReadWrite;
  bool armed = false;
};

struct Destination {
  uint32_t address = 0;  // the fixed up base address, the key used everywhere else
  TextureFetch fetch;
  uint8_t* memory_base = nullptr;

  // Where the destination lives in one of the apertures, and how much of it
  // there is. Chosen fresh on each publish, because which apertures are
  // committed is a property of the guest's memory map rather than of ours.
  uint8_t* guest = nullptr;
  // Which of the three apertures `guest` was taken from. Kept only to be logged:
  // the base address the fill uses has been through the address fixup, which adds
  // a page for the 0xE0000000 aperture, so writing it through a different one is
  // a page of skew (see the comment on out.raw_base_address in
  // native_renderer_d3d.cpp). That has never been observed here and the fills are
  // pixel-exact for the thumbnails, but it costs nothing to be able to see it.
  uint32_t aperture = 0;
  uint64_t extent = 0;
  uint32_t texel_bytes = 0;

  // The mapped readback buffer and its row pitch, owned by the frame layer and
  // valid until ReadbackForget.
  const uint8_t* pixels = nullptr;
  uint32_t row_bytes = 0;
  uint32_t pixel_rows = 0;

  ArmedRange ranges[kApertureCount];
  uint32_t range_count = 0;

  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t fills = 0;
  uint64_t publishes = 0;
  uint64_t pulls = 0;

  // The frame a resolve last wrote this destination. Memory stops being a
  // render target: the guest frees the buffer and loads an ordinary texture
  // into it, and filling that with the render target's old pixels is a texture
  // corrupting itself for no visible reason. See Fresh().
  uint64_t last_publish_frame = 0;

  // The frame the last copy into the readback buffer was recorded in.
  //
  // That copy has completed once the frame is over, because the present waits on
  // its fence: any frame after this one can read the buffer with no
  // synchronisation. An earlier version asked instead whether a *later* resolve
  // had published the destination, which is only true of surfaces resolved every
  // frame; a save thumbnail is resolved exactly once and read seconds later, so
  // it looked permanently unavailable and was never filled at all.
  uint64_t copy_frame = ~0ull;

  // Set when a flush has forced that copy through inside its own frame.
  bool flushed = false;

  // The guest has written these pages itself since the last resolve, so it owns
  // what is in them now. Filling over that is a texture corrupting itself: guest
  // memory does not stay a render target, and a freed buffer gets an ordinary
  // texture loaded into it.
  bool guest_owns = false;

  // Whether this destination has already been filled since the last publish,
  // which is once a frame. Both triggers set it: there is no point filling the
  // same bytes twice from the same readback.
  bool filled_since_publish = false;
  bool refused = false;
  // Reported once: this destination is too large for the page trap.
  bool arming_refused = false;
  bool layout_reported = false;
};

// How often an armed destination is taken apart and set up again even though
// nothing about it appears to have changed.
//
// Arming is the only thing this costs a frame that never reads anything back,
// and it is not free: a VirtualProtect over a 720p destination covers 920 pages.
// So an armed destination that is still described the same way and still backed
// by the same buffer is left armed rather than disarmed and rearmed, which takes
// the steady state to no system calls at all. The periodic rebuild is what
// catches the guest unmapping or recommitting the memory underneath, which the
// skipped path would otherwise never look at again.
constexpr uint64_t kRearmPeriod = 64;

// The largest destination whose guest pages are worth trapping on.
//
// The trap answers a read by writing the destination's *whole extent*, however
// long ago the resolve was and whatever the guest has done with the memory since,
// and the fault itself carries no evidence either way: it says some guest code
// touched an address, not that it still thinks that address holds this image.
// The damage from getting that wrong scales with the extent, and the two things
// this title actually reads back with its own CPU are small: the save thumbnail
// at 320x160 (204,800 bytes) and the in-game photo at 416x256 (425,984).
//
// Three crashes with an identical shape came from the other end of the range. A
// 1280x720 destination resolved during the boot sequence, armed and untouched for
// eighteen hundred frames, then read once: the fill scattered 3,768,320 bytes of
// an ancient loading screen over whatever the guest had put there since, which
// every time included the XMA context block. The guest's next XMAReleaseContext
// indexed XmaDecoder::contexts_ with a zeroed pointer and dereferenced a null.
//
// Nothing cheaper distinguishes those two. Age does not: the thumbnail is
// resolved once and read 251 frames later, sometimes 649, which is the same order
// as the crashing reads and is why a lifetime on the arming turned every new save
// thumbnail black. `guest_owns` does not either -- it is set from a write fault,
// and all three crashing runs report `faults read=1 write=0`, so nothing ever
// wrote through an armed aperture to give it a signal.
//
// A large destination is still tracked, still copied into its readback buffer and
// still filled through ReadbackFillForRead when the texture mirror binds it,
// because that path compares the layout the destination was written as against
// the layout it is being read as and refuses on disagreement. It just no longer
// has a page trap standing over three megabytes of guest memory waiting to
// overwrite it on the strength of one read.
constexpr uint64_t kMaxArmedExtentBytes = 1024 * 1024;

// Does this publish describe the same destination, in the same place, as the
// one that armed it?
bool SameAsArmed(const Destination& destination, const TextureFetch& dest, const uint8_t* pixels,
                 uint32_t row_bytes) {
  const TextureFetch& previous = destination.fetch;
  return destination.range_count != 0 && destination.pixels == pixels &&
         destination.row_bytes == row_bytes && previous.base_address == dest.base_address &&
         previous.format == dest.format && previous.width == dest.width &&
         previous.height == dest.height && previous.pitch == dest.pitch &&
         previous.tiled == dest.tiled && previous.endianness == dest.endianness;
}

// Taken by the publish (guest thread) and by the fault handler (any thread that
// touches a destination's pages). Nothing under it can fault, so the handler
// cannot deadlock against a holder of it: the fill runs only after the pages it
// writes have been made writable again.
std::mutex g_mutex;
std::vector<Destination> g_destinations;

uint64_t g_arms = 0;
uint64_t g_read_faults = 0;
uint64_t g_write_faults = 0;
uint64_t g_outside_faults = 0;
uint64_t g_pulls = 0;
uint64_t g_layout_disagreements = 0;
uint64_t g_flushes = 0;
uint64_t g_refused_stale = 0;
uint64_t g_refused_large = 0;
uint64_t g_disowned_faults = 0;
// `unanswered` splits two ways that need opposite fixes: a fault on a thread
// that cannot flush (the recording belongs to another thread), and a flush that
// was attempted and failed. Counted apart so the summary says which.
uint64_t g_unanswered_thread = 0;
uint64_t g_unanswered_flush = 0;
// Reads answered by waiting for the recording thread's present rather than by
// flushing. See WaitForCopy.
uint64_t g_waited_copies = 0;
// Fills that had to skip part of their extent because the mirror holds another
// texture over it, and how many bytes went unwritten that way.
uint64_t g_clipped_fills = 0;
uint64_t g_clipped_bytes = 0;
uint32_t g_clipped_reported = 0;
constexpr uint32_t kMaxClippedReports = 8;

// The most recent frame any resolve was published in, i.e. what "now" means to
// a fault, which arrives from guest code rather than from the frame loop.
uint64_t g_now = 0;
uint64_t g_unanswered = 0;

// The thread the frame is recorded on, learned from the publish. A flush
// submits that recording, so it can only be asked for from there; a fault on
// any other thread has to be answered with whatever is already in hand.
// A thread-local flag rather than a thread id: the only question ever asked of
// it is "am I the thread that records the frame", and every platform answers
// that the same way. The id form needed GetCurrentThreadId().
thread_local bool g_is_render_thread = false;
uint64_t g_fills = 0;
uint64_t g_fill_bytes = 0;
uint64_t g_refused_format = 0;
uint64_t g_refused_extent = 0;
uint64_t g_refused_unmapped = 0;
uint32_t g_logged = 0;
uint32_t g_content_reported = 0;

// The fill writes to guest memory this handler has just made writable, so by
// construction it cannot fault; if it ever does, running the handler on top of
// itself would deadlock on the mutex it already holds.
thread_local bool g_in_handler = false;

// ---------------------------------------------------------------------------
// The guest side of a destination

// Bytes per texel, for what a resolve destination is actually created in.
//
// The host image this is filled from is one of the frame layer's colour targets,
// which are B8G8R8A8_UNORM, so k_8_8_8_8 is a straight scatter and anything else
// costs a conversion on the way down. k_5_6_5 and k_1_5_5_5 are worth that
// conversion because the in-game photo renders into one of them; anything else
// is counted and left alone, which leaves guest memory holding whatever it held
// before rather than something wrong.
bool DestinationTexelBytes(uint32_t format, uint32_t& out) {
  switch (format) {
    case 3:  // k_1_5_5_5
    case 4:  // k_5_6_5
      out = 2;
      return true;
    case 6:  // k_8_8_8_8
      out = 4;
      return true;
    default:
      return false;
  }
}

// Pack one row of the host's B8G8R8A8 down into the 16 bit guest format, the
// inverse of the texture mirror's ExpandRows. Done before the endian swap, which
// works on the packed unit.
void PackRow(const uint8_t* source, uint32_t texels, uint32_t format, uint8_t* out) {
  for (uint32_t x = 0; x < texels; ++x) {
    const uint8_t b = source[x * 4 + 0];
    const uint8_t g = source[x * 4 + 1];
    const uint8_t r = source[x * 4 + 2];
    const uint8_t a = source[x * 4 + 3];
    uint16_t value = 0;
    if (format == 4) {  // k_5_6_5
      value = uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    } else {  // k_1_5_5_5
      value = uint16_t((a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    }
    std::memcpy(out + size_t(x) * 2, &value, 2);
  }
}

// The inverse of the texture mirror's EndianSwapUnit, which is the same
// function: every case here is its own inverse.
void EndianSwapUnit(uint8_t* data, uint32_t bytes, uint32_t endianness) {
  switch (endianness) {
    case 1:  // k_8in16
      for (uint32_t i = 0; i + 1 < bytes; i += 2)
        std::swap(data[i], data[i + 1]);
      break;
    case 2:  // k_8in32
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case 3:  // k_16in32
      for (uint32_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:  // k_none
      break;
  }
}

// The Xenos 2D tiled address, in units of one addressable block. The same two
// functions the texture mirror untiles with (native_renderer_texture.cpp), used
// the other way round: that one walks the linear image and gathers, this one
// walks it and scatters.
uint32_t TiledCombine(uint32_t oib, uint32_t bank, uint32_t pipe, uint32_t y_lsb) {
  return (y_lsb << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}

uint32_t TiledBlockOffset(uint32_t bx, uint32_t by, uint32_t pitch_macro_tiles,
                          uint32_t block_bytes_log2) {
  const uint32_t outer = ((by >> 5) * pitch_macro_tiles + (bx >> 5)) << 6;
  const uint32_t inner = (((by >> 1) & 7) << 3) | (bx & 7);
  const uint32_t oib = (outer | inner) << block_bytes_log2;
  const uint32_t bank = (by >> 4) & 1;
  const uint32_t pipe = ((bx >> 3) & 3) ^ (((by >> 3) & 1) << 1);
  return TiledCombine(oib, bank, pipe, by & 1);
}

uint32_t Log2Exact(uint32_t value) {
  uint32_t bits = 0;
  while ((1u << bits) < value)
    ++bits;
  return bits;
}

uint32_t PitchTexels(const TextureFetch& fetch) {
  return fetch.pitch > fetch.width ? fetch.pitch : fetch.width;
}

// How much guest memory the destination occupies. Tiled and linear disagree:
// the tiled swizzle's highest offset is not the last texel of the last row, so
// it is bounded by the whole macro tile grid instead. Same reasoning and the
// same numbers as SourceExtentBytes in the texture mirror.
uint64_t ExtentBytes(const TextureFetch& fetch, uint32_t texel_bytes) {
  const uint32_t pitch_texels = PitchTexels(fetch);
  if (fetch.tiled) {
    const uint32_t pitch_macro_tiles = pitch_texels / 32 > 1 ? pitch_texels / 32 : 1;
    const uint32_t macro_rows = (fetch.height + 31) / 32;
    return uint64_t(pitch_macro_tiles) * macro_rows * 32 * 32 * texel_bytes;
  }
  return uint64_t(pitch_texels) * texel_bytes * fetch.height;
}

// Is the whole of [start, start + bytes) committed and writable? Asked before
// anything is armed, so it never sees our own PAGE_NOACCESS.
bool RangeWritable(const uint8_t* start, uint64_t bytes) {
#ifdef _WIN32
  const uint8_t* cursor = start;
  const uint8_t* end = start + bytes;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(cursor, &info, sizeof(info)) == 0)
      return false;
    if (info.State != MEM_COMMIT)
      return false;
    const DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & writable) == 0 || (info.Protect & PAGE_GUARD) != 0)
      return false;
    cursor = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  // Off Windows the same question goes through the SDK's portable page query.
  // This used to be a bare `return false`, which made every destination look
  // unmapped: no aperture was ever picked, `destination->guest` stayed null and
  // nothing was ever written back. On Android that is not a cosmetic loss. The
  // game resolves a 64x64 surface at startup and then polls the guest memory
  // behind it for the result, so a readback that never lands is an infinite
  // spin before the first frame, which is the boot hang.
  const uint8_t* cursor = start;
  const uint8_t* end = start + bytes;
  const uintptr_t page_bytes = uintptr_t(rex::memory::page_size());
  if (page_bytes == 0)
    return false;
  while (cursor < end) {
    size_t region_bytes = 0;
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    if (!rex::memory::QueryProtect(const_cast<uint8_t*>(cursor), region_bytes, access))
      return false;
    if (access != rex::memory::PageAccess::kReadWrite &&
        access != rex::memory::PageAccess::kExecuteReadWrite) {
      return false;
    }
    // The reported run is measured from the first byte of the page the query
    // landed in, not from the query address, so advance from the page base.
    const uint8_t* page = reinterpret_cast<const uint8_t*>(
        reinterpret_cast<uintptr_t>(cursor) & ~(page_bytes - 1));
    const uint8_t* next = page + region_bytes;
    if (next <= cursor)  // No forward progress; refuse rather than spin here.
      return false;
    cursor = next;
  }
  return true;
#endif
}

// ---------------------------------------------------------------------------
// Writing guest memory

// Lay the host image out the way the guest's own fetch constant describes it.
// Called with the destination's pages accessible.
// Write the parts of [offset, offset + bytes) that no cached texture owns.
//
// `occupied` is sorted and non-overlapping, so this walks it once. Empty is the
// normal case and costs one comparison.
void WriteClipped(uint8_t* guest, const std::vector<MirrorOccupiedRange>& occupied,
                  uint64_t offset, const uint8_t* source, uint64_t bytes, uint64_t* skipped) {
  uint64_t cursor = offset;
  const uint64_t end = offset + bytes;
  for (const MirrorOccupiedRange& range : occupied) {
    if (range.end <= cursor)
      continue;
    if (range.begin >= end)
      break;
    if (range.begin > cursor) {
      const uint64_t run = range.begin - cursor;
      std::memcpy(guest + cursor, source + (cursor - offset), size_t(run));
    }
    *skipped += (range.end < end ? range.end : end) - (range.begin > cursor ? range.begin : cursor);
    cursor = range.end;
    if (cursor >= end)
      return;
  }
  if (cursor < end)
    std::memcpy(guest + cursor, source + (cursor - offset), size_t(end - cursor));
}

bool Fill(Destination& destination) {
  if (destination.pixels == nullptr || destination.guest == nullptr)
    return false;

  // Measured again, here, and not because guest memory is untrusted in general.
  // A destination is validated when it is resolved and written when something
  // reads it, and those can be hundreds of frames apart: a save thumbnail is
  // resolved once and read four seconds later. The guest is free to have freed
  // that buffer in between, and writing megabytes into what is now somebody
  // else's allocation -- or into nothing at all -- is a corrupted texture at
  // best and an access violation inside the fault handler at worst, which is
  // unrecoverable because the handler cannot fault on top of itself.
  if (!RangeWritable(destination.guest, destination.extent)) {
    ++g_refused_unmapped;
    return false;
  }

  const TextureFetch& fetch = destination.fetch;

  // Which of these pages belong to somebody else by now.
  //
  // The guest frees a render target and streams an ordinary asset into the same
  // pages; nothing arms a destination once it has been filled, so `Fresh` never
  // hears about it. Refusing the whole fill on any overlap made save previews
  // black, and filling regardless corrupted textures -- both are the fill being
  // all-or-nothing over a range it does not need all of, so the overlap is cut
  // out and the rest is written.
  static thread_local std::vector<MirrorOccupiedRange> occupied;
  occupied.clear();
  TextureMirrorOccupiedRanges(destination.address, destination.extent, destination.address,
                              fetch.width, fetch.height, &occupied);
  uint64_t skipped = 0;
  const uint32_t texel_bytes = destination.texel_bytes;
  // Clamped to the buffer in both directions, texels as well as bytes: the loop
  // below indexes the scratch row by texel, so a width the buffer cannot back
  // has to shorten the walk and not just the copy.
  // Clamped against the *readback buffer's* pitch, which is always four bytes a
  // texel whatever the guest's format is.
  constexpr uint32_t kHostTexelBytes = 4;
  uint32_t row_texels = fetch.width;
  if (row_texels * kHostTexelBytes > destination.row_bytes)
    row_texels = destination.row_bytes / kHostTexelBytes;
  const uint32_t row_source_bytes = row_texels * texel_bytes;
  const bool packing = texel_bytes != kHostTexelBytes;

  // One row at a time through a scratch buffer, because the endian swap is in
  // place and the readback buffer is the GPU's, not ours to modify.
  static thread_local std::vector<uint8_t> row;
  row.resize(row_source_bytes);

  const uint32_t pitch_texels = PitchTexels(fetch);
  const uint32_t pitch_macro_tiles = pitch_texels / 32 > 1 ? pitch_texels / 32 : 1;
  const uint32_t texel_bytes_log2 = Log2Exact(texel_bytes);

  // What is actually in the buffer about to be handed to the guest.
  //
  // This is the fork the counters cannot show. A black save preview is either
  // the GPU having read back a black image -- in which case nothing below this
  // line is at fault and the resolve or its source is -- or a good image laid
  // out wrongly on the way into guest memory. Reported per fill, capped, and
  // only for the small destinations the thumbnails use.
  if (fetch.width <= 512 && g_content_reported < 16) {
    ++g_content_reported;
    const uint32_t sample_rows = fetch.height < destination.pixel_rows ? fetch.height
                                                                       : destination.pixel_rows;
    uint64_t dark = 0;
    uint64_t counted = 0;
    uint64_t sum = 0;
    for (uint32_t y = 0; y < sample_rows; ++y) {
      const uint8_t* host_row = destination.pixels + size_t(y) * destination.row_bytes;
      for (uint32_t x = 0; x < row_texels; ++x) {
        const uint32_t luma =
            uint32_t(host_row[x * 4 + 0]) + host_row[x * 4 + 1] + host_row[x * 4 + 2];
        sum += luma;
        if (luma < 24)
          ++dark;
        ++counted;
      }
    }
    REXLOG_WARN(
        "native_renderer: readback content 0x{:08X} {}x{} at frame {} | resolved in frame {} | "
        "{}% of {} texel(s) near black | mean {} | flushed {}",
        destination.address, fetch.width, fetch.height, FrameIndex(),
        destination.last_publish_frame, counted != 0 ? dark * 100 / counted : 0, counted,
        counted != 0 ? sum / (counted * 3) : 0, destination.flushed ? 1 : 0);

    // And the image itself, because "mean 36" does not distinguish a dim
    // photograph from a flat fill, and that is the whole remaining question.
    // Raw BGRA at the buffer's own pitch; scripts/readback_png.py turns it into
    // something that can be looked at.
    char path[128];
    std::snprintf(path, sizeof(path), "logs/readback_%08X_%ux%u_p%u_f%llu.bin",
                  destination.address, fetch.width, sample_rows, destination.row_bytes,
                  (unsigned long long)FrameIndex());
    if (FILE* file = std::fopen(path, "wb")) {
      std::fwrite(destination.pixels, 1, size_t(destination.row_bytes) * sample_rows, file);
      std::fclose(file);
    }
  }

  // Bounded by the buffer as well as by the image. The two disagree when the
  // guest resolves a taller image to an address that already has a destination:
  // the host texture and its readback buffer keep the extent they were created
  // with, and reading rows the buffer does not have is a fault inside the fault
  // handler, which cannot be recovered from.
  const uint32_t rows = fetch.height < destination.pixel_rows ? fetch.height
                                                              : destination.pixel_rows;
  for (uint32_t y = 0; y < rows; ++y) {
    const uint8_t* host_row = destination.pixels + size_t(y) * destination.row_bytes;
    if (packing)
      PackRow(host_row, row_texels, fetch.format, row.data());
    else
      std::memcpy(row.data(), host_row, row_source_bytes);
    EndianSwapUnit(row.data(), row_source_bytes, fetch.endianness);

    if (fetch.tiled) {
      // Four texels at a time where the run is aligned to four.
      //
      // The tiled address is not a permutation of individual texels: with a four
      // byte texel, x's low two bits land in the low two bits of the offset, so
      // texels 4n..4n+3 of a row occupy sixteen contiguous bytes. Walking one
      // texel at a time was a memcpy of four bytes per pixel, which for a 720p
      // destination is 920,000 of them.
      const uint32_t aligned_texels = (texel_bytes == 4) ? (row_texels & ~3u) : 0;
      uint32_t x = 0;
      for (; x < aligned_texels; x += 4) {
        const uint32_t offset = TiledBlockOffset(x, y, pitch_macro_tiles, texel_bytes_log2);
        if (uint64_t(offset) + 16 > destination.extent)
          continue;
        WriteClipped(destination.guest, occupied, offset, row.data() + size_t(x) * texel_bytes, 16,
                     &skipped);
      }
      for (; x < row_texels; ++x) {
        const uint32_t offset = TiledBlockOffset(x, y, pitch_macro_tiles, texel_bytes_log2);
        if (uint64_t(offset) + texel_bytes > destination.extent)
          continue;
        WriteClipped(destination.guest, occupied, offset, row.data() + size_t(x) * texel_bytes,
                     texel_bytes, &skipped);
      }
    } else {
      const uint64_t offset = uint64_t(y) * pitch_texels * texel_bytes;
      if (offset + row_source_bytes > destination.extent)
        break;
      WriteClipped(destination.guest, occupied, offset, row.data(), row_source_bytes, &skipped);
    }
  }

  destination.filled_since_publish = true;
  ++destination.fills;
  ++g_fills;
  g_fill_bytes += uint64_t(row_source_bytes) * rows;
  if (skipped != 0) {
    ++g_clipped_fills;
    g_clipped_bytes += skipped;
    if (g_clipped_reported < kMaxClippedReports) {
      ++g_clipped_reported;
      REXLOG_WARN(
          "native_renderer: readback clipped the fill of 0x{:08X} {}x{}: {} of {} guest byte(s) "
          "belong to {} cached texture(s) now, the first at 0x{:08X}",
          destination.address, fetch.width, fetch.height, skipped, destination.extent,
          occupied.size(), uint32_t(destination.address + occupied.front().begin));
    }
  }
  return true;
}

// Is this destination still a render target?
//
// Guest memory does not stay one. The game frees a resolve destination and
// loads an ordinary texture into the same buffer, and an entry here lives
// forever, so without this a read of *the texture* is answered by filling it
// with the render target's old pixels: a texture that corrupts itself for no
// visible reason, some frames after anything last resolved. The frame layer
// makes the same argument about extents in FrameResolveTextureByAddress.
//
// The rule is that only memory the GPU has just written is written again, which
// is the narrowest defensible window: the frame that resolved it, or the one
// before, since a fault arrives from guest code rather than from the frame loop
// and can land either side of a present.
bool Fresh(const Destination& destination) { return !destination.guest_owns; }

// Has the copy recorded for this destination actually run?
//
// This used to be "we are in a later frame than the one that recorded it",
// which was true only because the present waited on its own fence: being in the
// next frame meant the previous one had finished. With frames in flight it does
// not, so the question is asked of the ring directly. Getting this wrong would
// hand the guest a buffer the GPU is still writing.
bool DataReady(const Destination& destination) {
  return destination.flushed ||
         (destination.copy_frame != ~0ull && PlumeFrameRetired(destination.copy_frame));
}

// One line per event, capped, because the ordering is the thing that cannot be
// read off the counters: whether the guest's read of a destination came before
// or after the resolve that was supposed to fill it decides whether a black
// first screenshot is a bug here or the game reading a buffer nothing has
// produced yet.
uint32_t g_events_logged = 0;
constexpr uint32_t kMaxLoggedEvents = 48;

void LogEvent(const Destination& destination, const char* what, bool filled) {
  if (g_events_logged >= kMaxLoggedEvents)
    return;
  ++g_events_logged;
  REXLOG_WARN(
      "native_renderer: readback event {} on 0x{:08X} {}x{} at frame {} | resolved in frame {} | "
      "has data {} | filled {}",
      what, destination.address, destination.fetch.width, destination.fetch.height, FrameIndex(),
      destination.last_publish_frame, DataReady(destination) ? 1 : 0, filled ? 1 : 0);
}

// Make sure the readback buffer holds a completed image, stalling the GPU if
// that is what it takes.
//
// The stall is the whole point rather than a wart: a destination that is
// resolved and read in the same frame cannot be answered from a buffer the GPU
// has not written yet, and answering it with the previous frame's image is what
// a save screenshot would show as stale or, on the first one, as nothing. This
// is the SDK's `readback_resolve=full` behaviour, reached only when a read
// actually needs it.
// How long a guest thread that is not recording the frame will wait for the
// copy it needs to retire on its own. Two or three frames at 60 Hz: long enough
// to cover a present that is already in flight, short enough that a render
// thread which is itself blocked behind this one degrades to the old black
// image rather than to a hang.
constexpr auto kCopyWaitTimeout = std::chrono::milliseconds(50);
constexpr auto kCopyWaitPoll = std::chrono::milliseconds(1);

// Wait for a recorded copy to run, without submitting anything.
//
// `lock` is released for the duration, which is the whole point: the render
// thread takes the same mutex in ReadbackPublish, so holding it here would stop
// the very present that retires the frame being waited on. That also means
// `destination` does not survive the call -- g_destinations is a vector and the
// render thread can grow it -- so this takes the address and the caller re-finds.
bool WaitForCopy(uint32_t address, uint64_t copy_frame, std::unique_lock<std::mutex>& lock) {
  if (copy_frame == ~0ull)
    return false;
  const auto deadline = std::chrono::steady_clock::now() + kCopyWaitTimeout;
  lock.unlock();
  bool retired = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (PlumeFrameRetired(copy_frame)) {
      retired = true;
      break;
    }
    std::this_thread::sleep_for(kCopyWaitPoll);
  }
  lock.lock();
  (void)address;
  return retired;
}

// Defined below; needed here because a destination reference does not survive
// EnsureData releasing the lock.
Destination* Find(uint32_t address);

bool EnsureData(Destination& destination, std::unique_lock<std::mutex>& lock) {
  if (DataReady(destination))
    return true;

  if (!g_is_render_thread) {
    // Another thread got there first, so a flush is not available: submitting
    // means submitting a command list the render thread is still writing.
    //
    // Waiting is, though, and it is what this case actually needs. The save
    // screenshot is resolved and read in the same frame, from the guest's save
    // thread rather than from the thread recording the frame, so the copy has
    // been recorded and simply has not run yet. Giving up here is what made the
    // *first* save after boot come back black while every later one worked --
    // a later one is answered from DataReady above, because its copy ran frames
    // ago.
    //
    // So wait for the render thread to present, which retires the frame and
    // completes the copy with no extra synchronisation. The caller re-finds the
    // destination afterwards; this releases the mutex.
    const uint32_t address = destination.address;
    const uint64_t copy_frame = destination.copy_frame;
    if (WaitForCopy(address, copy_frame, lock)) {
      ++g_waited_copies;
      return true;
    }
    ++g_unanswered_thread;
    ++g_unanswered;
    return false;
  }
  if (!PlumeFlushGuestWork()) {
    ++g_unanswered_flush;
    ++g_unanswered;
    return false;
  }

  // The flush ran every copy the frame had recorded, not just this one.
  ++g_flushes;
  for (Destination& other : g_destinations) {
    if (other.copy_frame != ~0ull)
      other.flushed = true;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Arming, and the fault that disarms

// Puts back exactly the protection Arm replaced, per range.
//
// This is not bookkeeping for its own sake. The memory system watches guest
// physical pages for writes by marking them kReadOnly and catching the fault
// (MMIOHandler's access-violation callback), and those are the same pages a
// resolve destination lives in. Restoring a blanket kReadWrite here clears that
// watch permanently: the watch never fires again, whatever cache it feeds never
// learns the guest rewrote the memory, and the stale contents are drawn for the
// rest of the run. That is what "textures corrupt as I play" looked like.
void Disarm(Destination& destination) {
#ifdef ETERNALSONATA_READBACK_PAGE_TRAP
  for (uint32_t i = 0; i < destination.range_count; ++i) {
    ArmedRange& range = destination.ranges[i];
    if (!range.armed)
      continue;
    rex::memory::Protect(range.page_base, range.page_bytes, range.old_access);
    range.armed = false;
  }
#endif
  destination.range_count = 0;
}

void Arm(Destination& destination) {
#ifdef ETERNALSONATA_READBACK_PAGE_TRAP
  destination.range_count = 0;
  const uint32_t offset = GuestOffset(destination.fetch);
  for (uint32_t aperture : kApertures) {
    uint8_t* mapping = destination.memory_base + (aperture | offset);
    if (!RangeWritable(mapping, destination.extent))
      continue;

    ArmedRange& range = destination.ranges[destination.range_count];
    range.start = mapping;
    range.bytes = destination.extent;
    const uintptr_t page_bytes = GuestPageBytes();
    const uintptr_t page_start = uintptr_t(mapping) & ~(page_bytes - 1);
    const uintptr_t page_end =
        (uintptr_t(mapping) + destination.extent + page_bytes - 1) & ~(page_bytes - 1);
    range.page_base = reinterpret_cast<uint8_t*>(page_start);
    range.page_bytes = size_t(page_end - page_start);

    // Ask for the protection being replaced rather than assuming it. This was
    // `= kReadWrite` on the reasoning that RangeWritable had just established
    // as much, and that reasoning is wrong in the one case that matters: the
    // memory system watches guest pages by making them kReadOnly, and restoring
    // those as kReadWrite silently destroys the watch. See Disarm.
    range.old_access = rex::memory::PageAccess::kReadWrite;
    if (!rex::memory::Protect(range.page_base, range.page_bytes,
                              rex::memory::PageAccess::kNoAccess, &range.old_access)) {
      continue;
    }
    range.armed = true;
    ++destination.range_count;
    ++g_arms;
  }
#else
  (void)destination;
#endif
}

#ifdef ETERNALSONATA_READBACK_PAGE_TRAP
// The platform-independent half of the fault: everything from "is this address
// one of ours" to "guest memory now holds the pixels". Returns true if the
// access was ours and has been let through, so the faulting instruction can be
// retried; false to pass the fault on to whoever else wants it.
//
// Both callers arrive here on the faulting thread with the fault synchronous, so
// taking the mutex and writing memory is no worse than it is on either platform
// alone. `g_in_handler` is what keeps a fault raised inside this from recursing
// onto the mutex it already holds.
bool HandleReadbackFault(uint8_t* address, bool is_write) {
  if (g_in_handler)
    return false;
  g_in_handler = true;
  struct Guard {
    ~Guard() { g_in_handler = false; }
  } guard;

  std::unique_lock<std::mutex> lock(g_mutex);
  for (Destination& destination : g_destinations) {
    for (uint32_t i = 0; i < destination.range_count; ++i) {
      const ArmedRange& range = destination.ranges[i];
      if (!range.armed || address < range.page_base ||
          address >= range.page_base + range.page_bytes) {
        continue;
      }

      // Armed by us once, but is the page still inaccessible *because* of that?
      // If the guest has freed the buffer since, this is a real access violation
      // on memory that merely used to be ours, and swallowing it would turn a
      // clean crash into a silent one. The arming is dropped either way so the
      // question is not asked twice.
      size_t region_bytes = 0;
      rex::memory::PageAccess access = rex::memory::PageAccess::kReadWrite;
      const bool ours = rex::memory::QueryProtect(address, region_bytes, access) &&
                        access == rex::memory::PageAccess::kNoAccess;
      if (!ours) {
        ++g_disowned_faults;
        Disarm(destination);
        return false;
      }

      // Ours. Whatever happens next, the guest's access has to be allowed
      // through, so every path here disarms first.
      const bool inside = address >= range.start && address < range.start + range.bytes;
      Disarm(destination);
      if (!inside) {
        // The tail page this destination shares with whatever follows it. Says
        // nothing about the destination, and re-arming would only fault again.
        ++g_outside_faults;
      } else if (is_write) {
        // The guest writing its own buffer. Filling it now would destroy what
        // it is in the middle of writing, so this only stops the arming for
        // this frame; the next resolve arms it again.
        ++destination.writes;
        ++g_write_faults;
        destination.guest_owns = true;
        LogEvent(destination, "guest write", false);
      } else {
        ++destination.reads;
        ++g_read_faults;
        // EnsureData may release the lock to wait for a copy, and the render
        // thread can grow g_destinations while it is out. Everything after that
        // point goes through a fresh lookup rather than through this reference.
        const uint32_t address = destination.address;
        bool filled = false;
        if (!Fresh(destination)) {
          ++g_refused_stale;
          LogEvent(destination, "guest read", false);
        } else {
          const bool ready = EnsureData(destination, lock);
          Destination* again = Find(address);
          if (again == nullptr) {
            // Freed while we waited. Nothing to fill and nothing to say about
            // it beyond that the access has been let through.
            ++g_refused_stale;
          } else {
            // Pages this destination shares with a cached texture are somebody
            // else's by now; Fill cuts them out of the write rather than
            // refusing the whole fill. See the comment there.
            if (ready)
              filled = Fill(*again);
            LogEvent(*again, "guest read", filled);
          }
        }
      }
      return true;
    }
  }
  return false;
}
#endif

#ifdef _WIN32
long __stdcall ReadbackExceptionHandler(EXCEPTION_POINTERS* info) {
  if (info == nullptr || info->ExceptionRecord == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (info->ExceptionRecord->NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;

  const bool is_write = info->ExceptionRecord->ExceptionInformation[0] == 1;
  auto* address = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
  return HandleReadbackFault(address, is_write) ? EXCEPTION_CONTINUE_EXECUTION
                                                : EXCEPTION_CONTINUE_SEARCH;
}

// Installed on the first arm rather than at startup, so a run with the readback
// off never adds a handler at all. First in the chain: these faults are ours and
// nothing else should have to reason about them.
void EnsureExceptionHandler() {
  static void* handle = AddVectoredExceptionHandler(1, ReadbackExceptionHandler);
  (void)handle;
}

#elif defined(ETERNALSONATA_READBACK_PAGE_TRAP)

// What was installed before us, forwarded to for every fault that is not ours.
// The SDK's own chain lives behind this, so getting the forward right is what
// keeps the memory system's watches working.
struct sigaction g_previous_segv = {};
bool g_previous_segv_valid = false;

void ForwardSegv(int signal_number, siginfo_t* info, void* context) {
  if (g_previous_segv_valid) {
    if ((g_previous_segv.sa_flags & SA_SIGINFO) != 0 && g_previous_segv.sa_sigaction != nullptr) {
      g_previous_segv.sa_sigaction(signal_number, info, context);
      return;
    }
    if (g_previous_segv.sa_handler != SIG_DFL && g_previous_segv.sa_handler != SIG_IGN) {
      g_previous_segv.sa_handler(signal_number);
      return;
    }
  }
  // Nobody else wants it. Restore the default and return, so the faulting
  // instruction runs again and dies the way it would have without us, with its
  // registers and fault address intact for a core dump.
  struct sigaction fallback = {};
  fallback.sa_handler = SIG_DFL;
  sigemptyset(&fallback.sa_mask);
  sigaction(signal_number, &fallback, nullptr);
}

void ReadbackSegvHandler(int signal_number, siginfo_t* info, void* context) {
  if (info == nullptr || context == nullptr) {
    ForwardSegv(signal_number, info, context);
    return;
  }

  // Bit 1 of the x86-64 page-fault error code: set for a write, clear for a
  // read. Same field the SDK's own posix handler reads.
  const auto* uc = static_cast<const ucontext_t*>(context);
  const bool is_write = (uc->uc_mcontext.gregs[REG_ERR] & 0x2) != 0;
  auto* address = static_cast<uint8_t*>(info->si_addr);

  // HandleReadbackFault claims the fault only if the address falls inside a
  // range this armed, so everything else -- including every guest write watch --
  // goes on to the handler that was there before.
  if (!HandleReadbackFault(address, is_write))
    ForwardSegv(signal_number, info, context);
  // Returning retries the faulting instruction, as EXCEPTION_CONTINUE_EXECUTION
  // does on Windows. The page is accessible again by now, so it succeeds.
}

// Installed on the first arm, which is well after the memory system has
// installed its own -- that is the point: this has to be the newer handler to
// be the one the kernel calls.
void EnsureExceptionHandler() {
  static const bool installed = []() {
    struct sigaction action = {};
    action.sa_sigaction = ReadbackSegvHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    g_previous_segv_valid = sigaction(SIGSEGV, &action, &g_previous_segv) == 0;
    return g_previous_segv_valid;
  }();
  (void)installed;
}
#endif

Destination* Find(uint32_t address) {
  for (Destination& destination : g_destinations) {
    if (destination.address == address)
      return &destination;
  }
  return nullptr;
}

}  // namespace

bool ReadbackEnabled() { return CurrentMode() != Mode::kOff; }

void ReadbackPublish(uint8_t* memory_base, const TextureFetch& dest, const uint8_t* pixels,
                     uint32_t row_bytes, uint32_t pixel_rows, bool pixels_ready,
                     uint64_t frame) {
  const Mode mode = CurrentMode();
  if (mode == Mode::kOff || memory_base == nullptr || pixels == nullptr ||
      dest.base_address == 0 || dest.width == 0 || dest.height == 0) {
    return;
  }

  // The arming is what this costs a frame that reads nothing back, so it is
  // measured. The lazy fill is not: it runs from the fault handler, which is not
  // necessarily on the render thread these counters assume.
  ProfileZone zone(kPhaseReadbackPublish);

  // Whoever publishes is whoever records the frame, which is what a flush needs.
  g_is_render_thread = true;

  std::lock_guard<std::mutex> lock(g_mutex);
  Destination* destination = Find(dest.base_address);
  if (destination == nullptr) {
    g_destinations.emplace_back();
    destination = &g_destinations.back();
    destination->address = dest.base_address;
  }

  // Nothing has changed and the pages are still armed, so the arming from the
  // previous frame is still the right one and still points at the right buffer.
  // Rebuilt every so often anyway; see kRearmPeriod.
  // Every resolve into a small destination, uncapped and cheap: there are a
  // handful a session, and the question they answer is the one the content line
  // raises. A save preview filled from a buffer whose last resolve was at frame
  // 239 means no screenshot was resolved at save time, and that is a fact about
  // the resolve path rather than about the readback.
  if (dest.width <= 512) {
    REXLOG_INFO("native_renderer: readback resolve into 0x{:08X} {}x{} at frame {}",
                dest.base_address, dest.width, dest.height, frame);
  }

  ++destination->publishes;
  destination->filled_since_publish = false;
  destination->last_publish_frame = frame;
  destination->guest_owns = false;
  // The caller records a copy into the buffer immediately after this, so this
  // frame is the one that copy belongs to. `pixels_ready` says an earlier copy
  // has already completed, which only matters for the log.
  destination->copy_frame = frame;
  // Cleared, not carried: a *new* copy has just been recorded and has not run,
  // so a read later in this frame still has to make the GPU catch up. Leaving it
  // set once any flush had ever happened is what made the first save after a load
  // black. The destination already existed from before the load, so `flushed` was
  // still true from an earlier flush, EnsureData believed the buffer was current
  // and the fill handed the guest whatever the buffer held from before, which
  // that soon after a load is the loading screen. Waiting a few seconds and
  // saving again worked because by then the copy had completed on its own, which
  // is exactly the shape of the report.
  destination->flushed = false;
  (void)pixels_ready;
  if (frame > g_now)
    g_now = frame;
  if (mode == Mode::kAuto && (destination->publishes % kRearmPeriod) != 0 &&
      SameAsArmed(*destination, dest, pixels, row_bytes)) {
    return;
  }

  // Before anything is measured or looked up: the arming from the previous
  // frame is what would otherwise make this destination's own pages read as
  // unmapped, and the extent may have changed underneath it.
  Disarm(*destination);

  destination->fetch = dest;
  destination->memory_base = memory_base;
  destination->pixels = pixels;
  destination->row_bytes = row_bytes;
  destination->pixel_rows = pixel_rows;
  destination->guest = nullptr;

  uint32_t texel_bytes = 0;
  if (!DestinationTexelBytes(dest.format, texel_bytes)) {
    if (!destination->refused) {
      destination->refused = true;
      ++g_refused_format;
      REXLOG_INFO(
          "native_renderer: readback will not write 0x{:08X} back to guest memory: format {} is "
          "not the colour format the host image is in",
          dest.base_address, dest.format);
    }
    return;
  }
  destination->texel_bytes = texel_bytes;
  destination->extent = ExtentBytes(dest, texel_bytes);
  if (destination->extent == 0 || destination->extent > kMaxDestinationBytes) {
    if (!destination->refused) {
      destination->refused = true;
      ++g_refused_extent;
    }
    return;
  }

  // Which aperture actually spans the whole destination. They alias the same
  // physical pages but are not all committed over the same ranges, so this is
  // asked per destination rather than fixed. See GuestPhysicalPointer.
  const uint32_t offset = GuestOffset(dest);
  for (uint32_t aperture : kApertures) {
    uint8_t* candidate = memory_base + (aperture | offset);
    if (RangeWritable(candidate, destination->extent)) {
      destination->guest = candidate;
      destination->aperture = aperture;
      break;
    }
  }
  if (destination->guest == nullptr) {
    if (!destination->refused) {
      destination->refused = true;
      ++g_refused_unmapped;
      REXLOG_INFO(
          "native_renderer: readback found no aperture spanning {} byte(s) of resolve destination "
          "0x{:08X}; guest memory there will not be written",
          destination->extent, dest.base_address);
    }
    return;
  }
  destination->refused = false;

  if (g_logged < 24) {
    ++g_logged;
    REXLOG_INFO(
        "native_renderer: readback tracking resolve destination 0x{:08X} (raw 0x{:08X}, aperture "
        "0x{:08X}), {}x{} fmt {} {} pitch {} endian {}, {} guest byte(s) to 0x{:08X}, mode {}",
        dest.base_address, dest.raw_base_address, destination->aperture, dest.width, dest.height,
        dest.format, dest.tiled ? "tiled" : "linear", dest.pitch, dest.endianness,
        destination->extent,
        uint32_t(GuestOffset(dest) + destination->extent), mode == Mode::kEager ? "eager" : "auto");
  }

  if (mode == Mode::kEager) {
    // Deliberately no EnsureData: this fills with whatever the GPU has already
    // finished, one frame late, which is what `readback_resolve=fast` does and
    // what every non-Windows build did before the page trap existed.
    //
    // Asking EnsureData for a same-frame guarantee here calls PlumeFlushGuestWork
    // during the very first resolve, before the frame this is recording into has
    // ever been presented, and that hangs the guest at frame 0 -- no window ever
    // appears. `auto` does not hit it because its first resolve arms rather than
    // fills. Eager is the instrument you reach for when the trap is the suspect,
    // so it has to boot.
    Fill(*destination);
    return;
  }

#ifdef ETERNALSONATA_READBACK_PAGE_TRAP
  // Too big to be answered on the strength of a read fault alone; see
  // kMaxArmedExtentBytes. Left tracked and left copied, so the texture mirror can
  // still pull it with the layout check that a fault cannot do.
  if (destination->extent > kMaxArmedExtentBytes) {
    if (!destination->arming_refused) {
      destination->arming_refused = true;
      ++g_refused_large;
      REXLOG_INFO(
          "native_renderer: readback will not trap on resolve destination 0x{:08X}: {}x{} is {} "
          "guest byte(s), too much to write back on a read fault alone",
          dest.base_address, dest.width, dest.height, destination->extent);
    }
    return;
  }
  EnsureExceptionHandler();
  Arm(*destination);
#else
  // No page trap on this platform, so "auto" has nothing to arm and nothing
  // would ever be written. Fill unconditionally rather than silently doing
  // nothing. This is `eager` in all but name and it costs what `eager` costs --
  // several ms a frame for destinations the guest never reads -- so a platform
  // landing here is a platform with a performance problem, not just a different
  // code path.
  Fill(*destination);
#endif
}

bool ReadbackFillForRead(const TextureFetch& bound) {
  if (CurrentMode() == Mode::kOff || bound.base_address == 0)
    return false;

  // unique_lock because EnsureData may release it to wait for a copy.
  std::unique_lock<std::mutex> lock(g_mutex);
  Destination* destination = Find(bound.base_address);
  if (destination == nullptr || destination->refused || destination->pixels == nullptr ||
      destination->guest == nullptr) {
    return false;
  }
  ++destination->pulls;
  ++g_pulls;

  // The layout the fill used against the layout the read is about to use. These
  // have to agree byte for byte; where they do not, the image comes back sheared
  // or rotated rather than wrong in any way that looks like a bug.
  const TextureFetch& wrote = destination->fetch;
  const bool disagrees = wrote.pitch != bound.pitch || wrote.width != bound.width ||
                         wrote.height != bound.height || wrote.tiled != bound.tiled ||
                         wrote.endianness != bound.endianness || wrote.format != bound.format;
  if (disagrees && !destination->layout_reported) {
    destination->layout_reported = true;
    ++g_layout_disagreements;
    REXLOG_WARN(
        "native_renderer: readback 0x{:08X} was written as {}x{} pitch {} {} fmt {} endian {} but "
        "is read as {}x{} pitch {} {} fmt {} endian {}",
        bound.base_address, wrote.width, wrote.height, wrote.pitch,
        wrote.tiled ? "tiled" : "linear", wrote.format, wrote.endianness, bound.width, bound.height,
        bound.pitch, bound.tiled ? "tiled" : "linear", bound.format, bound.endianness);
  }
  // And then refused rather than reconciled. A disagreement this wide is not a
  // resolve destination being read back: it is the guest having freed the buffer
  // and loaded something else into it, seen live as a 320x160 tiled BGRA
  // thumbnail destination being bound as a 64x64 linear DXT1 texture. Filling
  // there scatters render target pixels over an unrelated asset.
  //
  // Only this read is refused. Marking the destination as the guest's instead
  // was tried and is wrong: it makes the refusal self-sustaining, so a later
  // bind that *does* match the layout is refused too and the preview is drawn
  // from guest memory nothing has filled.
  if (disagrees) {
    ++g_refused_stale;
    return false;
  }
  if (destination->filled_since_publish)
    return false;
  if (!Fresh(*destination)) {
    ++g_refused_stale;
    LogEvent(*destination, "mirror pull", false);
    return false;
  }

  // Disarmed first, and not only for speed: the caller is about to read these
  // pages, and the fill writes to them.
  Disarm(*destination);
  const uint32_t address = destination->address;
  if (!EnsureData(*destination, lock))
    return false;
  // Re-found for the same reason as in the fault handler: EnsureData may have
  // released the lock, and the reference does not survive that.
  destination = Find(address);
  return destination != nullptr && Fill(*destination);
}

void ReadbackForget(uint32_t address) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t i = 0; i < g_destinations.size(); ++i) {
    if (g_destinations[i].address != address)
      continue;
    Disarm(g_destinations[i]);
    g_destinations.erase(g_destinations.begin() + i);
    return;
  }
}

void ReadbackForgetAll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Destination& destination : g_destinations)
    Disarm(destination);
  g_destinations.clear();
}

void LogReadbackSummary() {
  std::lock_guard<std::mutex> lock(g_mutex);
  REXLOG_INFO(
      "native_renderer: readback destinations={} arms={} | faults read={} write={} outside={} "
      "disowned={} | mirror pulls={} | fills={} ({} MiB) | flushes={} waited={} "
      "unanswered={} (thread {}, flush {}) | "
      "refused format={} "
      "extent={} unmapped={} stale={} untrapped={} | clipped fills={} ({} KiB) | layout disagreements={}",
      g_destinations.size(), g_arms, g_read_faults, g_write_faults, g_outside_faults,
      g_disowned_faults, g_pulls,
      g_fills, g_fill_bytes >> 20, g_flushes, g_waited_copies, g_unanswered, g_unanswered_thread,
      g_unanswered_flush, g_refused_format, g_refused_extent,
      g_refused_unmapped, g_refused_stale, g_refused_large, g_clipped_fills,
      g_clipped_bytes >> 10, g_layout_disagreements);

  // One line per destination anything has actually asked for, which is the
  // question this whole path exists to answer: who reads a resolved surface
  // back, the guest's own CPU or this renderer's texture mirror.
  for (const Destination& destination : g_destinations) {
    if (destination.reads == 0 && destination.writes == 0 && destination.pulls == 0)
      continue;
    REXLOG_INFO(
        "native_renderer: readback 0x{:08X} {}x{}: guest reads={} writes={} | mirror pulls={} | "
        "fills={}",
        destination.address, destination.fetch.width, destination.fetch.height, destination.reads,
        destination.writes, destination.pulls, destination.fills);
  }
}

}  // namespace eternalsonata
