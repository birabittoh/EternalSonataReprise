// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_texture.h.

#include "native_renderer_texture.h"

#include <cstring>
#include <memory>
#include <vector>

#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "native_renderer_frame.h"
#include "native_renderer_plume_internal.h"

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
// decode. Once per cache miss, not once per bind.
bool GuestRangeReadable(const uint8_t* start, uint64_t bytes) {
  if (start == nullptr || bytes == 0)
    return false;
#ifdef _WIN32
  const uint8_t* cursor = start;
  const uint8_t* end = start + bytes;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(cursor, &info, sizeof(info)) == 0)
      return false;
    if (info.State != MEM_COMMIT)
      return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD) != 0)
      return false;
    cursor = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  return true;
#endif
}

// A host pointer to guest physical memory.
//
// The bare physical address a fetch constant carries is not mapped: the host
// backs physical memory only through the 360's virtual apertures, which was
// established by probing 0x00000000, 0x80000000, 0xA0000000, 0xC0000000 and
// 0xE0000000 against a texture that would not read -- the first two are
// unmapped and the last three all alias the same pages.
//
// 0xA0000000 is the cached one, and any of the three would give identical
// bytes. It is derived from the *raw* fetch field rather than the fixed-up
// address, because the fixup's 0x1000 exists only to make an E-aperture
// resource compare equal to its resolve destination and would be a page of
// skew here.
const uint8_t* GuestPhysicalPointer(uint8_t* memory_base, uint32_t raw_base_address) {
  const uint32_t aliased = 0xA0000000u | (raw_base_address & 0x1FFFFFFFu);
  return memory_base + aliased;
}

// ---------------------------------------------------------------------------
// Formats

// What a guest texture format costs and what it becomes on the host.
//
// `block` is the edge of one addressable unit in texels, which is 1 for an
// uncompressed format and 4 for the DXT ones. Tiling works in these units, not
// in texels, which is the whole reason it is carried here.
struct FormatInfo {
  RenderFormat host = RenderFormat::UNKNOWN;
  uint32_t block_bytes = 0;
  uint32_t block = 1;
};

bool MapTextureFormat(uint32_t format, FormatInfo& out) {
  switch (format) {
    // k_8_8_8_8. Taken as BGRA rather than RGBA to match the frame layer's own
    // colour targets, which are B8G8R8A8_UNORM: a resolve destination and an
    // asset both arrive here as format 6, and they cannot disagree about
    // channel order. See the caveat on the fetch constant swizzle below.
    case 6:
      out = {RenderFormat::B8G8R8A8_UNORM, 4, 1};
      return true;
    case 18:  // k_DXT1
      out = {RenderFormat::BC1_UNORM, 8, 4};
      return true;
    case 19:  // k_DXT2_3
      out = {RenderFormat::BC2_UNORM, 16, 4};
      return true;
    case 20:  // k_DXT4_5
      out = {RenderFormat::BC3_UNORM, 16, 4};
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
};

std::vector<std::unique_ptr<MirroredTexture>> g_textures;

uint64_t g_resolve_hits = 0;
uint64_t g_decode_hits = 0;
uint64_t g_decoded = 0;
uint64_t g_refused_format = 0;
uint64_t g_refused_extent = 0;
uint64_t g_refused_upload = 0;
uint64_t g_refused_unmapped = 0;

// Report each unhandled guest format once. There are only 64 of them, so a flat
// array is cheaper than deciding whether it is worth being cleverer.
bool g_format_reported[64] = {};

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

    // What the tiled layout can address, which is what bounds the read. The
    // last block of the last row is not the highest offset the swizzle
    // produces, so bound by the whole macro tile grid instead of by that.
    const uint32_t macro_rows = (height_blocks + 31) / 32;
    const uint64_t source_bytes =
        uint64_t(pitch_macro_tiles) * macro_rows * 32 * 32 * info.block_bytes;
    if (source_bytes > kMaxTextureBytes) {
      ++g_refused_extent;
      return false;
    }
    if (!GuestRangeReadable(source, source_bytes)) {
      ++g_refused_unmapped;
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
    const uint64_t source_bytes = uint64_t(source_row_bytes) * height_blocks;
    if (source_bytes > kMaxTextureBytes) {
      ++g_refused_extent;
      return false;
    }
    if (!GuestRangeReadable(source, source_bytes)) {
      ++g_refused_unmapped;
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

// Decode and upload, returning the host texture or null. Only ever called on a
// cache miss.
std::unique_ptr<RenderTexture> CreateMirroredTexture(uint8_t* memory_base,
                                                     const TextureFetch& fetch,
                                                     const FormatInfo& info) {
  RenderDevice* device = PlumeDevice();
  RenderCommandQueue* queue = PlumeQueue();
  if (device == nullptr || queue == nullptr)
    return nullptr;

  const uint32_t width_blocks = (fetch.width + info.block - 1) / info.block;
  const uint32_t height_blocks = (fetch.height + info.block - 1) / info.block;

  // Plume takes a placed footprint's row width in *texels* and derives the row
  // pitch as ceil(rowWidth / block) * block_bytes, so the alignment has to be
  // chosen in blocks and handed back as texels.
  const uint32_t row_bytes = width_blocks * info.block_bytes;
  const uint32_t upload_row_bytes = AlignUp(row_bytes, kUploadRowAlignment);
  const uint32_t upload_row_blocks = upload_row_bytes / info.block_bytes;
  const uint32_t upload_row_texels = upload_row_blocks * info.block;

  std::vector<uint8_t> texels;
  if (!ReadTexels(GuestPhysicalPointer(memory_base, fetch.raw_base_address), fetch, info,
                  upload_row_bytes, texels)) {
    // ReadTexels has already counted why, so this does not count it again.
    if (g_refused_unmapped + g_refused_extent <= 8) {
      REXLOG_INFO(
          "native_renderer: texture mirror refused {}x{} fmt {} {} pitch {} at 0x{:08X}",
          fetch.width, fetch.height, fetch.format, fetch.tiled ? "tiled" : "linear", fetch.pitch,
          fetch.base_address);
    }
    return nullptr;
  }

  auto texture = device->createTexture(
      RenderTextureDesc::Texture2D(fetch.width, fetch.height, 1, info.host));
  if (!texture) {
    ++g_refused_upload;
    return nullptr;
  }

  auto staging = device->createBuffer(RenderBufferDesc::UploadBuffer(texels.size()));
  if (!staging) {
    ++g_refused_upload;
    return nullptr;
  }
  auto* mapped = static_cast<uint8_t*>(staging->map());
  if (mapped == nullptr) {
    ++g_refused_upload;
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
                   RenderTextureBarrier(texture.get(), RenderTextureLayout::COPY_DEST));
  upload->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), info.host, fetch.width,
                                                 fetch.height, 1, upload_row_texels));
  upload->barriers(RenderBarrierStage::GRAPHICS,
                   RenderTextureBarrier(texture.get(), RenderTextureLayout::SHADER_READ));
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
  return texture;
}

}  // namespace

void* TextureMirrorLookup(uint8_t* memory_base, const TextureFetch& fetch) {
  if (memory_base == nullptr || fetch.base_address == 0)
    return nullptr;

  // A resolve destination first: the frame layer already owns the image, and
  // the guest memory behind that address holds nothing the host wrote.
  if (void* resolved = FrameResolveTextureByAddress(fetch.base_address)) {
    ++g_resolve_hits;
    return resolved;
  }

  if (fetch.width > kMaxTextureExtent || fetch.height > kMaxTextureExtent) {
    ++g_refused_extent;
    return nullptr;
  }

  FormatInfo info;
  if (!MapTextureFormat(fetch.format, info)) {
    ++g_refused_format;
    if (fetch.format < 64 && !g_format_reported[fetch.format]) {
      g_format_reported[fetch.format] = true;
      REXLOG_INFO("native_renderer: texture mirror has no host format for guest format {}",
                  fetch.format);
    }
    return nullptr;
  }

  for (auto& candidate : g_textures) {
    if (candidate->address == fetch.base_address && candidate->format == fetch.format &&
        candidate->width == fetch.width && candidate->height == fetch.height) {
      ++g_decode_hits;
      // A failed decode is cached as a null entry so it is not retried on every
      // bind; the counters above already recorded why it failed.
      return candidate->texture.get();
    }
  }

  auto entry = std::make_unique<MirroredTexture>();
  entry->address = fetch.base_address;
  entry->format = fetch.format;
  entry->width = fetch.width;
  entry->height = fetch.height;
  entry->texture = CreateMirroredTexture(memory_base, fetch, info);

  RenderTexture* result = entry->texture.get();
  g_textures.push_back(std::move(entry));
  return result;
}

void LogTextureMirrorSummary() {
  REXLOG_INFO(
      "native_renderer: textures decoded={} cached={} | binds resolve={} cache={} | refused "
      "format={} extent={} unmapped={} upload={}",
      g_decoded, g_textures.size(), g_resolve_hits, g_decode_hits, g_refused_format,
      g_refused_extent, g_refused_unmapped, g_refused_upload);
}

void ShutdownTextureMirror() { g_textures.clear(); }

}  // namespace eternalsonata
