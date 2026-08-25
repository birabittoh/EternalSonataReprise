// eternalsonata - ReXGlue Recompiled Project
//
// See guest_shaders.h.
//
// The pack format, written by scripts/gen-guest-shaders.py. Everything is
// little endian and the whole file is read into one buffer that is kept alive
// for the process, so the pointers handed out below point straight into it.
//
//   header    magic 'ESGS', version, slot count, input count, key bytes,
//             blob bytes
//   entries   2 * slot count records: vertex table first, then pixel
//   inputs    (usage, usage index) pairs, referenced by offset from an entry
//   keys      interpolator semantic keys, referenced by byte offset
//   blob      the compiled shaders, referenced by offset and size
//
// Every offset is validated against the section it indexes before anything is
// handed out. A pack built by a different revision of the generator should fail
// to load rather than produce plausible garbage that turns into a GPU fault
// later.

#include "guest_shaders.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace eternalsonata {
namespace {

constexpr uint32_t kMagic = 0x53475345;  // 'ESGS', little endian
// Bumped to 2 when the emitter moved the pixel shaders' constant buffers to b2
// and b3. The pack layout did not change, so a stale pack would load and bind
// the vertex float bank to every pixel shader; refusing it is the point.
constexpr uint32_t kVersion = 2;
constexpr char kDefaultName[] = "guest_shaders.bin";

constexpr uint8_t kFlagPointSize = 1 << 0;
constexpr uint8_t kFlagHasCube = 1 << 1;

#pragma pack(push, 1)
struct PackHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t slots;
  uint32_t input_count;
  uint32_t key_bytes;
  uint32_t blob_bytes;
};

struct PackEntry {
  uint32_t dxil_offset;
  uint32_t dxil_size;
  uint32_t spirv_offset;
  uint32_t spirv_size;
  uint32_t texture_mask;
  uint16_t input_offset;  // in pairs, into the input section
  uint8_t input_count;
  uint8_t flags;
  uint16_t key_offset;  // in bytes, into the key section
  uint8_t key_count;
  uint8_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(PackHeader) == 24, "pack header layout");
static_assert(sizeof(PackEntry) == 28, "pack entry layout");

std::vector<uint8_t> g_pack;
std::vector<GuestShader> g_vertex;
std::vector<GuestShader> g_pixel;
const GuestShader g_absent;
bool g_attempted = false;
bool g_loaded = false;

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const std::streamoff size = file.tellg();
  if (size <= 0)
    return false;
  out.resize(static_cast<size_t>(size));
  file.seekg(0);
  return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

// Decode one record. Returns false on any offset that does not fit its section,
// which is the whole validation: a truncated or mismatched pack is rejected here
// rather than at the point a blob is handed to the driver.
bool Decode(const PackEntry& entry, const PackHeader& header, const uint8_t* blob,
            const GuestVertexInput* inputs, const uint8_t* keys, GuestShader& out) {
  auto in_blob = [&](uint32_t offset, uint32_t size) {
    return size == 0 || (offset <= header.blob_bytes && size <= header.blob_bytes - offset);
  };
  if (!in_blob(entry.dxil_offset, entry.dxil_size) ||
      !in_blob(entry.spirv_offset, entry.spirv_size))
    return false;
  if (entry.input_offset + entry.input_count > header.input_count)
    return false;
  if (entry.key_offset + entry.key_count > header.key_bytes)
    return false;

  if (entry.dxil_size) {
    out.dxil = blob + entry.dxil_offset;
    out.dxil_size = entry.dxil_size;
  }
  if (entry.spirv_size) {
    out.spirv = blob + entry.spirv_offset;
    out.spirv_size = entry.spirv_size;
  }
  out.texture_mask = entry.texture_mask;
  out.inputs = entry.input_count ? inputs + entry.input_offset : nullptr;
  out.input_count = entry.input_count;
  out.interpolator_keys = entry.key_count ? keys + entry.key_offset : nullptr;
  out.interpolator_key_count = entry.key_count;
  out.exports_point_size = (entry.flags & kFlagPointSize) != 0;
  out.has_cube_texture = (entry.flags & kFlagHasCube) != 0;
  return true;
}

}  // namespace

bool LoadGuestShaders(const char* path) {
  if (g_attempted)
    return g_loaded;
  g_attempted = true;

  const std::filesystem::path pack_path =
      path ? rex::to_path(path) : rex::filesystem::GetExecutableFolder() / kDefaultName;
  const std::string resolved = rex::path_to_utf8(pack_path);

  if (!ReadFile(pack_path, g_pack)) {
    REXLOG_ERROR("guest_shaders: cannot read {}", resolved);
    return false;
  }

  if (g_pack.size() < sizeof(PackHeader)) {
    REXLOG_ERROR("guest_shaders: {} is too small to be a pack", resolved);
    return false;
  }
  PackHeader header;
  std::memcpy(&header, g_pack.data(), sizeof(header));
  if (header.magic != kMagic || header.version != kVersion) {
    REXLOG_ERROR("guest_shaders: {} is not a v{} pack (magic {:08X} version {})", resolved,
                 kVersion, header.magic, header.version);
    return false;
  }

  const size_t entries_bytes = size_t(header.slots) * 2 * sizeof(PackEntry);
  const size_t expected = sizeof(PackHeader) + entries_bytes +
                          size_t(header.input_count) * sizeof(GuestVertexInput) +
                          size_t(header.key_bytes) + size_t(header.blob_bytes);
  if (g_pack.size() != expected) {
    REXLOG_ERROR("guest_shaders: {} is {} bytes, expected {}", resolved, g_pack.size(), expected);
    return false;
  }

  const uint8_t* cursor = g_pack.data() + sizeof(PackHeader);
  const PackEntry* records = reinterpret_cast<const PackEntry*>(cursor);
  cursor += entries_bytes;
  const GuestVertexInput* inputs = reinterpret_cast<const GuestVertexInput*>(cursor);
  cursor += size_t(header.input_count) * sizeof(GuestVertexInput);
  const uint8_t* keys = cursor;
  cursor += header.key_bytes;
  const uint8_t* blob = cursor;

  g_vertex.assign(header.slots, GuestShader{});
  g_pixel.assign(header.slots, GuestShader{});
  for (uint32_t i = 0; i < header.slots * 2; ++i) {
    GuestShader& out = i < header.slots ? g_vertex[i] : g_pixel[i - header.slots];
    if (!Decode(records[i], header, blob, inputs, keys, out)) {
      REXLOG_ERROR("guest_shaders: entry {} in {} is out of range", i, resolved);
      g_vertex.clear();
      g_pixel.clear();
      g_pack.clear();
      return false;
    }
  }

  g_loaded = true;
  REXLOG_INFO("guest_shaders: loaded {} vertex, {} pixel from {} ({} KiB)",
              GuestVertexShaderCount(), GuestPixelShaderCount(), resolved, g_pack.size() / 1024);
  return true;
}

const GuestShader& GuestVertexShader(uint32_t slot) {
  return slot < g_vertex.size() ? g_vertex[slot] : g_absent;
}

const GuestShader& GuestPixelShader(uint32_t slot) {
  return slot < g_pixel.size() ? g_pixel[slot] : g_absent;
}

uint32_t GuestVertexShaderCount() {
  uint32_t count = 0;
  for (const GuestShader& shader : g_vertex)
    count += shader.valid() ? 1 : 0;
  return count;
}

uint32_t GuestPixelShaderCount() {
  uint32_t count = 0;
  for (const GuestShader& shader : g_pixel)
    count += shader.valid() ? 1 : 0;
  return count;
}

}  // namespace eternalsonata
