#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/assets/AssetReference.h"

namespace prefab {

//------------------------------------------------------------------------------
// Delta Types
//------------------------------------------------------------------------------

// Represents a single property override for a model node
struct NodeOverride {
    std::string modelNodePath;          // Path relative to model root (e.g., "SkeletonRoot/Body")
    nlohmann::json components;          // Serialized component overrides
    bool active = true;                 // Entity active state
    bool visible = true;                // Entity visibility
    std::string name;                   // Name override (if renamed)
    
    // Matching metadata for robust node resolution
    std::string normalizedPath;
    std::string normalizedName;
    uint64_t contentHash = 0;
    int meshFileId = -1;
    
    // Stable mesh identity for matching across model re-exports
    // This is the MeshComponent::MeshName or entity name - semantic name that stays stable
    std::string stableMeshName;
    
    // Parent normalized name for disambiguation when multiple meshes have same name
    std::string parentNormalizedName;
};

// Represents an entity added by the user under the model hierarchy
// (Named ModelAddedEntity to avoid conflict with prefab::AddedEntity in PrefabInstanceComponent.h)
struct ModelAddedEntity {
    std::string parentPath;             // Path to parent (model node or another added entity)
    std::string name;
    ClaymoreGUID guid;
    nlohmann::json components;          // Full component serialization
    std::vector<ModelAddedEntity> children;  // Recursive children
};

// Represents a deleted model node
struct DeletedNode {
    std::string modelNodePath;
    std::string normalizedPath;
    std::string normalizedName;
    uint64_t contentHash = 0;
    int meshFileId = -1;
    std::string stableMeshName;
    std::string parentNormalizedName;
};

// Complete delta for a model-based entity
struct ModelDelta {
    std::vector<NodeOverride> overrides;
    std::vector<ModelAddedEntity> added;
    std::vector<DeletedNode> deleted;
};

//------------------------------------------------------------------------------
// Delta Computation
//------------------------------------------------------------------------------

// Build a model delta by comparing current scene state against fresh model instantiation
ModelDelta ComputeModelDelta(
    Scene& scene,
    EntityID modelRoot,
    const std::string& modelPath,
    const ClaymoreGUID& modelGuid
);

// Serialize a model delta to JSON format for prefab storage
nlohmann::json SerializeModelDelta(const ModelDelta& delta);

// Deserialize model delta from JSON
ModelDelta DeserializeModelDelta(const nlohmann::json& j);

//------------------------------------------------------------------------------
// Delta Application
//------------------------------------------------------------------------------

// Apply model delta to a freshly instantiated model
bool ApplyModelDelta(
    Scene& scene,
    EntityID modelRoot,
    const ModelDelta& delta
);

// Apply model delta from JSON (used during prefab load)
bool ApplyModelDeltaFromJson(
    Scene& scene,
    EntityID modelRoot,
    const nlohmann::json& childrenArray
);

//------------------------------------------------------------------------------
// Path Utilities
//------------------------------------------------------------------------------

// Compute relative path from root to descendant
std::string ComputeNodePath(Scene& scene, EntityID root, EntityID descendant);

// Resolve entity by path under root
EntityID ResolveByPath(Scene& scene, EntityID root, const std::string& path);

// Normalize a path for fuzzy matching (strips numeric suffixes)
std::string NormalizePath(const std::string& path);

// Normalize a name for fuzzy matching
std::string NormalizeName(const std::string& name);

// Compute a content-based hash for a node (mesh indices, transform, etc.)
uint64_t ComputeNodeContentHash(Scene& scene, EntityID id);

//------------------------------------------------------------------------------
// Entity Classification
//------------------------------------------------------------------------------

// Check if an entity is part of the original model structure (has matching path in model)
bool IsModelEntity(Scene& scene, EntityID id, EntityID modelRoot);

// Check if an entity appears to be user-added (has user components)
bool IsUserAddedEntity(Scene& scene, EntityID id);

// Get all descendant entity IDs under a root
std::vector<EntityID> GetAllDescendants(Scene& scene, EntityID root);

} // namespace prefab

