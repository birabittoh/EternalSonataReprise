// eternalsonata - Texture patches for the granular asset API.
//
// See eternalsonata_asset_texture.h for the shape of this, and the comments on
// TiledBlockOffset and MipLevelBytes below for where the layout rules come from
// and what they were checked against.

#include "eternalsonata_asset_texture.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <rex/ui/image_decode.h>

namespace eternalsonata::assets {
namespace {

uint32_t ReadU32BE(const uint8_t* d) {
  return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | uint32_t(d[3]);
}

uint32_t ReadU32LE(const uint8_t* d) {
  return uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}

uint32_t Log2Ceil(uint32_t value) {
  uint32_t bits = 0;
  while ((1u << bits) < value)
    ++bits;
  return bits;
}

uint32_t BlockBytes(TextureFormat format) {
  switch (format) {
    case TextureFormat::kDXT1:
      return 8;
    case TextureFormat::kDXT3:
    case TextureFormat::kDXT5:
      return 16;
    case TextureFormat::kRGBA8:
      return 4;
    default:
      return 0;
  }
}

bool IsBlockCompressed(TextureFormat format) {
  return format == TextureFormat::kDXT1 || format == TextureFormat::kDXT3 ||
         format == TextureFormat::kDXT5;
}

// ---------------------------------------------------------------------------
// Tiling
//
// The Xenos 2D tiled address in units of one addressable block, and the endian
// unit swap that goes with it. Both are the same functions the native renderer
// reads guest textures with (src/native_renderer_texture.cpp); a splice is that
// path run backwards, since the address map is a bijection.
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
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Mip layout
//
// A level occupies its whole macro-tile grid: the pitch is padded up to 32
// blocks and so is the height. Levels follow each other with no further
// alignment, and levels at and below 16 texels share one packed tail tile.
//
// Checked against the shipped data before any of this was written: over the
// 5426 NTX2 chunks in the 678 containers, level 0 plus this chain accounts for
// the chunk's pixel region exactly in 5422. The four that disagree are
// non-power-of-two (1600x1600, 2048x1028, 8x256), and ApplyTextureEdit refuses
// them rather than guessing.
uint32_t MipLevelBytes(uint32_t width, uint32_t height, TextureFormat format) {
  const uint32_t block = IsBlockCompressed(format) ? 4 : 1;
  const uint32_t width_blocks = (width + block - 1) / block;
  const uint32_t height_blocks = (height + block - 1) / block;
  const uint32_t pitch_macro_tiles = std::max(1u, ((width_blocks + 31) / 32 * 32) / 32);
  return pitch_macro_tiles * 32 * ((height_blocks + 31) / 32 * 32) * BlockBytes(format);
}

uint32_t MipPitchMacroTiles(uint32_t width, TextureFormat format) {
  const uint32_t block = IsBlockCompressed(format) ? 4 : 1;
  const uint32_t width_blocks = (width + block - 1) / block;
  return std::max(1u, ((width_blocks + 31) / 32 * 32) / 32);
}

// The level the packed mip tail starts at, or 0 when the whole chain is packed.
uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Block offset of a packed level inside the tail tile. Ported from xenia's
// texture_util.cc GetPackedMipOffset, 2D only.
void PackedMipOffset(uint32_t width, uint32_t height, TextureFormat format, uint32_t mip,
                     uint32_t& x_blocks, uint32_t& y_blocks) {
  const uint32_t log2_width = Log2Ceil(width);
  const uint32_t log2_height = Log2Ceil(height);
  const uint32_t packed_base = PackedMipLevel(width, height);
  const uint32_t packed_mip = mip - packed_base;

  uint32_t x = 0, y = 0;
  if (packed_mip < 3) {
    if (log2_width > log2_height)
      y = 16u >> packed_mip;  // wider than tall: laid out vertically
    else
      x = 16u >> packed_mip;
  } else {
    if (log2_width > log2_height)
      x = (1u << (log2_width - packed_base)) >> (packed_mip - 2);
    else
      y = (1u << (log2_height - packed_base)) >> (packed_mip - 2);
  }

  const uint32_t shift = IsBlockCompressed(format) ? 2 : 0;
  x_blocks = x >> shift;
  y_blocks = y >> shift;
}

// ---------------------------------------------------------------------------
// Chunk parsing
TextureFormat FormatFromFetchConstant(uint32_t id) {
  switch (id) {
    case 6:
      return TextureFormat::kRGBA8;
    case 18:
      return TextureFormat::kDXT1;
    case 19:
      return TextureFormat::kDXT3;
    case 20:
      return TextureFormat::kDXT5;
    default:
      return TextureFormat::kUnknown;
  }
}

bool ParseNtx2(const std::vector<uint8_t>& data, size_t offset, TextureRef& out) {
  const size_t available = data.size() - offset;
  if (available < 0x80)
    return false;
  const uint8_t* d = data.data() + offset;
  if (std::memcmp(d + 0x08, "XPR2", 4) != 0 || std::memcmp(d + 0x18, "TX2D", 4) != 0)
    return false;

  const uint32_t chunk_size = ReadU32BE(d + 0x04);
  const uint32_t xpr2_header_size = ReadU32BE(d + 0x0C);
  const uint32_t pix_size = ReadU32BE(d + 0x10);
  const uint32_t name_size = ReadU32BE(d + 0x1C);
  if (chunk_size < 0x80 || chunk_size > available || pix_size == 0 || pix_size > chunk_size)
    return false;

  // The pixel data starts at the first 4 KB-aligned *absolute* offset at or
  // after the XPR2 header, which is why this is a property of where the chunk
  // sits rather than of the chunk alone.
  const size_t header_end = offset + 0x08 + xpr2_header_size;
  const size_t pix_absolute = (header_end + 0xFFFu) & ~size_t{0xFFF};
  if (pix_absolute < offset || pix_absolute - offset + pix_size > chunk_size)
    return false;

  const size_t fetch = 0x18 + size_t(name_size) + 0x18;
  if (fetch + 24 > available)
    return false;
  const uint32_t w0 = ReadU32BE(d + fetch);
  const uint32_t w1 = ReadU32BE(d + fetch + 4);
  const uint32_t w2 = ReadU32BE(d + fetch + 8);

  out.format = FormatFromFetchConstant(w1 & 0x3F);
  if (out.format == TextureFormat::kUnknown)
    return false;
  out.width = (w2 & 0x1FFF) + 1;
  out.height = ((w2 >> 13) & 0x1FFF) + 1;
  if (out.width > 8192 || out.height > 8192)
    return false;
  out.endianness = (w1 >> 6) & 3;
  out.tiled = ((w0 >> 31) & 1) != 0;
  // W0's pitch field is in texels >> 5, and is padded up to a macro tile, so a
  // 64px-wide DXT1 reports 1 rather than the 0 that width_blocks >> 5 gives.
  out.pitch_macro_tiles = std::max(1u, (((w0 >> 22) & 0x1FF) * 32 / 4) / 32);

  out.chunk_offset = offset;
  out.chunk_size = chunk_size;
  out.pix_offset = uint32_t(pix_absolute - offset);
  out.pix_size = pix_size;
  out.ntex = false;

  const char* name = reinterpret_cast<const char*>(d + 0x2C);
  const size_t max = std::min<size_t>(32, available - 0x2C);
  size_t length = 0;
  while (length < max && name[length] != '\0')
    ++length;
  out.name.assign(name, length);
  return true;
}

bool ParseNtex(const std::vector<uint8_t>& data, size_t offset, TextureRef& out) {
  const size_t available = data.size() - offset;
  if (available < 8 + 128)
    return false;
  const uint8_t* d = data.data() + offset;
  const uint32_t chunk_size = ReadU32BE(d + 0x04);
  if (chunk_size < 8 + 128 || chunk_size > available)
    return false;

  const uint8_t* dds = d + 8;
  if (std::memcmp(dds, "DDS ", 4) != 0)
    return false;
  out.height = ReadU32LE(dds + 12);
  out.width = ReadU32LE(dds + 16);
  if (out.width == 0 || out.height == 0 || out.width > 8192 || out.height > 8192)
    return false;

  const uint32_t pixel_flags = ReadU32LE(dds + 80);
  char fourcc[4];
  std::memcpy(fourcc, dds + 84, 4);
  if (pixel_flags & 0x4) {
    if (std::memcmp(fourcc, "DXT1", 4) == 0)
      out.format = TextureFormat::kDXT1;
    else if (std::memcmp(fourcc, "DXT3", 4) == 0)
      out.format = TextureFormat::kDXT3;
    else if (std::memcmp(fourcc, "DXT5", 4) == 0)
      out.format = TextureFormat::kDXT5;
    else
      return false;
  } else if (ReadU32LE(dds + 88) == 32) {
    out.format = TextureFormat::kRGBA8;
  } else {
    return false;  // the 16bpp light and shadow maps, which nothing references
  }

  out.chunk_offset = offset;
  out.chunk_size = chunk_size;
  out.ntex = true;
  out.tiled = false;
  out.endianness = 0;
  out.pix_offset = 8 + 128;
  out.pix_size = chunk_size - out.pix_offset;
  out.pitch_macro_tiles = 1;
  return true;
}

// ---------------------------------------------------------------------------
// Encoding
//
// A compact BC1/BC2/BC3 encoder: the block's colour bounding box, inset once,
// as the two endpoints. Good enough for the art a mod replaces and small enough
// to keep the whole texture path free of dependencies.
uint16_t PackRgb565(uint32_t r, uint32_t g, uint32_t b) {
  return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void UnpackRgb565(uint16_t value, int32_t rgb[3]) {
  const uint32_t r = (value >> 11) & 0x1F;
  const uint32_t g = (value >> 5) & 0x3F;
  const uint32_t b = value & 0x1F;
  rgb[0] = int32_t((r << 3) | (r >> 2));
  rgb[1] = int32_t((g << 2) | (g >> 4));
  rgb[2] = int32_t((b << 3) | (b >> 2));
}

// `block` is 16 RGBA8 texels, row major. Writes the 8-byte BC1 colour block.
// `punchthrough` selects the 3-colour mode, where index 3 is transparent black.
void EncodeColourBlock(const uint8_t block[64], bool punchthrough, uint8_t out[8]) {
  int32_t low[3] = {255, 255, 255};
  int32_t high[3] = {0, 0, 0};
  bool any = false;
  for (uint32_t i = 0; i < 16; ++i) {
    if (punchthrough && block[i * 4 + 3] < 128)
      continue;
    any = true;
    for (uint32_t c = 0; c < 3; ++c) {
      low[c] = std::min(low[c], int32_t(block[i * 4 + c]));
      high[c] = std::max(high[c], int32_t(block[i * 4 + c]));
    }
  }
  if (!any) {
    low[0] = low[1] = low[2] = 0;
    high[0] = high[1] = high[2] = 0;
  }

  // Inset the box by an eighth of its range. The endpoints are quantised and
  // the interpolants sit at 1/3 and 2/3, so the extremes are better served by
  // pulling in slightly than by sitting exactly on the outliers.
  for (uint32_t c = 0; c < 3; ++c) {
    const int32_t inset = (high[c] - low[c]) >> 4;
    low[c] = std::min(low[c] + inset, 255);
    high[c] = std::max(high[c] - inset, 0);
  }

  uint16_t c0 = PackRgb565(uint32_t(high[0]), uint32_t(high[1]), uint32_t(high[2]));
  uint16_t c1 = PackRgb565(uint32_t(low[0]), uint32_t(low[1]), uint32_t(low[2]));
  // The mode is chosen by the ordering of the endpoints, so it has to hold even
  // when both quantise to the same value.
  if (punchthrough) {
    if (c0 > c1)
      std::swap(c0, c1);
    if (c0 == c1 && c0 != 0)
      --c0;
  } else {
    if (c0 < c1)
      std::swap(c0, c1);
    if (c0 == c1) {
      if (c1 != 0)
        --c1;
      else
        c0 = 1;
    }
  }

  int32_t palette[4][3];
  UnpackRgb565(c0, palette[0]);
  UnpackRgb565(c1, palette[1]);
  for (uint32_t c = 0; c < 3; ++c) {
    if (punchthrough) {
      palette[2][c] = (palette[0][c] + palette[1][c]) / 2;
      palette[3][c] = 0;
    } else {
      palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
      palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
    }
  }

  uint32_t indices = 0;
  const uint32_t candidates = punchthrough ? 3 : 4;
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t best = 0;
    if (punchthrough && block[i * 4 + 3] < 128) {
      best = 3;
    } else {
      int32_t best_error = INT32_MAX;
      for (uint32_t p = 0; p < candidates; ++p) {
        int32_t error = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const int32_t delta = int32_t(block[i * 4 + c]) - palette[p][c];
          error += delta * delta;
        }
        if (error < best_error) {
          best_error = error;
          best = p;
        }
      }
    }
    indices |= best << (i * 2);
  }

  out[0] = uint8_t(c0 & 0xFF);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1 & 0xFF);
  out[3] = uint8_t(c1 >> 8);
  out[4] = uint8_t(indices & 0xFF);
  out[5] = uint8_t((indices >> 8) & 0xFF);
  out[6] = uint8_t((indices >> 16) & 0xFF);
  out[7] = uint8_t((indices >> 24) & 0xFF);
}

void EncodeAlphaBlockBc3(const uint8_t block[64], uint8_t out[8]) {
  uint8_t low = 255, high = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    low = std::min(low, block[i * 4 + 3]);
    high = std::max(high, block[i * 4 + 3]);
  }
  out[0] = high;
  out[1] = low;

  uint64_t indices = 0;
  const int32_t range = int32_t(high) - int32_t(low);
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t index = 0;
    if (range > 0) {
      // The 8-value palette runs high, low, then six steps from high down.
      const int32_t step = ((int32_t(block[i * 4 + 3]) - int32_t(low)) * 7 + range / 2) / range;
      if (step == 7)
        index = 0;
      else if (step == 0)
        index = 1;
      else
        index = uint32_t(8 - step);
    }
    indices |= uint64_t(index) << (i * 3);
  }
  for (uint32_t i = 0; i < 6; ++i)
    out[2 + i] = uint8_t((indices >> (i * 8)) & 0xFF);
}

void EncodeAlphaBlockBc2(const uint8_t block[64], uint8_t out[8]) {
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t a0 = block[(i * 2) * 4 + 3] >> 4;
    const uint32_t a1 = block[(i * 2 + 1) * 4 + 3] >> 4;
    out[i] = uint8_t(a0 | (a1 << 4));
  }
}

// Gathers a 4x4 texel block, clamping at the edges so a level narrower than a
// block still produces a whole one.
void GatherBlock(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t bx, uint32_t by,
                 uint8_t out[64]) {
  for (uint32_t y = 0; y < 4; ++y) {
    const uint32_t sy = std::min(by * 4 + y, height - 1);
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t sx = std::min(bx * 4 + x, width - 1);
      std::memcpy(out + (y * 4 + x) * 4, pixels + (size_t(sy) * width + sx) * 4, 4);
    }
  }
}

void EncodeBlock(const uint8_t block[64], TextureFormat format, uint8_t* out) {
  switch (format) {
    case TextureFormat::kDXT1: {
      bool punchthrough = false;
      for (uint32_t i = 0; i < 16 && !punchthrough; ++i)
        punchthrough = block[i * 4 + 3] < 128;
      EncodeColourBlock(block, punchthrough, out);
      break;
    }
    case TextureFormat::kDXT3:
      EncodeAlphaBlockBc2(block, out);
      EncodeColourBlock(block, false, out + 8);
      break;
    case TextureFormat::kDXT5:
      EncodeAlphaBlockBc3(block, out);
      EncodeColourBlock(block, false, out + 8);
      break;
    default:
      break;
  }
}

// A box filter, which is what a mip chain of this kind of art wants and is the
// only thing the shipped chains could have been made with anyway.
std::vector<uint8_t> Downsample(const std::vector<uint8_t>& pixels, uint32_t width,
                                uint32_t height, uint32_t& out_width, uint32_t& out_height) {
  out_width = std::max(1u, width / 2);
  out_height = std::max(1u, height / 2);
  std::vector<uint8_t> out(size_t(out_width) * out_height * 4);
  for (uint32_t y = 0; y < out_height; ++y) {
    for (uint32_t x = 0; x < out_width; ++x) {
      const uint32_t x0 = std::min(x * 2, width - 1);
      const uint32_t x1 = std::min(x * 2 + 1, width - 1);
      const uint32_t y0 = std::min(y * 2, height - 1);
      const uint32_t y1 = std::min(y * 2 + 1, height - 1);
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t sum = pixels[(size_t(y0) * width + x0) * 4 + c] +
                             pixels[(size_t(y0) * width + x1) * 4 + c] +
                             pixels[(size_t(y1) * width + x0) * 4 + c] +
                             pixels[(size_t(y1) * width + x1) * 4 + c];
        out[(size_t(y) * out_width + x) * 4 + c] = uint8_t((sum + 2) / 4);
      }
    }
  }
  return out;
}

// The levels the chunk's pixel region holds, level 0 first.
struct MipChain {
  std::vector<std::vector<uint8_t>> levels;  // RGBA8
  std::vector<uint32_t> widths;
  std::vector<uint32_t> heights;
};

MipChain BuildMipChain(const SourceImage& image, uint32_t level_count) {
  MipChain chain;
  chain.levels.push_back(image.pixels);
  chain.widths.push_back(image.width);
  chain.heights.push_back(image.height);
  for (uint32_t level = 1; level < level_count; ++level) {
    uint32_t width = 0, height = 0;
    chain.levels.push_back(Downsample(chain.levels.back(), chain.widths.back(),
                                      chain.heights.back(), width, height));
    chain.widths.push_back(width);
    chain.heights.push_back(height);
  }
  return chain;
}

// Writes one level's blocks into `dest`, tiled, at `block_x`/`block_y` inside
// the level's own grid (non-zero only for the packed mip tail).
void WriteTiledLevel(uint8_t* dest, size_t dest_size, const uint8_t* pixels, uint32_t width,
                     uint32_t height, uint32_t block_x, uint32_t block_y,
                     uint32_t pitch_macro_tiles, const TextureRef& ref) {
  const uint32_t block_bytes = BlockBytes(ref.format);
  const uint32_t block_bytes_log2 = block_bytes == 8 ? 3 : 4;
  const uint32_t width_blocks = (width + 3) / 4;
  const uint32_t height_blocks = (height + 3) / 4;

  uint8_t block[64];
  uint8_t encoded[16];
  for (uint32_t by = 0; by < height_blocks; ++by) {
    for (uint32_t bx = 0; bx < width_blocks; ++bx) {
      GatherBlock(pixels, width, height, bx, by, block);
      EncodeBlock(block, ref.format, encoded);
      EndianSwapUnit(encoded, block_bytes, ref.endianness);
      const uint32_t offset =
          TiledBlockOffset(block_x + bx, block_y + by, pitch_macro_tiles, block_bytes_log2);
      if (size_t(offset) + block_bytes > dest_size)
        continue;
      std::memcpy(dest + offset, encoded, block_bytes);
    }
  }
}

void WriteLinearLevel(uint8_t* dest, size_t dest_size, const uint8_t* pixels, uint32_t width,
                      uint32_t height, TextureFormat format) {
  const uint32_t block_bytes = BlockBytes(format);
  const uint32_t width_blocks = (width + 3) / 4;
  const uint32_t height_blocks = (height + 3) / 4;

  uint8_t block[64];
  uint8_t encoded[16];
  for (uint32_t by = 0; by < height_blocks; ++by) {
    for (uint32_t bx = 0; bx < width_blocks; ++bx) {
      const size_t offset = (size_t(by) * width_blocks + bx) * block_bytes;
      if (offset + block_bytes > dest_size)
        return;
      GatherBlock(pixels, width, height, bx, by, block);
      EncodeBlock(block, format, encoded);
      std::memcpy(dest + offset, encoded, block_bytes);
    }
  }
}

// ---------------------------------------------------------------------------
// Source images
bool LoadDds(const std::vector<uint8_t>& bytes, SourceImage& out, std::string* error) {
  if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0)
    return false;
  const uint32_t height = ReadU32LE(bytes.data() + 12);
  const uint32_t width = ReadU32LE(bytes.data() + 16);
  const uint32_t pixel_flags = ReadU32LE(bytes.data() + 80);
  const uint32_t rgb_bits = ReadU32LE(bytes.data() + 88);
  if (pixel_flags & 0x4) {
    if (error)
      *error = "compressed .dds input is not supported; export the image as .png";
    return false;
  }
  if (rgb_bits != 32) {
    if (error)
      *error = "only a 32-bit uncompressed .dds is supported; export the image as .png";
    return false;
  }
  const size_t pixels = size_t(width) * height;
  if (bytes.size() < 128 + pixels * 4) {
    if (error)
      *error = "the .dds is shorter than its header claims";
    return false;
  }

  // The masks say which byte is which; the common cases are B8G8R8A8 and
  // R8G8B8A8, and anything else is rejected rather than guessed at.
  const uint32_t red_mask = ReadU32LE(bytes.data() + 92);
  const bool bgra = red_mask == 0x00FF0000;
  if (!bgra && red_mask != 0x000000FF) {
    if (error)
      *error = "the .dds channel masks are neither RGBA nor BGRA";
    return false;
  }

  out.width = width;
  out.height = height;
  out.pixels.resize(pixels * 4);
  const uint8_t* source = bytes.data() + 128;
  for (size_t i = 0; i < pixels; ++i) {
    out.pixels[i * 4 + 0] = bgra ? source[i * 4 + 2] : source[i * 4 + 0];
    out.pixels[i * 4 + 1] = source[i * 4 + 1];
    out.pixels[i * 4 + 2] = bgra ? source[i * 4 + 0] : source[i * 4 + 2];
    out.pixels[i * 4 + 3] = source[i * 4 + 3];
  }
  return true;
}

}  // namespace

const char* TextureFormatName(TextureFormat format) {
  switch (format) {
    case TextureFormat::kDXT1:
      return "DXT1";
    case TextureFormat::kDXT3:
      return "DXT3";
    case TextureFormat::kDXT5:
      return "DXT5";
    case TextureFormat::kRGBA8:
      return "RGBA8";
    default:
      return "unknown";
  }
}

std::vector<TextureRef> FindTextures(const std::vector<uint8_t>& data) {
  std::vector<TextureRef> found;
  if (data.size() < 0x80)
    return found;

  for (size_t offset = 0; offset + 8 <= data.size(); ++offset) {
    TextureRef ref;
    bool ok = false;
    if (std::memcmp(data.data() + offset, "NTX2", 4) == 0)
      ok = ParseNtx2(data, offset, ref);
    else if (std::memcmp(data.data() + offset, "NTEX", 4) == 0)
      ok = ParseNtex(data, offset, ref);
    if (!ok)
      continue;
    found.push_back(std::move(ref));
    // A chunk cannot contain another, and the pixel data is arbitrary bytes
    // that can hold either magic, so skip past what was just accepted.
    offset += found.back().chunk_size - 1;
  }
  return found;
}

bool LoadSourceImage(const std::filesystem::path& file, SourceImage& out, std::string* error) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    if (error)
      *error = "could not be read";
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0)
    return LoadDds(bytes, out, error);

  int width = 0, height = 0;
  auto pixels = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), width, height);
  if (pixels.empty() || width <= 0 || height <= 0) {
    if (error)
      *error = "is not a .png or an uncompressed .dds this build can read";
    return false;
  }
  out.width = uint32_t(width);
  out.height = uint32_t(height);
  out.pixels = std::move(pixels);
  return true;
}

EditStatus ApplyTextureEdit(std::vector<uint8_t>& data, const TextureRef& ref,
                            const SourceImage& image, std::string* error) {
  if (!IsBlockCompressed(ref.format)) {
    if (error)
      *error = "is not a DXT texture, which is the only kind this build can write";
    return EditStatus::kBadData;
  }
  if (image.width != ref.width || image.height != ref.height) {
    if (error) {
      *error = "is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
               ", but the chunk is " + std::to_string(ref.width) + "x" +
               std::to_string(ref.height) + "; a texture is spliced into the space it already has";
    }
    return EditStatus::kTooLarge;
  }
  if (image.pixels.size() < size_t(image.width) * image.height * 4)
    return EditStatus::kBadData;
  if (ref.chunk_offset + ref.pix_offset + ref.pix_size > data.size())
    return EditStatus::kBadData;

  uint8_t* dest = data.data() + ref.chunk_offset + ref.pix_offset;
  const size_t dest_size = ref.pix_size;

  if (ref.ntex || !ref.tiled) {
    // NTEX is a plain PC DDS: linear levels, no swap, mip chain packed tight.
    uint32_t level_width = ref.width, level_height = ref.height;
    size_t offset = 0;
    std::vector<uint8_t> level = image.pixels;
    while (offset < dest_size) {
      const uint32_t width_blocks = (level_width + 3) / 4;
      const uint32_t height_blocks = (level_height + 3) / 4;
      const size_t level_bytes = size_t(width_blocks) * height_blocks * BlockBytes(ref.format);
      if (offset + level_bytes > dest_size)
        break;
      WriteLinearLevel(dest + offset, level_bytes, level.data(), level_width, level_height,
                       ref.format);
      offset += level_bytes;
      if (level_width == 1 && level_height == 1)
        break;
      uint32_t next_width = 0, next_height = 0;
      level = Downsample(level, level_width, level_height, next_width, next_height);
      level_width = next_width;
      level_height = next_height;
    }
    return EditStatus::kOk;
  }

  // Tiled NTX2. Each level above the packed one owns its whole macro-tile
  // grid, they follow each other with no further alignment, and everything at
  // and below 16 texels shares the one tail tile. Refuse unless that accounts
  // for the pixel region exactly: a layout we cannot predict is one we must not
  // write. Over the shipped data that is 5423 of 5426 chunks; the three it
  // turns down are the non-power-of-two ones (1600x1600 and 2048x1028).
  const uint32_t packed_level = PackedMipLevel(ref.width, ref.height);
  std::vector<uint32_t> level_offsets;
  uint32_t total = 0;
  for (uint32_t level = 0; level <= packed_level; ++level) {
    level_offsets.push_back(total);
    total += MipLevelBytes(std::max(1u, ref.width >> level), std::max(1u, ref.height >> level),
                           ref.format);
  }
  if (total != ref.pix_size) {
    if (error) {
      *error = "has a mip layout this build cannot reproduce (" + std::to_string(ref.pix_size) +
               " bytes of pixels, " + std::to_string(total) + " predicted)";
    }
    return EditStatus::kBadData;
  }

  // Levels above the tail, each in its own region at the tile's origin.
  const MipChain chain = BuildMipChain(image, packed_level + 1);
  for (uint32_t level = 0; level < packed_level; ++level) {
    const uint32_t pitch =
        level == 0 ? ref.pitch_macro_tiles : MipPitchMacroTiles(chain.widths[level], ref.format);
    const uint32_t offset = level_offsets[level];
    WriteTiledLevel(dest + offset, dest_size - offset, chain.levels[level].data(),
                    chain.widths[level], chain.heights[level], 0, 0, pitch, ref);
  }

  // The tail: the packed level and every smaller one, each in its own corner of
  // a single tile.
  const uint32_t tail_offset = level_offsets[packed_level];
  const uint32_t tail_pitch = MipPitchMacroTiles(chain.widths[packed_level], ref.format);
  uint32_t level_width = chain.widths[packed_level];
  uint32_t level_height = chain.heights[packed_level];
  std::vector<uint8_t> level = chain.levels[packed_level];
  for (uint32_t mip = packed_level;; ++mip) {
    uint32_t block_x = 0, block_y = 0;
    PackedMipOffset(ref.width, ref.height, ref.format, mip, block_x, block_y);
    WriteTiledLevel(dest + tail_offset, dest_size - tail_offset, level.data(), level_width,
                    level_height, block_x, block_y, tail_pitch, ref);
    if (level_width == 1 && level_height == 1)
      break;
    uint32_t next_width = 0, next_height = 0;
    level = Downsample(level, level_width, level_height, next_width, next_height);
    level_width = next_width;
    level_height = next_height;
  }
  return EditStatus::kOk;
}

}  // namespace eternalsonata::assets
