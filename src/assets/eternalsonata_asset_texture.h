// eternalsonata - ReXGlue Recompiled Project
//
// The texture half of the granular asset API: finding the NTX2 / NTEX chunks
// inside a decoded .e, and splicing a replacement image into one of them.
//
// A texture patch never changes the container's length. The replacement is
// encoded into the format and the exact pixel region the shipped chunk already
// occupies, so nothing after it moves and no relocation table is involved. That
// is also why the dimensions have to match: the fetch constant, the mip chain's
// layout and the region's size are one consistent set, and rewriting them would
// turn a splice into a container rebuild.
//
// Formats: docs/asset-formats.md, plus eternal-sonata-studio's Ntx2parser.h for
// the NTX2 wrapper around XPR2 and Ntexparser.h for the plain-DDS NTEX case.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "eternalsonata_asset_container.h"

namespace eternalsonata::assets {

enum class TextureFormat {
  kUnknown,
  kDXT1,
  kDXT3,
  kDXT5,
  kRGBA8,
};

const char* TextureFormatName(TextureFormat format);

struct TextureRef {
  size_t chunk_offset = 0;  // of the magic, in the decoded container
  uint32_t chunk_size = 0;
  std::string name;  // the chunk's embedded name, empty when it has none

  uint32_t width = 0;
  uint32_t height = 0;
  TextureFormat format = TextureFormat::kUnknown;

  // NTX2 only. NTEX carries an ordinary little-endian PC DDS, untiled.
  bool ntex = false;
  bool tiled = true;
  uint32_t endianness = 0;         // the fetch constant's, 1 = 8in16
  uint32_t pitch_macro_tiles = 1;  // from W0, not width_blocks >> 5
  uint32_t pix_offset = 0;         // from the chunk's start
  uint32_t pix_size = 0;           // the whole mip chain
};

// Every texture chunk in a decoded container, in ascending file order. The bulk
// section has no directory, so this scans for the magics and validates by
// parsing, the way FindBtxBlobs does.
std::vector<TextureRef> FindTextures(const std::vector<uint8_t>& data);

// A replacement, decoded to tightly packed RGBA8, top row first.
struct SourceImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

// PNG (via the SDK's stb wrapper) or an uncompressed/DXT .dds.
bool LoadSourceImage(const std::filesystem::path& file, SourceImage& out, std::string* error);

// Encodes `image` into the chunk's own format and writes it over the chunk's
// pixel region, mip chain included. `data` keeps its length.
EditStatus ApplyTextureEdit(std::vector<uint8_t>& data, const TextureRef& ref,
                            const SourceImage& image, std::string* error);

}  // namespace eternalsonata::assets
