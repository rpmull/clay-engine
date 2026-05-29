#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "core/rendering/Mesh.h"
#include "editor/import/ModelLoader.h"

struct PreparedModel;

namespace meshbin {

static constexpr uint32_t MESH_BIN_MAGIC = 'B' | ('H'<<8) | ('S'<<16) | ('M'<<24); // 'MSHB' little-endian
// v2 adds submesh names and material texture hints
// v3 adds quantized vertex encoding metadata (positions/uv/normals/weights)
// v4 keeps per-entry mesh data aligned with PreparedModel (no SkinnedCombined aggregate)
// v5 rebakes skinned vertices into skeleton space (fix animation parity)
// v6 stores full 4x4 local transforms for deterministic re-instantiation
// v7 adds submesh/material hints + keeps blendshape metadata external
// v8 serializes explicit submesh layouts + blendshape blobs for fast instantiation
// v9 adds sparse blendshape storage for memory efficiency
static constexpr uint32_t MESH_BIN_VERSION = 9;

// Write all meshes from a loaded Model into a single meshbin file (one submesh block per Model mesh)
bool WriteMeshBinFromModel(const Model& model, const std::string& filePath);
bool WriteMeshBinFromPrepared(const PreparedModel& prepared, const std::string& filePath);

// Read a specific submesh index from meshbin and create a Mesh ready for GPU upload
// Returns nullptr on failure. Sets outSkinned to true if the submesh contains skinning streams.
std::shared_ptr<Mesh> ReadMeshFromBin(const std::string& filePath,
                                      uint32_t submeshIndex,
                                      bool& outSkinned,
                                      std::unique_ptr<BlendShapeComponent>* outBlendShapes = nullptr);
std::unique_ptr<BlendShapeComponent> ReadBlendShapesFromBin(const std::string& filePath,
                                                            uint32_t submeshIndex);

// Async prefetch: read raw bytes for multiple submeshes on a background thread
struct PrefetchRequest { std::string path; std::vector<uint32_t> submeshIndices; };
struct PrefetchResult {
    std::vector<std::shared_ptr<Mesh>> meshes;
    std::vector<std::unique_ptr<BlendShapeComponent>> blendShapes;
};
void PrefetchMeshesAsync(const PrefetchRequest& req, std::function<void(PrefetchResult&&)> onComplete);

// Utility: number of submeshes contained in a meshbin
uint32_t GetSubmeshCount(const std::string& filePath);

// Quick header check for cache validation
bool HasCurrentVersion(const std::string& filePath);

// Optional: lookups for names and textures
std::string GetSubmeshName(const std::string& filePath, uint32_t submeshIndex);
struct TextureHints { std::string albedo, metallicRoughness, normal; };
TextureHints GetSubmeshTextureHints(const std::string& filePath, uint32_t submeshIndex);

struct MaterialExtras { bool hasTint=false; glm::vec4 tint=glm::vec4(1.0f); bool twoSided=false; };
MaterialExtras GetSubmeshMaterialExtras(const std::string& filePath, uint32_t submeshIndex);

struct TransformInfo {
    bool has = false;
    glm::vec3 t = glm::vec3(0);
    glm::quat r = glm::quat(1,0,0,0);
    glm::vec3 s = glm::vec3(1);
    glm::mat4 matrix = glm::mat4(1.0f);
};
TransformInfo GetSubmeshLocalTransform(const std::string& filePath, uint32_t submeshIndex);

}


