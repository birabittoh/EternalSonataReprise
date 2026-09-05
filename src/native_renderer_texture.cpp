// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_texture.h.

#include "native_renderer_texture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "native_renderer_frame.h"
#include "native_renderer_readback.h"
#include "native_renderer_plume_internal.h"
#include "native_renderer_profile.h"

namespace eternalsonata {
namespace {

using namespace plume;

// D3D12 wants a placed footprint's rows 256 byte aligned and its offset 512
// byte aligned. Each upload gets a buffer of its own, so only the first bites.
constexpr uint32_t kUploadRowAlignment = 256;

// A bound on what will be believed out of a fetch constant. The decode is
// runtime-verified (see the handoff's texture section), but an extent is still
// read from guest memory that anything could have written, and the product of
// two 13 bit fields is what gets allocated. The largest texture this title was
// observed to bind is 1280x720.
constexpr uint32_t kMaxTextureExtent = 4096;
constexpr uint64_t kMaxTextureBytes = 64ull * 1024 * 1024;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Smallest n with (1 << n) >= value. Zero for 0 and 1.
uint32_t Log2Ceil(uint32_t value) {
  uint32_t n = 0;
  while ((1u << n) < value)
    ++n;
  return n;
}

// Is the whole of [start, start + bytes) actually readable?
//
// This is not defensive coding, it is required. A fetch constant is guest state
// like any other, and a stage can hold one that points at memory the guest has
// not committed -- an address left over from a freed resource, or a slot the
// game never cleared because it never sampled it. The hardware would fault the
// same way; the difference is that the guest only ever fetches from stages its
// shader actually reads, and the mirror visits all sixteen because it cannot
// know which those are without reflecting the blob.
//
// The first texture the title binds is exactly this case (0x0AF5C000 at the
// second pipeline), so without the check the mirror crashes on its first real
// decode.
//
// Returns how many bytes from `start` are readable, capped at `bytes`. The
// count rather than a flag, because a refusal that stops one page short of the
// end is a different bug from one that finds nothing mapped at all, and the two
// are indistinguishable from the counters alone.
uint64_t GuestRangeReadableBytes(const uint8_t* start, uint64_t bytes) {
  if (start == nullptr || bytes == 0)
    return 0;
#ifdef _WIN32
  const uint8_t* cursor = start;
  const uint8_t* end = start + bytes;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(cursor, &info, sizeof(info)) == 0)
      break;
    if (info.State != MEM_COMMIT)
      break;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD) != 0)
      break;
    cursor = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return cursor > end ? bytes : uint64_t(cursor - start);
#else
  const uint8_t* cursor = start;
  const uint8_t* end = start + bytes;
  while (cursor < end) {
    size_t region_length = 0;
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    if (!rex::memory::QueryProtect(const_cast<uint8_t*>(cursor), region_length, access))
      break;
    if (access == rex::memory::PageAccess::kNoAccess || region_length == 0)
      break;
    cursor += region_length;
  }
  return cursor > end ? bytes : uint64_t(cursor - start);
#endif
}

bool GuestRangeReadable(const uint8_t* start, uint64_t bytes) {
  return GuestRangeReadableBytes(start, bytes) >= bytes;
}

// A host pointer to guest physical memory.
//
// The bare physical address a fetch constant carries is not mapped: the host
// backs physical memory only through the 360's virtual apertures, which was
// established by probing 0x00000000, 0x80000000, 0xA0000000, 0xC0000000 and
// 0xE0000000 against a texture that would not read -- the first two are
// unmapped and the last three all alias the same pages.
//
// 0xA0000000 is the cached one and the three alias the same physical pages, so
// where all three are mapped the bytes are identical. They do **not** all cover
// the same range, though, which is what made the first intro logo white: its
// 1280x720 DXT1 source at 0x08567000 needs 460800 bytes and the A aperture is
// committed only as far as 0x085C0000, 364544 bytes in, while C and E carry the
// whole thing. So the aperture is chosen per read, by asking which one actually
// spans the bytes wanted, rather than fixed.
//
// The address is the physical one the fetch decode produces, not the raw field,
// so an E-aperture resource and its resolve destination agree on one key.
//
// Guest side, the 0xE0000000 view sits one 4 KB page above physical memory
// (PhysicalHeap::GetPhysicalAddress adds 0x1000 for a heap based there). Host
// side, whether that page is in the mapping at all depends on the platform:
// Memory::MapViews maps the E view at file offset 0x100001000 rounded down to
// the allocation granularity, so the page survives only where the granularity
// is 4 KB. On Windows, at 64 KB, it is masked away and the heap emulates the
// offset in its own bookkeeping, leaving the E view aliasing A and C byte for
// byte. Verified live by comparing the views over one range: with the shift
// applied they differ from byte 0, without it they are equal.
const uint32_t kEApertureShift =
    rex::memory::allocation_granularity() > 0x1000 ? 0u : 0x1000u;

const uint8_t* GuestPhysicalPointer(uint8_t* memory_base, uint32_t physical_address,
                                    uint64_t bytes) {
  ProfileZone zone(kPhaseGuestPointer);
  const uint32_t offset = physical_address & 0x1FFFFFFFu;
  const uint8_t* first = nullptr;
  for (uint32_t aperture : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    const uint32_t shifted = aperture == 0xE0000000u ? offset - kEApertureShift : offset;
    const uint8_t* candidate = memory_base + (aperture | shifted);
    if (first == nullptr)
      first = candidate;
    if (GuestRangeReadableBytes(candidate, bytes) >= bytes)
      return candidate;
  }
  // Nothing spans it. Hand back the cached aperture anyway so the caller's own
  // readability check is what refuses, and reports against the usual one.
  return first;
}

// ---------------------------------------------------------------------------
// Formats

// What a guest texture format costs and what it becomes on the host.
//
// `block` is the edge of one addressable unit in texels, which is 1 for an
// uncompressed format and 4 for the DXT ones. Tiling works in these units, not
// in texels, which is the whole reason it is carried here.
//
// `expand` is how a guest unit that has no host format of its own is widened on
// the way to the GPU. Plume's RenderFormat list has no 16 bit colour format at
// all, so the packed 5:6:5 and 1:5:5:5 the game takes its photos in are read out
// of guest memory as the two byte units they are -- which is what tiling and the
// endian swap need -- and then unpacked into B8G8R8A8 before the upload.
//
// The BC entries are the same mechanism for a different reason: the host format
// exists but the device may not support it. Vulkan guarantees one compressed
// family of BC, ETC2 or ASTC, and mobile GPUs implement the latter two, so the
// DXT formats are decoded to B8G8R8A8 there instead. See g_bc_supported.
enum class Expand {
  kNone,
  k5_6_5,
  k1_5_5_5,
  k8_8,
  kBC1,
  kBC2,
  kBC3,
};

bool IsBlockExpand(Expand expand) {
  return expand == Expand::kBC1 || expand == Expand::kBC2 || expand == Expand::kBC3;
}

// Whether the device can sample BC directly. Queried once at first use rather
// than at startup, because the texture mirror has no init hook that runs after
// the Plume device exists.
int g_bc_supported = -1;

bool DeviceSupportsBC() {
  if (g_bc_supported < 0) {
    RenderDevice* device = PlumeDevice();
    if (device == nullptr)
      return true;
    g_bc_supported = device->getCapabilities().textureCompressionBC ? 1 : 0;
    // Forces the decode path on a device that does support BC, which is the
    // only way to exercise it where a frame debugger is available.
    if (std::getenv("ETERNALSONATA_NO_BC") != nullptr)
      g_bc_supported = 0;
    REXLOG_INFO("native_renderer: device texture compression BC={}",
                g_bc_supported != 0 ? "yes" : "no, DXT will be decoded to B8G8R8A8");
  }
  return g_bc_supported != 0;
}

struct FormatInfo {
  RenderFormat host = RenderFormat::UNKNOWN;
  uint32_t block_bytes = 0;
  uint32_t block = 1;
  Expand expand = Expand::kNone;
};

// Bytes one addressable unit costs on the host, which is the guest's own size
// unless the unit is being widened on the way up.
uint32_t HostBlockBytes(const FormatInfo& info) {
  return info.expand == Expand::kNone ? info.block_bytes : 4;
}

bool MapTextureFormat(const TextureFetch& fetch, FormatInfo& out) {
  switch (fetch.format) {
    // k_1_5_5_5 and k_5_6_5. The in-game photo feature renders into one of
    // these, and a refused format is drawn with the 1x1 white placeholder, which
    // is exactly what a photo that comes out completely white looks like.
    case 3:
      out = {RenderFormat::B8G8R8A8_UNORM, 2, 1, Expand::k1_5_5_5};
      return true;
    case 4:
      out = {RenderFormat::B8G8R8A8_UNORM, 2, 1, Expand::k5_6_5};
      return true;
    // k_8_8_8_8. Taken as BGRA rather than RGBA to match the frame layer's own
    // colour targets, which are B8G8R8A8_UNORM: a resolve destination and an
    // asset both arrive here as format 6, and they cannot disagree about
    // channel order. See the caveat on the fetch constant swizzle below.
    case 6:
      out = {RenderFormat::B8G8R8A8_UNORM, 4, 1};
      return true;
    // k_8_8, a two channel 8 bit texture, which is what a title stores a
    // tangent space normal or a DuDv offset map in.
    //
    // Widened to four channels rather than bound as a two channel host format,
    // because the hardware does not read zero out of the missing ones: xenia's
    // host format table gives k_8_8 the swizzle RGGG, so both b and a sample
    // back as g. The terrain material depends on it: its normal map fetch is
    // `tfetch2D r11.xyz_` and the tangent frame it builds normalizes
    // (x, y, z) with z coming from that third channel, so a zero there leaves a
    // normal lying in the tangent plane, N.L collapses to zero for every light,
    // and the ground draws with its ambient term alone.
    //
    // The signedness is the fetch's, for the same reason: xenia keys the host
    // format on it, and a normal map read as unsigned has no negative half.
    case 10:
      out = {(fetch.sign & 3u) != 0 ? RenderFormat::R8G8B8A8_SNORM
                                    : RenderFormat::R8G8B8A8_UNORM,
             2, 1, Expand::k8_8};
      return true;
    // The guest unit stays 8 or 16 bytes over a 4x4 block either way: that is
    // what the untiler and the endian swap address in, and it is what guest
    // memory holds. Only the host format and the step before the upload change.
    case 18:  // k_DXT1
      if (DeviceSupportsBC())
        out = {RenderFormat::BC1_UNORM, 8, 4};
      else
        out = {RenderFormat::B8G8R8A8_UNORM, 8, 4, Expand::kBC1};
      return true;
    case 19:  // k_DXT2_3
      if (DeviceSupportsBC())
        out = {RenderFormat::BC2_UNORM, 16, 4};
      else
        out = {RenderFormat::B8G8R8A8_UNORM, 16, 4, Expand::kBC2};
      return true;
    case 20:  // k_DXT4_5
      if (DeviceSupportsBC())
        out = {RenderFormat::BC3_UNORM, 16, 4};
      else
        out = {RenderFormat::B8G8R8A8_UNORM, 16, 4, Expand::kBC3};
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Endianness
//
// Guest memory holds what the PowerPC wrote, and the fetch constant's endian
// field says how the texture hardware unswizzles it on the way in. Xenia's
// texture loaders apply the swap *uniformly over the whole addressable unit*
// (XeEndianSwap32 in texture_load_64bpb/128bpb.xesli), so a DXT block is
// swapped in its entirety, index bytes included, rather than only at its
// endpoints. That is worth stating because the obvious alternative -- swapping
// only the 16 bit endpoint pair, which is what a reader of the on-disk NTX2
// container does -- is right for a file and wrong here. The two differ only in
// the index bytes, so getting it wrong is a subtly scrambled texture, not an
// obviously broken one.

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

// ---------------------------------------------------------------------------
// Tiling
//
// The Xenos 2D tiled address, in units of one addressable block. Transcribed
// from xenia's texture_util.cc and cross-checked against eternal-sonata-studio's
// Ntx2Parser, which carries the same two functions and was validated against
// this title's own textures.

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

// ---------------------------------------------------------------------------
// The cache

struct MirroredTexture {
  uint32_t address = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::unique_ptr<RenderTexture> texture;

  // What the guest bytes behind this texture looked like when it was uploaded.
  // Without it the cache serves whatever was at this address the first time it
  // was bound, forever, which is not a corner case in this title: the game
  // streams a new texture into a buffer it already owns rather than allocating
  // a new one, so a cutscene effect and a menu background can be the same
  // address at the same size and format. See CheckSourceChanged.
  uint64_t content_hash = 0;
  uint64_t source_bytes = 0;

  // Offset from the fetch constant's base to level 0; see Level0ByteOffset.
  // Carried on the entry because the hash and the aperture walk both work from
  // the entry alone, without the FormatInfo needed to recompute it, and they
  // must cover the same bytes the upload reads or the cache would fingerprint
  // the mip tail and never notice level 0 changing.
  uint32_t level0_offset = 0;

  // The aperture-resolved host pointer to this texture's source, and the frame
  // the choice was last validated in.
  //
  // Picking the aperture means asking which of the three spans the bytes, which
  // is a VirtualQuery walk over the whole source. That was measured at 73 us a
  // call against the 12 us hash it feeds, once per texture per frame: the
  // single largest cost in the frame after the vertex upload was fixed. The
  // answer is a property of the guest's memory map rather than of the frame, so
  // it is kept and revalidated occasionally instead.
  //
  // Revalidation is cheap insurance rather than a correctness requirement. The
  // walk never protected this path anyway: when no aperture spans the range it
  // returns the cached one regardless and lets the read proceed, which is what
  // the hash has always done.
  const uint8_t* source_pointer = nullptr;
  uint64_t source_pointer_frame = ~0ull;

  // The frame this texture's source was last hashed in. The hash covers the
  // whole source and is therefore too expensive to repeat on every bind, so it
  // runs at most once per texture per frame; a texture bound two hundred times
  // in a frame is hashed once. See HashSource.
  uint64_t hashed_frame = ~0ull;
};

// Bumped once per guest swap. Only ever compared for equality.
uint64_t g_frame = 0;

// How often a cached aperture choice is checked again.
//
// The three apertures alias the same physical pages, so a stale choice only
// matters if the one that was picked stops being committed while another still
// spans the range. At 64 frames that window is about a second, and the walks it
// costs are a rounding error: measured at 77 per 300 frames with a 256 frame
// period, so a few hundred here against the 22,827 this replaced.
constexpr uint64_t kApertureRevalidateFrames = 64;

// Walks behind the cache, so the saving is visible rather than assumed.
uint64_t g_aperture_walks = 0;
uint64_t g_aperture_reuses = 0;

std::vector<std::unique_ptr<MirroredTexture>> g_textures;

// An index over the same entries, because the cache is unbounded and a linear
// scan of it is paid per texture slot per draw. At a few hundred entries that
// was the single largest cost in the frame; the working set here reaches ~390.
//
// The key is exact rather than a hash of the fields: a fetch's address is 32
// bits, its format 6, and its width and height 13 each, which is 64 bits with
// nothing to spare. So a match on the key is a match on all four fields and the
// entries need no further comparison.
//
// Nothing is ever evicted (see the note on the cache being unbounded), so this
// only ever grows alongside g_textures and needs no invalidation.
std::unordered_map<uint64_t, MirroredTexture*> g_texture_index;

uint64_t TextureKey(uint32_t address, uint32_t format, uint32_t width, uint32_t height) {
  return (uint64_t(address) << 32) | (uint64_t(format & 0x3F) << 26) |
         (uint64_t(width & 0x1FFF) << 13) | uint64_t(height & 0x1FFF);
}

uint64_t g_resolve_hits = 0;
uint64_t g_decode_hits = 0;
uint64_t g_decoded = 0;
uint64_t g_refused_format = 0;
uint64_t g_refused_extent = 0;
uint64_t g_refused_upload = 0;
uint64_t g_refused_unmapped = 0;

// Failed decodes that were attempted again on a later bind, and how many of
// those attempts eventually succeeded. A refusal is counted once per address,
// on the first attempt only, so that the refusal counters stay a count of
// distinct textures the mirror could not produce rather than a count of binds.
uint64_t g_retries = 0;
uint64_t g_recovered = 0;
bool g_count_refusals = true;

void CountRefusal(uint64_t& counter) {
  if (g_count_refusals)
    ++counter;
}

// Cached textures whose guest bytes changed under them and were re-uploaded.
uint64_t g_refreshed = 0;

// Report each unhandled guest format once. There are only 64 of them, so a flat
// array is cheaper than deciding whether it is worth being cleverer.
bool g_format_reported[64] = {};

// How many bytes of guest memory this texture occupies, which is both what
// bounds the read below and what the content hash covers. Tiled and linear
// disagree: the tiled swizzle's highest offset is not the last block of the
// last row, so it is bounded by the whole macro tile grid instead.
uint64_t SourceExtentBytes(const TextureFetch& fetch, const FormatInfo& info) {
  const uint32_t height_blocks = (fetch.height + info.block - 1) / info.block;
  const uint32_t pitch_texels = fetch.pitch > fetch.width ? fetch.pitch : fetch.width;
  const uint32_t pitch_blocks = (pitch_texels + info.block - 1) / info.block;

  if (fetch.tiled) {
    const uint32_t pitch_macro_tiles = pitch_blocks / 32 > 1 ? pitch_blocks / 32 : 1;
    const uint32_t macro_rows = (height_blocks + 31) / 32;
    return uint64_t(pitch_macro_tiles) * macro_rows * 32 * 32 * info.block_bytes;
  }
  return uint64_t(pitch_blocks) * info.block_bytes * height_blocks;
}

// Where level 0 starts, as an offset from the fetch constant's base.
//
// Zero unless `mip_address` is 0 with a non-zero `mip_max_level`, which covers
// two layouts:
//
// Smallest dimension below 16: the whole chain is packed into one tile and
// level 0 sits at 16 >> (4 - log2_size) blocks into it. The SDK's
// texture_util GetPackedMipOffset describes the same tile but clamps
// `packed_mip_base` at zero, so it reports 16 blocks for every base below 16.
//
// Otherwise: the mip chain comes first and level 0 follows one extent in.
uint64_t Level0ByteOffset(const TextureFetch& fetch, const FormatInfo& info) {
  if (fetch.mip_address != 0 || fetch.mip_max_level == 0)
    return 0;

  const uint32_t log2_size =
      Log2Ceil(fetch.width < fetch.height ? fetch.width : fetch.height);
  if (log2_size < 4) {
    const uint32_t offset_blocks = 16u >> (4 - log2_size);
    return uint64_t(offset_blocks) * info.block_bytes;
  }
  return SourceExtentBytes(fetch, info);
}

// A fingerprint of the guest bytes, used to notice that the game has replaced
// a texture's contents under an address the cache has already seen.
//
// This covers **every byte**, and it has to. An earlier version sampled 64
// chunks of 32 bytes spread across the extent, on the theory that the change
// worth catching is a wholesale replacement -- streaming a new asset into a
// buffer the game already owns -- which changes essentially all of them. That
// is true of assets and false of the one texture that matters most: the font
// glyph cache is an 864x864 atlas the game fills a cell at a time, and a cell
// is roughly 30x34 pixels in three megabytes. Sampling covered 2048 bytes of
// 2,985,984, so a newly rasterised glyph fell between two samples every time,
// the atlas never refreshed, and the cell kept whatever character had been
// rasterised into it earlier. On screen that is "Brani" reading as "trani":
// correct quads, correct UVs, correct atlas *cell*, stale atlas *content*.
//
// Completeness costs, so the caller pays it at most once per texture per frame
// (see MirroredTexture::hashed_frame) rather than on each of the ~700 binds a
// frame contains. Within a frame the first bind decides, which means a glyph
// the guest rasterises mid frame appears on the next one.
// Bytes fed through HashSource, so the cost of completeness is visible in the
// summary instead of being guessed at.
uint64_t g_hash_bytes = 0;

uint64_t HashSource(const uint8_t* source, uint64_t bytes) {
  ProfileZone zone(kPhaseTextureHash);
  g_hash_bytes += bytes;

  // Four independent FNV-1a lanes rather than one. A single accumulator makes
  // every multiply wait for the previous one, which is the whole cost of the
  // loop; four lanes fill that latency and cost nothing in collision terms,
  // since they are folded together at the end. This is what makes hashing the
  // full extent affordable enough to do at all.
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t lanes[4] = {1469598103934665603ull, 1469598103934665603ull ^ bytes,
                       1469598103934665603ull, 1469598103934665603ull ^ (bytes << 17)};

  uint64_t offset = 0;
  for (; offset + 32 <= bytes; offset += 32) {
    uint64_t chunk[4];
    std::memcpy(chunk, source + offset, sizeof(chunk));
    for (uint32_t i = 0; i < 4; ++i) {
      lanes[i] ^= chunk[i];
      lanes[i] *= kPrime;
    }
  }

  uint64_t hash = bytes;
  for (uint32_t i = 0; i < 4; ++i) {
    hash ^= lanes[i];
    hash *= kPrime;
  }
  for (; offset < bytes; ++offset) {
    hash ^= source[offset];
    hash *= kPrime;
  }
  return hash;
}

// Read the texels out of guest memory into `out`, laid out linearly at
// `dest_row_bytes` per row of blocks, untiling and byte swapping on the way.
// False when the source would be read out of bounds, which is the check on the
// extent and pitch the fetch constant claims.
bool ReadTexels(const uint8_t* source, const TextureFetch& fetch, const FormatInfo& info,
                uint32_t dest_row_bytes, std::vector<uint8_t>& out) {
  const uint32_t width_blocks = (fetch.width + info.block - 1) / info.block;
  const uint32_t height_blocks = (fetch.height + info.block - 1) / info.block;

  // The pitch is in texels and is what the *source* is laid out at; the
  // destination is packed to its own aligned pitch. A pitch below the width
  // would mean the extent and the pitch disagree, so take the wider of the two
  // rather than reading rows that overlap.
  const uint32_t pitch_texels = fetch.pitch > fetch.width ? fetch.pitch : fetch.width;
  const uint32_t pitch_blocks = (pitch_texels + info.block - 1) / info.block;

  out.assign(size_t(dest_row_bytes) * height_blocks, 0);

  if (fetch.tiled) {
    // A macro tile is 32x32 blocks. The hardware pads the pitch up to one, so a
    // texture narrower than a macro tile still occupies a whole one.
    const uint32_t pitch_macro_tiles = pitch_blocks / 32 > 1 ? pitch_blocks / 32 : 1;
    const uint32_t block_bytes_log2 = Log2Exact(info.block_bytes);

    // What the tiled layout can address, which is what bounds the read.
    const uint64_t source_bytes = SourceExtentBytes(fetch, info);
    if (source_bytes > kMaxTextureBytes) {
      CountRefusal(g_refused_extent);
      return false;
    }
    if (!GuestRangeReadable(source, source_bytes)) {
      CountRefusal(g_refused_unmapped);
      return false;
    }

    for (uint32_t by = 0; by < height_blocks; ++by) {
      uint8_t* dest_row = out.data() + size_t(by) * dest_row_bytes;
      for (uint32_t bx = 0; bx < width_blocks; ++bx) {
        const uint32_t offset = TiledBlockOffset(bx, by, pitch_macro_tiles, block_bytes_log2);
        if (uint64_t(offset) + info.block_bytes > source_bytes)
          return false;
        std::memcpy(dest_row + size_t(bx) * info.block_bytes, source + offset, info.block_bytes);
      }
    }
  } else {
    const uint32_t source_row_bytes = pitch_blocks * info.block_bytes;
    const uint64_t source_bytes = SourceExtentBytes(fetch, info);
    if (source_bytes > kMaxTextureBytes) {
      CountRefusal(g_refused_extent);
      return false;
    }
    if (!GuestRangeReadable(source, source_bytes)) {
      CountRefusal(g_refused_unmapped);
      return false;
    }

    const uint32_t copy_bytes = width_blocks * info.block_bytes;
    for (uint32_t by = 0; by < height_blocks; ++by) {
      std::memcpy(out.data() + size_t(by) * dest_row_bytes,
                  source + size_t(by) * source_row_bytes, copy_bytes);
    }
  }

  // Swap after untiling rather than before. The two are independent -- tiling
  // moves whole blocks and the swap works inside one -- so this is only a
  // matter of touching the bytes that are actually kept.
  for (uint32_t by = 0; by < height_blocks; ++by) {
    EndianSwapUnit(out.data() + size_t(by) * dest_row_bytes, width_blocks * info.block_bytes,
                   fetch.endianness);
  }
  return true;
}

// Unpack a 16 bit guest colour into the B8G8R8A8 the host format is in.
//
// `in` is already untiled and byte swapped, so a plain uint16 load reads the
// value the PowerPC wrote. The channel order is the one the Xenos names the
// format for, with the widest channel in the middle for 5:6:5 and the alpha bit
// at the top for 1:5:5:5; the low bits are replicated upward rather than zero
// filled so that an all-ones channel comes out as 255 rather than 248.
void ExpandRows(const std::vector<uint8_t>& in, uint32_t in_row_bytes, uint32_t width,
                uint32_t height, Expand expand, uint32_t out_row_bytes,
                std::vector<uint8_t>& out) {
  out.assign(size_t(out_row_bytes) * height, 0);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = in.data() + size_t(y) * in_row_bytes;
    uint8_t* dest = out.data() + size_t(y) * out_row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      uint16_t value = 0;
      std::memcpy(&value, source + size_t(x) * 2, 2);
      // RGGG, and in RGBA order rather than the BGRA the other two widen into,
      // because the host format here keeps the guest's channel order.
      if (expand == Expand::k8_8) {
        dest[x * 4 + 0] = source[size_t(x) * 2 + 0];
        dest[x * 4 + 1] = source[size_t(x) * 2 + 1];
        dest[x * 4 + 2] = source[size_t(x) * 2 + 1];
        dest[x * 4 + 3] = source[size_t(x) * 2 + 1];
        continue;
      }
      uint8_t r = 0, g = 0, b = 0, a = 255;
      if (expand == Expand::k5_6_5) {
        const uint32_t r5 = (value >> 11) & 0x1F;
        const uint32_t g6 = (value >> 5) & 0x3F;
        const uint32_t b5 = value & 0x1F;
        r = uint8_t((r5 << 3) | (r5 >> 2));
        g = uint8_t((g6 << 2) | (g6 >> 4));
        b = uint8_t((b5 << 3) | (b5 >> 2));
      } else {
        const uint32_t r5 = (value >> 10) & 0x1F;
        const uint32_t g5 = (value >> 5) & 0x1F;
        const uint32_t b5 = value & 0x1F;
        r = uint8_t((r5 << 3) | (r5 >> 2));
        g = uint8_t((g5 << 3) | (g5 >> 2));
        b = uint8_t((b5 << 3) | (b5 >> 2));
        a = (value & 0x8000) != 0 ? 255 : 0;
      }
      dest[x * 4 + 0] = b;
      dest[x * 4 + 1] = g;
      dest[x * 4 + 2] = r;
      dest[x * 4 + 3] = a;
    }
  }
}

// Decode BC1/BC2/BC3 blocks into B8G8R8A8 rows, for devices with no BC support.
//
// The block bytes arrive in the standard little endian DXT layout: the endian
// swap ahead of this runs uniformly over the whole 8 or 16 byte unit, which is
// what the fetch constant asks for and what leaves the block readable here.
//
// `height` is in blocks, so the output carries height * 4 rows. Edge blocks are
// written whole; the row is padded out to the upload pitch and the extra texels
// are outside the copied extent.
void DecodeBlockRows(const std::vector<uint8_t>& in, uint32_t in_row_bytes, uint32_t width,
                     uint32_t height, Expand expand, uint32_t out_row_bytes,
                     std::vector<uint8_t>& out) {
  const uint32_t block_bytes = expand == Expand::kBC1 ? 8 : 16;
  out.assign(size_t(out_row_bytes) * height * 4, 0);

  for (uint32_t by = 0; by < height; ++by) {
    const uint8_t* source = in.data() + size_t(by) * in_row_bytes;
    for (uint32_t bx = 0; bx < width; ++bx) {
      const uint8_t* block = source + size_t(bx) * block_bytes;
      const uint8_t* colour = expand == Expand::kBC1 ? block : block + 8;

      uint16_t c0 = uint16_t(colour[0] | (colour[1] << 8));
      uint16_t c1 = uint16_t(colour[2] | (colour[3] << 8));
      uint32_t bits = uint32_t(colour[4]) | (uint32_t(colour[5]) << 8) |
                      (uint32_t(colour[6]) << 16) | (uint32_t(colour[7]) << 24);

      uint8_t red[4], green[4], blue[4], alpha[4];
      const uint16_t endpoints[2] = {c0, c1};
      for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t r5 = (endpoints[i] >> 11) & 0x1F;
        const uint32_t g6 = (endpoints[i] >> 5) & 0x3F;
        const uint32_t b5 = endpoints[i] & 0x1F;
        red[i] = uint8_t((r5 << 3) | (r5 >> 2));
        green[i] = uint8_t((g6 << 2) | (g6 >> 4));
        blue[i] = uint8_t((b5 << 3) | (b5 >> 2));
        alpha[i] = 255;
      }

      // The one bit punchthrough mode exists only in BC1. BC2 and BC3 carry
      // their alpha separately and always take the four colour interpolation.
      const bool punchthrough = expand == Expand::kBC1 && c0 <= c1;
      if (punchthrough) {
        red[2] = uint8_t((red[0] + red[1]) / 2);
        green[2] = uint8_t((green[0] + green[1]) / 2);
        blue[2] = uint8_t((blue[0] + blue[1]) / 2);
        alpha[2] = 255;
        red[3] = green[3] = blue[3] = 0;
        alpha[3] = 0;
      } else {
        red[2] = uint8_t((2 * red[0] + red[1]) / 3);
        green[2] = uint8_t((2 * green[0] + green[1]) / 3);
        blue[2] = uint8_t((2 * blue[0] + blue[1]) / 3);
        alpha[2] = 255;
        red[3] = uint8_t((red[0] + 2 * red[1]) / 3);
        green[3] = uint8_t((green[0] + 2 * green[1]) / 3);
        blue[3] = uint8_t((blue[0] + 2 * blue[1]) / 3);
        alpha[3] = 255;
      }

      // BC3's alpha palette: two endpoints and either six or four interpolated
      // values, the shorter run reserving two slots for 0 and 255.
      uint8_t alpha_palette[8] = {};
      if (expand == Expand::kBC3) {
        alpha_palette[0] = block[0];
        alpha_palette[1] = block[1];
        if (alpha_palette[0] > alpha_palette[1]) {
          for (uint32_t i = 1; i < 7; ++i) {
            alpha_palette[i + 1] =
                uint8_t(((7 - i) * alpha_palette[0] + i * alpha_palette[1]) / 7);
          }
        } else {
          for (uint32_t i = 1; i < 5; ++i) {
            alpha_palette[i + 1] =
                uint8_t(((5 - i) * alpha_palette[0] + i * alpha_palette[1]) / 5);
          }
          alpha_palette[6] = 0;
          alpha_palette[7] = 255;
        }
      }
      uint64_t alpha_bits = 0;
      if (expand == Expand::kBC3) {
        for (uint32_t i = 0; i < 6; ++i)
          alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
      }

      for (uint32_t y = 0; y < 4; ++y) {
        uint8_t* dest = out.data() + size_t(by * 4 + y) * out_row_bytes +
                        size_t(bx) * 4 * 4;
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t texel = y * 4 + x;
          const uint32_t index = (bits >> (2 * texel)) & 0x3;
          uint8_t a = alpha[index];
          if (expand == Expand::kBC2) {
            const uint8_t nibble = uint8_t((block[texel / 2] >> ((texel & 1) * 4)) & 0xF);
            a = uint8_t((nibble << 4) | nibble);
          } else if (expand == Expand::kBC3) {
            a = alpha_palette[(alpha_bits >> (3 * texel)) & 0x7];
          }
          dest[x * 4 + 0] = blue[index];
          dest[x * 4 + 1] = green[index];
          dest[x * 4 + 2] = red[index];
          dest[x * 4 + 3] = a;
        }
      }
    }
  }
}

// Decode and upload. `existing` is null on a cache miss, in which case a texture
// is created; on a refresh it is the texture already in the cache and the texels
// are copied over it in place. Reusing the object is what makes a refresh safe:
// the descriptor sets and command lists that already point at it stay valid,
// where destroying and replacing it would be a use-after-free on whatever is
// still in flight. It is always the same size and format, because those are part
// of the cache key.
std::unique_ptr<RenderTexture> DecodeAndUpload(uint8_t* memory_base, const TextureFetch& fetch,
                                               const FormatInfo& info, RenderTexture* existing) {
  ProfileZone zone(kPhaseTextureUpload);

  // If this address is a resolve destination, guest memory holds nothing the
  // host ever wrote and the decode below would read whatever the allocation
  // came with. Reaching here at all means the frame layer's own lookup missed,
  // so the readback is the only thing that can put an image there. It is also
  // what unprotects pages the readback path may have armed.
  ReadbackFillForRead(fetch);
  RenderDevice* device = PlumeDevice();
  RenderCommandQueue* queue = PlumeQueue();
  if (device == nullptr || queue == nullptr)
    return nullptr;

  const uint32_t width_blocks = (fetch.width + info.block - 1) / info.block;
  const uint32_t height_blocks = (fetch.height + info.block - 1) / info.block;

  // Plume takes a placed footprint's row width in *texels* and derives the row
  // pitch as ceil(rowWidth / block) * block_bytes, so the alignment has to be
  // chosen in blocks and handed back as texels.
  const uint32_t host_block_bytes = HostBlockBytes(info);
  // A decoded block is no longer one host unit but info.block of them across,
  // so its row is counted in texels rather than in blocks.
  const uint32_t host_row_units =
      IsBlockExpand(info.expand) ? width_blocks * info.block : width_blocks;
  const uint32_t row_bytes = host_row_units * host_block_bytes;
  const uint32_t upload_row_bytes = AlignUp(row_bytes, kUploadRowAlignment);
  const uint32_t upload_row_units = upload_row_bytes / host_block_bytes;
  const uint32_t upload_row_texels =
      IsBlockExpand(info.expand) ? upload_row_units : upload_row_units * info.block;

  // A widened format is read at its own packed guest pitch and unpacked into the
  // upload pitch afterwards; everything else is read straight into the layout the
  // upload wants.
  const bool expanding = info.expand != Expand::kNone;
  const uint32_t read_row_bytes =
      expanding ? width_blocks * info.block_bytes : upload_row_bytes;

  std::vector<uint8_t> texels;
  if (!ReadTexels(GuestPhysicalPointer(memory_base,
                                       fetch.base_address +
                                           uint32_t(Level0ByteOffset(fetch, info)),
                                       SourceExtentBytes(fetch, info)),
                  fetch, info,
                  read_row_bytes, texels)) {
    // ReadTexels has already counted why, so this does not count it again.
    // Only on the first attempt at an address: a retry that fails again is the
    // expected case for a slot holding a stale fetch constant, and logging it
    // would be one line per bind forever.
    if (g_count_refusals && g_refused_unmapped + g_refused_extent <= 8) {
      const uint64_t wanted = SourceExtentBytes(fetch, info);
      const uint32_t offset = fetch.base_address & 0x1FFFFFFFu;
      REXLOG_INFO(
          "native_renderer: texture mirror refused {}x{} fmt {} {} pitch {} at 0x{:08X}, "
          "of {} source byte(s) readable: A={} C={} E={} 0={} 8={}",
          fetch.width, fetch.height, fetch.format, fetch.tiled ? "tiled" : "linear", fetch.pitch,
          fetch.base_address, wanted,
          GuestRangeReadableBytes(memory_base + (0xA0000000u | offset), wanted),
          GuestRangeReadableBytes(memory_base + (0xC0000000u | offset), wanted),
          GuestRangeReadableBytes(memory_base + (0xE0000000u | (offset - kEApertureShift)), wanted),
          GuestRangeReadableBytes(memory_base + offset, wanted),
          GuestRangeReadableBytes(memory_base + (0x80000000u | offset), wanted));
    }
    return nullptr;
  }

  if (IsBlockExpand(info.expand)) {
    std::vector<uint8_t> decoded;
    DecodeBlockRows(texels, read_row_bytes, width_blocks, height_blocks, info.expand,
                    upload_row_bytes, decoded);
    texels.swap(decoded);
  } else if (expanding) {
    // What the 16 bit units actually look like, once, per format.
    //
    // Brownish, low contrast output has two possible causes that argue the same
    // way and are only separable from the bytes: a unit that was never byte
    // swapped, so the channels straddle the wrong bit fields, or the right value
    // with red and blue the wrong way round. Both raw and swapped are printed
    // with what each would decode to, so the log says which reading is a picture
    // and which is noise.
    static bool reported[2] = {false, false};
    const size_t which = info.expand == Expand::k5_6_5 ? 0 : 1;
    if (!reported[which] && texels.size() >= 16) {
      reported[which] = true;
      std::string raw;
      std::string swapped;
      for (uint32_t i = 0; i < 8; ++i) {
        uint16_t value = 0;
        std::memcpy(&value, texels.data() + size_t(i) * 2, 2);
        const uint16_t other = uint16_t((value >> 8) | (value << 8));
        raw += fmt::format(" {:04X}(r{} g{} b{})", value, (value >> 11) & 0x1F, (value >> 5) & 0x3F,
                           value & 0x1F);
        swapped += fmt::format(" {:04X}(r{} g{} b{})", other, (other >> 11) & 0x1F,
                               (other >> 5) & 0x3F, other & 0x1F);
      }
      REXLOG_WARN(
          "native_renderer: expand fmt {} {}x{} {} pitch {} endian {} at 0x{:08X}\n"
          "  as read: {}\n"
          "  swapped:{}",
          fetch.format, fetch.width, fetch.height, fetch.tiled ? "tiled" : "linear", fetch.pitch,
          fetch.endianness, fetch.base_address, raw, swapped);
    }

    std::vector<uint8_t> widened;
    ExpandRows(texels, read_row_bytes, width_blocks, height_blocks, info.expand, upload_row_bytes,
               widened);
    texels.swap(widened);
  }

  std::unique_ptr<RenderTexture> created;
  if (existing == nullptr) {
    created = device->createTexture(
        RenderTextureDesc::Texture2D(fetch.width, fetch.height, 1, info.host));
    if (!created) {
      CountRefusal(g_refused_upload);
      return nullptr;
    }
    existing = created.get();
  }

  auto staging = device->createBuffer(RenderBufferDesc::UploadBuffer(texels.size()));
  if (!staging) {
    CountRefusal(g_refused_upload);
    return nullptr;
  }
  auto* mapped = static_cast<uint8_t*>(staging->map());
  if (mapped == nullptr) {
    CountRefusal(g_refused_upload);
    return nullptr;
  }
  std::memcpy(mapped, texels.data(), texels.size());
  staging->unmap();

  // A one-shot list, not the frame's, for the same reason the overlay's upload
  // uses one: this runs from inside a draw, and the frame's list is mid-record.
  auto upload = queue->createCommandList();
  auto fence = device->createCommandFence();
  upload->begin();
  upload->barriers(RenderBarrierStage::COPY,
                   RenderTextureBarrier(existing, RenderTextureLayout::COPY_DEST));
  upload->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(existing),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), info.host, fetch.width,
                                                 fetch.height, 1, upload_row_texels));
  upload->barriers(RenderBarrierStage::GRAPHICS,
                   RenderTextureBarrier(existing, RenderTextureLayout::SHADER_READ));
  upload->end();

  const RenderCommandList* submit = upload.get();
  queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0, fence.get());
  queue->waitForCommandFence(fence.get());

  ++g_decoded;
  if (g_decoded <= 8) {
    REXLOG_INFO(
        "native_renderer: texture mirror decoded {}x{} fmt {} {} pitch {} endian {} at 0x{:08X}",
        fetch.width, fetch.height, fetch.format, fetch.tiled ? "tiled" : "linear", fetch.pitch,
        fetch.endianness, fetch.base_address);
  }

  // On a refresh there is no new object to hand back; the caller keeps the one
  // it already has, which now holds the new contents.
  return created;
}

}  // namespace

// The source pointer for an entry the mirror already holds, resolving the
// aperture at most once every kApertureRevalidateFrames. See the fields on
// MirroredTexture for why this is cached at all.
const uint8_t* EntrySourcePointer(MirroredTexture* entry, uint8_t* memory_base,
                                  const TextureFetch& fetch) {
  const bool stale = entry->source_pointer == nullptr ||
                     entry->source_pointer_frame == ~0ull ||
                     g_frame - entry->source_pointer_frame >= kApertureRevalidateFrames;
  if (stale) {
    ++g_aperture_walks;
    entry->source_pointer = GuestPhysicalPointer(
        memory_base, fetch.base_address + entry->level0_offset, entry->source_bytes);
    entry->source_pointer_frame = g_frame;
  } else {
    ++g_aperture_reuses;
  }
  return entry->source_pointer;
}

void* TextureMirrorLookup(uint8_t* memory_base, const TextureFetch& fetch) {
  if (memory_base == nullptr || fetch.base_address == 0)
    return nullptr;

  // A resolve destination first: the frame layer already owns the image, and
  // the guest memory behind that address holds nothing the host wrote.
  if (void* resolved =
          FrameResolveTextureByAddress(fetch.base_address, fetch.width, fetch.height)) {
    ++g_resolve_hits;
    return resolved;
  }

  if (fetch.width > kMaxTextureExtent || fetch.height > kMaxTextureExtent) {
    CountRefusal(g_refused_extent);
    return nullptr;
  }

  FormatInfo info;
  if (!MapTextureFormat(fetch, info)) {
    ++g_refused_format;
    if (fetch.format < 64 && !g_format_reported[fetch.format]) {
      g_format_reported[fetch.format] = true;
      REXLOG_INFO("native_renderer: texture mirror has no host format for guest format {}",
                  fetch.format);
    }
    return nullptr;
  }

  const uint64_t key = TextureKey(fetch.base_address, fetch.format, fetch.width, fetch.height);
  const auto found = g_texture_index.find(key);
  if (found != g_texture_index.end()) {
    MirroredTexture* candidate = found->second;
    ++g_decode_hits;

    // A failed decode is remembered, but it is *not* final. The reason is
    // usually that the source was not readable yet, and the game streams a
    // texture into a buffer some frames before it first samples it, so the very
    // first bind of an asset can lose a race the second bind would win. Caching
    // the failure permanently turns that race into a texture that is white for
    // the rest of the run, which is what the two intro logos were.
    //
    // Retried on every bind rather than on a timer: the readability walk is a
    // couple of VirtualQuery calls and is cheaper than the content hash the
    // success path below already pays. The refusal counters are frozen for the
    // retry so they stay a count of distinct textures, not of binds.
    if (!candidate->texture) {
      ++g_retries;
      g_count_refusals = false;
      candidate->texture = DecodeAndUpload(memory_base, fetch, info, nullptr);
      g_count_refusals = true;
      if (!candidate->texture)
        return nullptr;
      ++g_recovered;
      candidate->hashed_frame = g_frame;
      candidate->content_hash = HashSource(EntrySourcePointer(candidate, memory_base, fetch),
                                           candidate->source_bytes);
      return candidate->texture.get();
    }

    // Has the guest replaced what is at this address? This is the check that
    // was missing, and its absence is what showed a cutscene's texture as a
    // menu background: the game reuses a buffer rather than allocating a new
    // one, so the address, format and extent all match while the contents do
    // not. It also catches the font atlas being filled a glyph at a time, which
    // is why the hash has to be complete rather than sampled.
    //
    // Once per frame: the hash reads the whole source, and this texture may be
    // bound hundreds of times before the frame ends.
    if (candidate->hashed_frame != g_frame) {
      candidate->hashed_frame = g_frame;
      // Before the hash rather than before the decode the hash may trigger: the
      // hash is what decides whether the contents changed, so hashing memory the
      // readback has not filled in yet would conclude that a render target that
      // changes every frame never changes at all.
      ReadbackFillForRead(fetch);
      const uint64_t hash = HashSource(EntrySourcePointer(candidate, memory_base, fetch),
                                       candidate->source_bytes);
      if (hash != candidate->content_hash) {
        ++g_refreshed;
        DecodeAndUpload(memory_base, fetch, info, candidate->texture.get());
        candidate->content_hash = hash;
      }
    }
    return candidate->texture.get();
  }

  auto entry = std::make_unique<MirroredTexture>();
  entry->address = fetch.base_address;
  entry->format = fetch.format;
  entry->width = fetch.width;
  entry->height = fetch.height;
  entry->source_bytes = SourceExtentBytes(fetch, info);
  entry->level0_offset = uint32_t(Level0ByteOffset(fetch, info));
  entry->texture = DecodeAndUpload(memory_base, fetch, info, nullptr);

  // Hashed after the upload rather than before, so a source that changed
  // between the two is noticed on the next bind instead of being recorded as
  // already current.
  if (entry->texture) {
    entry->hashed_frame = g_frame;
    entry->content_hash =
        HashSource(EntrySourcePointer(entry.get(), memory_base, fetch), entry->source_bytes);
  }

  RenderTexture* result = entry->texture.get();
  g_texture_index.emplace(key, entry.get());
  g_textures.push_back(std::move(entry));
  return result;
}

void LogTextureMirrorSummary() {
  REXLOG_INFO(
      "native_renderer: textures decoded={} cached={} refreshed={} hashed={} MiB | "
      "binds resolve={} cache={} | "
      "refused format={} extent={} unmapped={} upload={} | retries={} recovered={}",
      g_decoded, g_textures.size(), g_refreshed, g_hash_bytes / (1024 * 1024), g_resolve_hits,
      g_decode_hits, g_refused_format, g_refused_extent, g_refused_unmapped, g_refused_upload,
      g_retries, g_recovered);

  REXLOG_INFO("native_renderer:   aperture walks={} reused={}", g_aperture_walks,
              g_aperture_reuses);
}

void TextureMirrorOccupiedRanges(uint32_t address, uint64_t bytes, uint32_t expected_address,
                                 uint32_t expected_width, uint32_t expected_height,
                                 std::vector<MirrorOccupiedRange>* out) {
  if (out == nullptr || bytes == 0)
    return;
  const uint64_t end = uint64_t(address) + bytes;
  for (const std::unique_ptr<MirroredTexture>& entry : g_textures) {
    if (entry == nullptr || entry->source_bytes == 0)
      continue;
    const uint64_t entry_end = uint64_t(entry->address) + entry->source_bytes;
    if (entry_end <= address || entry->address >= end)
      continue;
    // Overlaps. The destination's own image is not a conflict with itself: the
    // guest binds a resolved thumbnail as a texture, which is the whole reason
    // a resolve destination is reachable from here at all.
    //
    // Matched on the base address alone. Requiring the extent to agree as well
    // made the save preview black: an entry sat at exactly the destination's
    // address at a different extent and clipped 204800 of 204800 bytes, so a
    // faithful readback buffer was never written to guest memory. A bind that
    // does agree is served by FrameResolveTextureByAddress and never reaches
    // this cache (mirror pulls=0 for that destination), so an entry here is by
    // construction one whose extent differs, and the resolve the guest just
    // asked for is the authority on what those pixels are.
    if (entry->address == expected_address) {
      if (entry->width != expected_width || entry->height != expected_height) {
        static uint32_t reported = 0;
        if (reported < 8) {
          ++reported;
          REXLOG_INFO(
              "native_renderer: readback fill overwrites cached texture 0x{:08X} {}x{} with the "
              "{}x{} resolve destination at the same address",
              entry->address, entry->width, entry->height, expected_width, expected_height);
        }
      }
      continue;
    }
    // Nothing is asked about when the texture was decoded. That test used to be
    // here -- a texture cached before the resolve was treated as stale -- and it
    // is not true: a resolve writes the host render target and the readback
    // buffer, never guest memory, so a cached texture overlapping a destination
    // is live guest data whichever happened first.
    const uint64_t begin = (uint64_t(entry->address) > address ? uint64_t(entry->address) : address)
                           - address;
    const uint64_t stop = (entry_end < end ? entry_end : end) - address;
    out->push_back({begin, stop});
  }

  if (out->size() < 2)
    return;
  std::sort(out->begin(), out->end(),
            [](const MirrorOccupiedRange& a, const MirrorOccupiedRange& b) {
              return a.begin < b.begin;
            });
  size_t kept = 0;
  for (size_t i = 1; i < out->size(); ++i) {
    if ((*out)[i].begin <= (*out)[kept].end) {
      if ((*out)[i].end > (*out)[kept].end)
        (*out)[kept].end = (*out)[i].end;
    } else {
      (*out)[++kept] = (*out)[i];
    }
  }
  out->resize(kept + 1);
}

void TextureMirrorBeginFrame() { ++g_frame; }

void ShutdownTextureMirror() {
  g_texture_index.clear();
  g_textures.clear();
}

}  // namespace eternalsonata
