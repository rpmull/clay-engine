#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "core/assets/AssetReference.h"

namespace cm {

/**
 * Runtime Model Manifest - Compiled metadata for model instantiation
 * 
 * This is the "runtime metadata" tier - generated during build export from
 * the editor's .meta files. Contains only what's needed to reconstruct
 * model hierarchies at runtime.
 * 
 * Design principles:
 * - Trimmed: No editor-only data (import settings, source paths)
 * - Transformed: Paths converted to GUIDs, formats optimized
 * - Self-contained: Everything needed to instantiate without .meta files
 */

struct RuntimeMaterialSlot {
    std::string name;
    ClaymoreGUID albedoGuid;
    ClaymoreGUID normalGuid;
    ClaymoreGUID metallicRoughnessGuid;
    ClaymoreGUID aoGuid;
    ClaymoreGUID emissionGuid;
    ClaymoreGUID displacementGuid;
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    float normalScale = 1.0f;
    float aoStrength = 1.0f;
    bool alphaBlend = false;
    bool alphaCutout = false;
    bool twoSided = false;
    bool hasTint = false;
    float alphaCutoutThreshold = 0.5f;
};

struct RuntimeMeshNode {
    std::string name;
    int32_t parentIndex = -1;      // -1 = root
    int32_t meshFileId = -1;       // -1 = no mesh (bone only), otherwise submesh index
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    bool skinned = false;
    std::vector<RuntimeMaterialSlot> materials;
};

struct RuntimeMeshProxy {
    std::string name;
    std::string displayName;
    int32_t meshEntryIndex = -1;   // Index into nodes array for target mesh
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    bool skinned = false;
    std::vector<uint32_t> submeshSlots;
    int32_t originalMeshIndex = -1;
};

struct RuntimeModelManifest {
    static constexpr uint32_t MAGIC = 0x4D4F4452;  // 'MODL'
    static constexpr uint32_t VERSION = 5;         // v5 adds authored material slot names
    
    ClaymoreGUID modelGuid;           // GUID of the model asset
    ClaymoreGUID meshbinGuid;         // Reference to .meshbin
    ClaymoreGUID skeletonGuid;        // Reference to .skelbin (if skinned)
    
    std::vector<RuntimeMeshNode> nodes;
    std::vector<RuntimeMeshProxy> proxies;  // Mesh proxy entities for submesh rendering
    
    // Skeleton info (duplicated from skelbin for fast access)
    std::vector<std::string> boneNames;
    std::vector<int32_t> boneParents;
    
    // Root transform (applied to entire model)
    glm::vec3 rootPosition{0.0f};
    glm::quat rootRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 rootScale{1.0f};
    
    bool hasSkeleton() const { return !boneNames.empty(); }
    bool isValid() const { return !nodes.empty(); }
};

/**
 * Binary format for .modelrt files:
 * 
 * Header:
 *   uint32_t magic          // 'MODL'
 *   uint32_t version        // 1
 *   uint64_t modelGuid[2]   // high, low
 *   uint64_t meshbinGuid[2]
 *   uint64_t skeletonGuid[2]
 *   float rootPos[3]
 *   float rootRot[4]        // wxyz
 *   float rootScale[3]
 *   uint32_t nodeCount
 *   uint32_t boneCount
 * 
 * Nodes[nodeCount]:
 *   uint16_t nameLength
 *   char name[nameLength]
 *   int32_t parentIndex
 *   int32_t meshFileId
 *   float pos[3]
 *   float rot[4]
 *   float scale[3]
 *   uint8_t skinned
 *   uint16_t materialCount
 *   Materials[materialCount]:
 *     uint16_t nameLength           // v5+
 *     char name[nameLength]         // v5+
 *     uint64_t albedoGuid[2]
 *     uint64_t normalGuid[2]
 *     uint64_t mrGuid[2]
 *     uint64_t aoGuid[2]
 *     uint64_t emissionGuid[2]
 *     uint64_t displacementGuid[2]
 *     float tint[4]
 *     uint8_t flags       // bit0=alphaBlend, bit1=twoSided, bit2=hasTint, bit3=alphaCutout
 *     float alphaCutoutThreshold   // v3+ (only when version >= 3)
 * 
 * Bones[boneCount]:
 *   uint16_t nameLength
 *   char name[nameLength]
 *   int32_t parentIndex
 */

class RuntimeModelManifestLoader {
public:
    static bool Load(const std::string& path, RuntimeModelManifest& outManifest);
    static bool LoadFromMemory(const uint8_t* data, size_t size, RuntimeModelManifest& outManifest);
};

} // namespace cm

