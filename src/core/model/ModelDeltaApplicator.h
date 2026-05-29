#pragma once

#include "ModelDelta.h"
#include "core/ecs/Entity.h"
#include <functional>
#include <unordered_map>
#include <unordered_set>

class Scene;
struct EntityData;

namespace cm::model {

/// @brief Configuration for delta application
struct DeltaApplicationConfig {
    /// Apply transform deltas
    bool applyTransforms = true;
    
    /// Apply mesh material overrides
    bool applyMeshOverrides = true;
    
    /// Apply user-added components (lights, colliders, cameras, etc.)
    bool applyAddedComponents = true;
    
    /// Apply scripts
    bool applyScripts = true;
    
    /// Apply animation constraints (LookAt, IK)
    bool applyConstraints = true;
    
    /// Apply user-added children
    bool applyAddedChildren = true;
    
    /// Apply deleted node tracking (hide/remove nodes)
    bool applyDeletedNodes = true;
    
    /// Apply blend shape weights
    bool applyBlendShapes = true;
    
    /// Entity-level overrides (name, layer, tag, visible, active)
    bool applyEntityOverrides = true;
    
    /// Minimum match confidence to apply delta (lower = more permissive)
    StableNodeId::MatchConfidence minMatchConfidence = StableNodeId::MatchConfidence::FuzzyName;
    
    /// Verbose logging
    bool verbose = false;
    
    /// If true, warn but don't fail on unmatched deltas
    bool allowUnmatchedDeltas = true;
    
    /// If true, attempt to reparent nodes to match delta's intended hierarchy
    bool attemptReparenting = false;

    /// If true, use fast entity creation/parenting (runtime prefab instantiation).
    bool fastHierarchy = false;
};

/// @brief Validates a ModelDelta against a model structure
/// @param delta The delta to validate
/// @param modelRootId The root entity of the model to validate against
/// @param scene The scene containing the model
/// @return Vector of validation warning messages (empty = valid)
std::vector<std::string> ValidateModelDelta(
    const ModelDelta& delta,
    EntityID modelRootId,
    Scene& scene);

/// @brief Unified applicator for applying ModelDelta to model instances
/// 
/// This class provides a single implementation for delta restoration that is
/// used by both:
/// - Scene load (JSON/binary) - restores deltas to freshly instantiated models
/// - Hot reload - restores deltas after model re-instantiation
/// 
/// This eliminates the "dual code paths" problem by ensuring identical restoration
/// logic regardless of the restoration context.
class ModelDeltaApplicator {
public:
    explicit ModelDeltaApplicator(Scene& scene);
    
    /// Apply a delta to a model root entity
    /// @param modelRootId The entity ID of the model root
    /// @param delta The delta to apply
    /// @param config Application configuration
    /// @return Detailed result of the application
    DeltaApplicationResult Apply(
        EntityID modelRootId, 
        const ModelDelta& delta,
        const DeltaApplicationConfig& config = {});
    
    /// Build lookup maps for a model's entities (for efficient matching)
    /// This is called automatically by Apply but can be pre-built for batch operations
    void BuildModelLookupMaps(EntityID modelRootId);
    
    /// Clear cached lookup maps
    void ClearLookupMaps();
    
    /// Callback for resolving entity references during application
    /// Signature: (StableEntityRef, Scene&) -> EntityID
    using EntityRefResolver = std::function<EntityID(const StableEntityRef&, Scene&)>;
    
    /// Set custom entity reference resolver
    void SetEntityRefResolver(EntityRefResolver resolver) { m_RefResolver = std::move(resolver); }
    
    /// Default entity reference resolver
    static EntityID DefaultRefResolver(const StableEntityRef& ref, Scene& scene);

private:
    Scene& m_Scene;
    EntityRefResolver m_RefResolver;
    
    // Lookup maps for efficient matching (built per model)
    struct ModelLookupMaps {
        EntityID rootId = INVALID_ENTITY_ID;
        std::unordered_map<std::string, EntityID> byPath;          ///< Exact path -> entity
        std::unordered_map<std::string, EntityID> byNormalizedPath; ///< Normalized path -> entity
        std::unordered_map<std::string, std::vector<EntityID>> byNormalizedName; ///< Name -> entities (multiple)
        std::unordered_map<int, std::vector<EntityID>> byMeshFileId;   ///< File ID -> entities
        std::unordered_map<uint64_t, std::vector<EntityID>> byContentHash;  ///< Content hash -> entities
        std::unordered_map<uint64_t, uint64_t> guidHigh;           ///< GUID high -> low (for quick lookup)
        std::unordered_map<uint64_t, EntityID> byGuidLow;          ///< GUID low -> entity
    };
    
    ModelLookupMaps m_CurrentMaps;
    
    /// Build lookup maps recursively
    void BuildMapsRecursive(EntityID entityId, EntityID modelRootId, const std::string& parentPath);
    
    /// Find entity matching a StableNodeId using fallback strategy
    std::pair<EntityID, StableNodeId::MatchConfidence> FindMatchingEntity(
        const StableNodeId& nodeId,
        const ModelLookupMaps& maps) const;
    
    /// Apply a single node delta
    bool ApplyNodeDelta(
        EntityID targetEntity,
        const NodeDelta& delta,
        const DeltaApplicationConfig& config,
        DeltaApplicationResult& result);
    
    /// Apply transform delta to entity
    void ApplyTransformDelta(EntityData* data, const TransformDelta& delta);
    
    /// Apply mesh delta to entity
    void ApplyMeshDelta(EntityData* data, const MeshDelta& delta);
    
    /// Apply render overrides delta to entity
    void ApplyRenderOverridesDelta(EntityData* data, const RenderOverridesDelta& delta);
    
    /// Apply blend shape delta to entity
    void ApplyBlendShapeDelta(EntityData* data, const BlendShapeDelta& delta);
    
    /// Apply bone attachment delta to entity
    void ApplyBoneAttachmentDelta(EntityData* data, const BoneAttachmentDelta& delta, EntityID modelRootId);
    
    /// Apply LookAt constraints to entity
    void ApplyLookAtConstraints(EntityData* data, const std::vector<LookAtConstraintDelta>& deltas, EntityID modelRootId);
    
    /// Apply IK constraints to entity
    void ApplyIKConstraints(EntityData* data, const std::vector<IKConstraintDelta>& deltas, EntityID modelRootId);
    
    /// Apply scripts to entity
    void ApplyScripts(EntityData* data, const std::vector<ScriptDelta>& deltas);
    
    /// Apply added light component
    void ApplyAddedLight(EntityData* data, const AddedLightDelta& delta);
    
    /// Apply added collider component
    void ApplyAddedCollider(EntityData* data, const AddedColliderDelta& delta);
    
    /// Apply added camera component
    void ApplyAddedCamera(EntityData* data, const AddedCameraDelta& delta);
    
    /// Apply added emitter component
    void ApplyAddedEmitter(EntityData* data, const AddedEmitterDelta& delta);
    
    /// Apply TintController component from JSON delta (resolves entity paths within model)
    /// @param data The entity data to apply the TintController to
    /// @param tintJson The TintController JSON delta
    /// @param targetEntityId The entity where the TintController is being applied (for self-references)
    /// @param modelRootId The model root entity (for path resolution)
    void ApplyTintControllerDelta(EntityData* data, const nlohmann::json& tintJson, EntityID targetEntityId, EntityID modelRootId);
    
    /// Apply deleted nodes (mark as invisible or remove)
    void ApplyDeletedNodes(
        EntityID modelRootId,
        const std::vector<std::string>& deletedPaths,
        DeltaApplicationResult& result);
    
    /// Apply added children (create entities)
    void ApplyAddedChildren(
        EntityID modelRootId,
        const std::vector<AddedChildDelta>& addedChildren,
        const DeltaApplicationConfig& config,
        DeltaApplicationResult& result);
    
    /// Resolve a stable entity reference to an entity ID
    EntityID ResolveEntityRef(const StableEntityRef& ref, EntityID modelRootId);
    
    /// Helper: Get entity by path relative to model root
    EntityID GetEntityByPath(EntityID modelRootId, const std::string& path) const;
    
    /// Helper: Compute entity path relative to model root
    std::string ComputeEntityPath(EntityID modelRootId, EntityID entityId) const;
    
    /// Helper: Check if entity is a descendant of another
    bool IsDescendantOf(EntityID entityId, EntityID ancestorId) const;
};

} // namespace cm::model

