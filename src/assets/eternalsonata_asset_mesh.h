// eternalsonata - NSHP mesh discovery for the granular asset API.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "eternalsonata_asset_api.h"

namespace eternalsonata::assets {

struct MeshRef {
  size_t offset = 0;
  uint32_t chunk_size = 0;
  std::string name;
  uint16_t vertex_count = 0;
  uint16_t section_count = 0;
  uint8_t bone_count = 0;
};

struct SkeletonRef {
  size_t offset = 0;
  uint32_t chunk_size = 0;
  uint32_t bone_count = 0;
};

struct AnimationRef {
  size_t offset = 0;
  uint32_t chunk_size = 0;
  std::string name;
  uint16_t frame_count = 0;
  uint32_t track_count = 0;
};

// Scans the decoded container and returns only structurally valid NSHP chunks.
// Ordinals are assigned after malformed candidates are skipped.
std::vector<MeshRef> FindMeshes(const std::vector<uint8_t>& data);
std::vector<SkeletonRef> FindSkeletons(const std::vector<uint8_t>& data);
std::vector<AnimationRef> FindAnimations(const std::vector<uint8_t>& data);

enum class ModelEditStatus { kOk, kNotFound, kTooLarge, kBadData, kUnsupported };

ModelEditStatus ReplaceMeshChunk(std::vector<uint8_t>& data, const MeshRef& original,
                                 const EternalSonataMesh& mesh, bool allow_resize,
                                 std::string* error);

bool ValidateModelGraph(const std::vector<uint8_t>& data, std::string* error);

}  // namespace eternalsonata::assets
