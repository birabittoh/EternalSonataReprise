// eternalsonata - NSHP mesh discovery for the granular asset API.

#include "eternalsonata_asset_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "eternalsonata_asset_container.h"

namespace eternalsonata::assets {
namespace {

uint16_t ReadBE16(const uint8_t* p) {
  return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}

uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool IsPlausible(const std::vector<uint8_t>& data, size_t at, MeshRef* out) {
  if (at + 0x38 > data.size())
    return false;
  const uint32_t size = ReadBE32(data.data() + at + 4);
  if (size < 0x38 || size > data.size() - at)
    return false;

  const uint16_t vertices = ReadBE16(data.data() + at + 0x1A);
  const uint16_t sections = ReadBE16(data.data() + at + 0x1E);
  const uint8_t bones = data[at + 0x20];
  const size_t prefix = (0x38 + size_t(bones) * 2 + 3) & ~size_t(3);
  if (!vertices || prefix >= size)
    return false;
  // Every known indexed layout needs one 16 byte record per section. A zero
  // section mesh is permitted because sequential meshes carry a 12 byte footer.
  if (sections && prefix + size_t(sections) * 16 > size)
    return false;

  size_t name_length = 0;
  while (name_length < 16 && data[at + 8 + name_length])
    ++name_length;
  out->offset = at;
  out->chunk_size = size;
  out->name.assign(reinterpret_cast<const char*>(data.data() + at + 8), name_length);
  out->vertex_count = vertices;
  out->section_count = sections;
  out->bone_count = bones;
  return true;
}

void WriteBE16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

void WriteBE32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void WriteBEFloat(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteBE32(p, bits);
}

uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000;
  int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) {
    if (exponent < -10)
      return uint16_t(sign);
    mantissa = (mantissa | 0x800000) >> (1 - exponent);
    return uint16_t(sign | ((mantissa + 0x1000) >> 13));
  }
  if (exponent >= 31)
    return uint16_t(sign | 0x7C00);
  mantissa += 0x1000;
  if (mantissa & 0x800000) {
    mantissa = 0;
    if (++exponent >= 31)
      return uint16_t(sign | 0x7C00);
  }
  return uint16_t(sign | uint32_t(exponent << 10) | (mantissa >> 13));
}

uint32_t PackNormal(const float normal[3]) {
  uint32_t packed = 0;
  for (int i = 0; i < 3; ++i) {
    const int value = std::clamp(int(std::lround(normal[i] * 511.0f)), -512, 511);
    packed |= (uint32_t(value) & 0x3FF) << (22 - i * 10);
  }
  return packed;
}

bool FiniteVertex(const EternalSonataVertex& vertex) {
  for (float value : vertex.position)
    if (!std::isfinite(value))
      return false;
  for (float value : vertex.normal)
    if (!std::isfinite(value))
      return false;
  for (float value : vertex.uv)
    if (!std::isfinite(value))
      return false;
  for (float value : vertex.bone_weights)
    if (!std::isfinite(value) || value < 0.0f)
      return false;
  return true;
}

struct MeshLayout {
  size_t vertex_offset = 0;
  uint32_t stride = 0;
  bool skinned = false;
  bool sequential = false;
  std::vector<uint16_t> materials;
};

bool DetectLayout(const std::vector<uint8_t>& data, const MeshRef& ref, MeshLayout* layout) {
  const uint8_t* chunk = data.data() + ref.offset;
  const size_t prefix = (0x38 + size_t(ref.bone_count) * 2 + 3) & ~size_t(3);
  const size_t end = ref.chunk_size;
  const int skinned_strides[] = {32, 40, 28};
  const int static_strides[] = {24, 28, 32, 20, 16};
  const int* strides = ref.bone_count ? skinned_strides : static_strides;
  const size_t stride_count =
      ref.bone_count ? std::size(skinned_strides) : std::size(static_strides);
  for (size_t pre = 0; pre <= (ref.bone_count ? 0 : 32); pre += 32) {
    for (size_t si = 0; si < stride_count; ++si) {
      const size_t vertices_end = prefix + pre + size_t(ref.vertex_count) * strides[si];
      if (vertices_end > end)
        continue;
      if (vertices_end + 12 == end && ReadBE16(chunk + vertices_end + 4) == ref.vertex_count &&
          ReadBE16(chunk + vertices_end + 6) == 0 &&
          ReadBE16(chunk + vertices_end + 8) == 0 &&
          ReadBE16(chunk + vertices_end + 10) == 0) {
        layout->vertex_offset = prefix + pre;
        layout->stride = strides[si];
        layout->skinned = ref.bone_count != 0;
        layout->sequential = true;
        layout->materials = {ReadBE16(chunk + vertices_end)};
        return true;
      }
      size_t records = vertices_end;
      size_t indices = records + size_t(ref.section_count) * 16;
      std::vector<uint16_t> materials;
      bool valid = indices <= end;
      for (uint16_t section = 0; valid && section < ref.section_count; ++section) {
        const uint16_t count = ReadBE16(chunk + records + size_t(section) * 16 + 14);
        valid = count >= 3 && size_t(count) * 2 <= end - indices;
        materials.push_back(ReadBE16(chunk + records + size_t(section) * 16));
        indices += size_t(count) * 2;
      }
      if (valid && end - indices <= 4) {
        layout->vertex_offset = prefix + pre;
        layout->stride = strides[si];
        layout->skinned = ref.bone_count != 0;
        layout->materials = std::move(materials);
        return true;
      }
      size_t cursor = vertices_end;
      materials.clear();
      valid = true;
      for (uint16_t section = 0; valid && section < ref.section_count; ++section) {
        if (cursor + 16 > end) {
          valid = false;
          break;
        }
        const uint16_t count = ReadBE16(chunk + cursor + 14);
        materials.push_back(ReadBE16(chunk + cursor));
        cursor += 16;
        valid = count >= 3 && size_t(count) * 2 <= end - cursor;
        cursor += size_t(count) * 2;
      }
      if (valid && end - cursor <= 4) {
        layout->vertex_offset = prefix + pre;
        layout->stride = strides[si];
        layout->skinned = ref.bone_count != 0;
        layout->materials = std::move(materials);
        return true;
      }
    }
  }
  return false;
}

void EncodeVertex(uint8_t* out, const EternalSonataVertex& vertex, const MeshLayout& layout) {
  WriteBEFloat(out, vertex.position[0]);
  WriteBEFloat(out + 4, vertex.position[1]);
  WriteBEFloat(out + 8, vertex.position[2]);
  if (layout.skinned && layout.stride == 40) {
    WriteBEFloat(out + 0x0C, vertex.bone_weights[0]);
    WriteBEFloat(out + 0x10, vertex.bone_weights[1]);
    WriteBEFloat(out + 0x14, vertex.bone_weights[2]);
    out[0x1B] = vertex.bone_ids[0];
    out[0x1A] = vertex.bone_ids[1];
    out[0x19] = vertex.bone_ids[2];
    out[0x18] = vertex.bone_ids[3];
    WriteBE16(out + 0x24, FloatToHalf(vertex.uv[0]));
    WriteBE16(out + 0x26, FloatToHalf(vertex.uv[1]));
  } else if (layout.skinned) {
    float sum = 0.0f;
    for (float weight : vertex.bone_weights)
      sum += weight;
    for (int i = 0; i < 4; ++i) {
      const float weight = sum > 0.0f ? vertex.bone_weights[i] / sum : (i == 0 ? 1.0f : 0.0f);
      out[0x0C + i] = uint8_t(std::clamp(int(std::lround(weight * 255.0f)), 0, 255));
      out[0x10 + i] = vertex.bone_ids[i];
    }
    for (int i = 0; i < 3; ++i)
      out[0x14 + i] =
          uint8_t(int8_t(std::clamp(int(std::lround(vertex.normal[i] * 127.0f)), -127, 127)));
    WriteBE16(out + layout.stride - 4, FloatToHalf(vertex.uv[0]));
    WriteBE16(out + layout.stride - 2, FloatToHalf(vertex.uv[1]));
  } else if (layout.stride != 16) {
    WriteBE32(out + 0x0C, PackNormal(vertex.normal));
    if (layout.stride == 24 || layout.stride == 32) {
      WriteBE16(out + 0x14, FloatToHalf(vertex.uv[0]));
      WriteBE16(out + 0x16, FloatToHalf(vertex.uv[1]));
    } else if (layout.stride == 28) {
      WriteBE16(out + 0x18, FloatToHalf(vertex.uv[0]));
      WriteBE16(out + 0x1A, FloatToHalf(vertex.uv[1]));
    } else {
      WriteBE16(
          out + 0x10,
          uint16_t(int16_t(std::clamp(int(std::lround(vertex.uv[0] * 32767.0f)), -32768, 32767))));
      WriteBE16(
          out + 0x12,
          uint16_t(int16_t(std::clamp(int(std::lround(vertex.uv[1] * 32767.0f)), -32768, 32767))));
    }
  }
}

}  // namespace

std::vector<MeshRef> FindMeshes(const std::vector<uint8_t>& data) {
  static constexpr uint8_t kMagic[] = {'N', 'S', 'H', 'P'};
  std::vector<MeshRef> found;
  size_t cursor = 0;
  while (cursor + sizeof(kMagic) <= data.size()) {
    const auto it = std::search(data.begin() + ptrdiff_t(cursor), data.end(), std::begin(kMagic),
                                std::end(kMagic));
    if (it == data.end())
      break;
    const size_t at = size_t(it - data.begin());
    MeshRef ref;
    if (IsPlausible(data, at, &ref)) {
      found.push_back(std::move(ref));
      cursor = at + found.back().chunk_size;
    } else {
      cursor = at + sizeof(kMagic);
    }
  }
  return found;
}

std::vector<SkeletonRef> FindSkeletons(const std::vector<uint8_t>& data) {
  static constexpr uint8_t kMagic[] = {'N', 'B', 'N', '2'};
  std::vector<SkeletonRef> found;
  for (size_t cursor = 0; cursor + 8 <= data.size();) {
    const auto it = std::search(data.begin() + ptrdiff_t(cursor), data.end(), std::begin(kMagic),
                                std::end(kMagic));
    if (it == data.end())
      break;
    const size_t at = size_t(it - data.begin());
    const uint32_t size = at + 8 <= data.size() ? ReadBE32(data.data() + at + 4) : 0;
    uint32_t count = 0;
    bool valid = size >= 72 && size <= data.size() - at;
    for (size_t off = 8; valid && off + 64 <= size; off += 64) {
      if (!data[at + off])
        break;
      const int16_t parent = int16_t(ReadBE16(data.data() + at + off + 0x12));
      valid = parent < int16_t(count);
      ++count;
    }
    if (valid) {
      found.push_back({at, size, count});
      cursor = at + size;
    } else {
      cursor = at + 4;
    }
  }
  return found;
}

std::vector<AnimationRef> FindAnimations(const std::vector<uint8_t>& data) {
  static constexpr uint8_t kMagic[] = {'N', 'M', 'T', 'N'};
  std::vector<AnimationRef> found;
  for (size_t cursor = 0; cursor + 0x30 <= data.size();) {
    const auto it = std::search(data.begin() + ptrdiff_t(cursor), data.end(), std::begin(kMagic),
                                std::end(kMagic));
    if (it == data.end())
      break;
    const size_t at = size_t(it - data.begin());
    const uint32_t size = ReadBE32(data.data() + at + 4);
    uint32_t tracks = 0;
    size_t pos = 0x30;
    if (size >= 0x30 && size <= data.size() - at) {
      while (pos + 0x14 <= size) {
        const uint32_t track_size = ReadBE32(data.data() + at + pos + 0x10);
        if (track_size < 0x14 || track_size > size - pos)
          break;
        ++tracks;
        pos += track_size;
      }
    }
    if (tracks) {
      size_t name_length = 0;
      while (name_length < 16 && data[at + 0x10 + name_length])
        ++name_length;
      AnimationRef ref;
      ref.offset = at;
      ref.chunk_size = size;
      ref.name.assign(reinterpret_cast<const char*>(data.data() + at + 0x10), name_length);
      ref.frame_count = ReadBE16(data.data() + at + 0x20);
      ref.track_count = tracks;
      found.push_back(std::move(ref));
      cursor = at + size;
    } else {
      cursor = at + 4;
    }
  }
  return found;
}

ModelEditStatus ReplaceMeshChunk(std::vector<uint8_t>& data, const MeshRef& original,
                                 const EternalSonataMesh& mesh, bool allow_resize,
                                 std::string* error) {
  if (!mesh.vertices || !mesh.vertex_count || !mesh.indices || !mesh.index_count ||
      !mesh.sections || !mesh.section_count || mesh.vertex_count > 0xFFFF ||
      mesh.section_count > 0xFFFF) {
    if (error)
      *error = "mesh arrays are empty or exceed the format limits";
    return ModelEditStatus::kBadData;
  }
  MeshLayout layout;
  if (!DetectLayout(data, original, &layout)) {
    if (error)
      *error = "the original NSHP vertex layout is not supported";
    return ModelEditStatus::kUnsupported;
  }
  if (layout.sequential) {
    if (error)
      *error = "sequential NSHP replacement is not supported";
    return ModelEditStatus::kUnsupported;
  }
  if (layout.stride == 40 && mesh.vertex_count != original.vertex_count) {
    if (error)
      *error = "stride 40 normals require a native NSHP replacement when the vertex count changes";
    return ModelEditStatus::kUnsupported;
  }
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    if (!FiniteVertex(mesh.vertices[i])) {
      if (error)
        *error = "a vertex contains an invalid numeric value";
      return ModelEditStatus::kBadData;
    }
    if (layout.skinned) {
      for (uint8_t bone : mesh.vertices[i].bone_ids) {
        if (bone >= original.bone_count) {
          if (error)
            *error = "a vertex names a bone outside the mesh bone list";
          return ModelEditStatus::kBadData;
        }
      }
    }
  }

  std::vector<std::vector<uint16_t>> strips(mesh.section_count);
  for (uint32_t section = 0; section < mesh.section_count; ++section) {
    const auto& input = mesh.sections[section];
    if (std::find(layout.materials.begin(), layout.materials.end(), input.material_id) ==
            layout.materials.end() ||
        input.index_start > mesh.index_count ||
        input.index_count > mesh.index_count - input.index_start || input.index_count % 3) {
      if (error)
        *error = "a face section has an invalid material or index range";
      return ModelEditStatus::kBadData;
    }
    auto& strip = strips[section];
    for (uint32_t i = 0; i < input.index_count; i += 3) {
      if (!strip.empty())
        strip.push_back(0xFFFF);
      for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t index = mesh.indices[input.index_start + i + k];
        if (index >= mesh.vertex_count || index > 0xFFFE) {
          if (error)
            *error = "an index is outside the replacement vertex array";
          return ModelEditStatus::kBadData;
        }
        strip.push_back(uint16_t(index));
      }
    }
    if (strip.size() > 0xFFFF) {
      if (error)
        *error = "a face section exceeds the NSHP index limit";
      return ModelEditStatus::kBadData;
    }
  }

  const size_t prefix = layout.vertex_offset;
  size_t new_size =
      prefix + size_t(mesh.vertex_count) * layout.stride + size_t(mesh.section_count) * 16;
  for (const auto& strip : strips)
    new_size += strip.size() * 2;
  new_size = (new_size + 3) & ~size_t(3);
  if (!allow_resize && new_size > original.chunk_size) {
    if (error)
      *error = "the encoded mesh is larger than the original chunk";
    return ModelEditStatus::kTooLarge;
  }

  std::vector<uint8_t> chunk(new_size, 0);
  std::memcpy(chunk.data(), data.data() + original.offset, prefix);
  WriteBE32(chunk.data() + 4, uint32_t(new_size));
  WriteBE16(chunk.data() + 0x1A, uint16_t(mesh.vertex_count));
  WriteBE16(chunk.data() + 0x1E, uint16_t(mesh.section_count));
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    if (i < original.vertex_count) {
      std::memcpy(chunk.data() + prefix + size_t(i) * layout.stride,
                  data.data() + original.offset + layout.vertex_offset + size_t(i) * layout.stride,
                  layout.stride);
    }
    EncodeVertex(chunk.data() + prefix + size_t(i) * layout.stride, mesh.vertices[i], layout);
  }

  const size_t records = prefix + size_t(mesh.vertex_count) * layout.stride;
  size_t indices = records + size_t(mesh.section_count) * 16;
  for (uint32_t section = 0; section < mesh.section_count; ++section) {
    uint8_t* record = chunk.data() + records + size_t(section) * 16;
    WriteBE16(record, mesh.sections[section].material_id);
    WriteBE16(record + 14, uint16_t(strips[section].size()));
    for (uint16_t index : strips[section]) {
      WriteBE16(chunk.data() + indices, index);
      indices += 2;
    }
  }
  if (!allow_resize && chunk.size() < original.chunk_size) {
    chunk.resize(original.chunk_size, 0);
    WriteBE32(chunk.data() + 4, original.chunk_size);
  }
  if (!ReplaceContainerRange(data, original.offset, original.chunk_size, chunk)) {
    if (error)
      *error = "the NSHP range is outside the container";
    return ModelEditStatus::kBadData;
  }
  return ModelEditStatus::kOk;
}

bool ValidateModelGraph(const std::vector<uint8_t>& data, std::string* error) {
  const auto skeletons = FindSkeletons(data);
  if (skeletons.empty())
    return true;
  uint32_t largest_skeleton = 0;
  for (const auto& skeleton : skeletons) {
    largest_skeleton = std::max(largest_skeleton, skeleton.bone_count);
  }
  for (const auto& mesh : FindMeshes(data)) {
    const size_t list = mesh.offset + 0x38;
    for (uint32_t i = 0; i < mesh.bone_count; ++i) {
      if (ReadBE16(data.data() + list + size_t(i) * 2) >= largest_skeleton) {
        if (error)
          *error = "an NSHP local bone table points outside every NBN2 skeleton";
        return false;
      }
    }
  }
  return true;
}

}  // namespace eternalsonata::assets
