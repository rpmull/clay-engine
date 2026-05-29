#pragma once

#include "ModelDelta.h"
#include "core/ecs/Entity.h"
#include <functional>

class Scene;
struct EntityData;

namespace cm::model {

/// @brief Configuration for delta extraction
struct DeltaExtractionConfig {
    /// Include entity-level overrides (name, layer, tag, visible, active)
    bool extractEntityOverrides = true;

    /// Include transform deltas (position, rotation, scale)
    bool extractTransforms = true;
    
    /// Include mesh material overrides
    bool extractMeshOverrides = true;
    
    /// Include user-added components (lights, colliders, cameras, etc.)
    bool extractAddedComponents = true;
    
    /// Include scripts
    bool extractScripts = true;
    
    /// Include animation constraints (LookAt, IK)
    bool extractConstraints = true;
    
    /// Include user-added children
    bool extractAddedChildren = true;
    
    /// Include deleted node tracking
    bool extractDeletedNodes = true;
    
    /// Include blend shape weights
    bool extractBlendShapes = true;
    
    /// If true, extract ALL transforms; if false, only extract modified transforms
    /// (Always extracting is safer but produces larger deltas)
    bool alwaysExtractTransforms = true;
    
    /// Verbose logging
    bool verbose = false;
};

/// @brief Extracts ModelDelta from a live model instance in the scene
/// 
/// This class provides a unified way to extract all user modifications from a
/// model instance. It is used by:
/// - Scene serialization (to save deltas to JSON/binary)
/// - Hot reload (to capture state before destroying old model)
class ModelDeltaExtractor {
public:
    explicit ModelDeltaExtractor(Scene& scene);
    
    /// Extract delta from a model root entity
    /// @param modelRootId The entity ID of the model root
    /// @param config Extraction configuration
    /// @return The extracted ModelDelta
    ModelDelta Extract(EntityID modelRootId, const DeltaExtractionConfig& config = {});
    
    /// Extract delta from multiple model roots (batch operation)
    std::vector<std::pair<EntityID, ModelDelta>> ExtractAll(const DeltaExtractionConfig& config = {});
    
    /// Check if an entity is a model root (has ModelAssetGuid set)
    static bool IsModelRoot(const EntityData* data);
    
    /// Get all model roots in the scene
    std::vector<EntityID> GetModelRoots() const;

private:
    Scene& m_Scene;
    
    /// Build StableNodeId for an entity within a model
    StableNodeId BuildNodeId(EntityID entityId, EntityID modelRootId, const std::string& relativePath, int siblingIndex);
    
    /// Extract transform delta from entity
    std::optional<TransformDelta> ExtractTransformDelta(const EntityData* data, bool alwaysExtract);
    
    /// Extract mesh delta from entity  
    std::optional<MeshDelta> ExtractMeshDelta(const EntityData* data);
    
    /// Extract render overrides delta
    std::optional<RenderOverridesDelta> ExtractRenderOverridesDelta(const EntityData* data);
    
    /// Extract blend shape delta
    std::optional<BlendShapeDelta> ExtractBlendShapeDelta(const EntityData* data);
    
    /// Extract bone attachment delta
    std::optional<BoneAttachmentDelta> ExtractBoneAttachmentDelta(const EntityData* data, EntityID modelRootId);
    
    /// Extract LookAt constraints
    std::vector<LookAtConstraintDelta> ExtractLookAtConstraints(const EntityData* data, EntityID modelRootId);
    
    /// Extract IK constraints
    std::vector<IKConstraintDelta> ExtractIKConstraints(const EntityData* data, EntityID modelRootId);
    
    /// Extract scripts
    std::vector<ScriptDelta> ExtractScripts(const EntityData* data, EntityID modelRootId);
    
    /// Extract added light component
    std::optional<AddedLightDelta> ExtractAddedLight(const EntityData* data);
    
    /// Extract added collider component
    std::optional<AddedColliderDelta> ExtractAddedCollider(const EntityData* data);
    
    /// Extract added camera component
    std::optional<AddedCameraDelta> ExtractAddedCamera(const EntityData* data);
    
    /// Extract added emitter component
    std::optional<AddedEmitterDelta> ExtractAddedEmitter(const EntityData* data);
    
    /// Extract TintController component with entity paths for stable references
    std::optional<nlohmann::json> ExtractTintController(const EntityData* data, EntityID modelRootId);
    
    /// Build entity reference for stable serialization
    StableEntityRef BuildEntityRef(EntityID targetId, EntityID modelRootId);
    
    /// Compute relative path from model root to entity
    std::string ComputeRelativePath(EntityID modelRootId, EntityID entityId);
    
    /// Determine if a child entity is user-added (not part of original model)
    bool IsUserAddedChild(EntityID childId, EntityID modelRootId);
    
    /// Determine if an entity is a nested model root
    bool IsNestedModelRoot(EntityID entityId, EntityID parentModelRootId);
    
    /// Recursive extraction of node deltas
    void ExtractNodeDeltasRecursive(
        EntityID entityId,
        EntityID modelRootId,
        const std::string& parentPath,
        int siblingIndex,
        const DeltaExtractionConfig& config,
        ModelDelta& outDelta,
        std::vector<EntityID>& visitedNestedModels);
    
    /// Extract added children under a node
    void ExtractAddedChildren(
        EntityID parentId,
        EntityID modelRootId,
        const std::string& parentPath,
        const DeltaExtractionConfig& config,
        ModelDelta& outDelta);
};

} // namespace cm::model

