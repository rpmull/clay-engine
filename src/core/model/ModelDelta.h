#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "core/assets/AssetReference.h"
#include "core/ecs/Entity.h"
#include "core/rendering/MaterialPropertyBlock.h"

// Forward declarations
struct TransformComponent;
struct MeshComponent;
struct LightComponent;
struct ColliderComponent;
struct CameraComponent;
struct RigidBodyComponent;
struct StaticBodyComponent;
struct CharacterControllerComponent;
struct ParticleEmitterComponent;
struct BoneAttachmentComponent;
struct RenderOverridesComponent;
struct TintMaskController;
struct UnifiedMorphComponent;
struct BlendShapeComponent;

namespace cm {
namespace animation {
class AnimationPlayerComponent;
namespace lookat { struct LookAtConstraintComponent; }
namespace ik { struct IKComponent; }
}
}

struct ScriptInstance;

namespace cm::model {

// =============================================================================
// STABLE NODE IDENTITY
// =============================================================================

/// @brief Stable identity for a model node that survives re-export from DCC tools
/// 
/// The key insight is that we need multiple identification strategies that are
/// tried in order of reliability. This structure captures all the information
/// needed to robustly match nodes across model re-exports.
struct StableNodeId {
    /// Primary: Hierarchical path from model root (e.g., "Armature/Hips/Spine")
    std::string path;
    
    /// Secondary: Path with trailing _### suffixes stripped for fuzzy matching
    std::string normalizedPath;
    
    /// Tertiary: Node name only (for last-resort fuzzy matching)
    std::string nodeName;
    
    /// Normalized node name (trailing _### stripped)
    std::string normalizedName;
    
    /// Content hash computed from mesh data + local transform
    /// Useful for matching renamed nodes that still have same geometry
    uint64_t contentHash = 0;
    
    /// Structural signature: hash of (parent_normalized_path + sibling_index + node_type)
    /// More stable than content hash when geometry changes but structure stays same
    uint64_t structuralHash = 0;
    
    /// Node type for disambiguation
    enum class NodeType : uint8_t {
        Unknown = 0,
        MeshNode,       ///< Has MeshComponent
        SkeletonRoot,   ///< Has SkeletonComponent
        Bone,           ///< Child of skeleton, typically no mesh
        Empty,          ///< No mesh/skeleton, pure transform node
        UserAdded       ///< User-created entity, not from model file
    };
    NodeType type = NodeType::Unknown;
    
    /// Mesh file ID for mesh nodes (-1 if not applicable)
    int meshFileId = -1;
    
    /// Depth in hierarchy (0 = model root)
    int depth = 0;
    
    /// Entity GUID at serialization time (used for user-added entities)
    ClaymoreGUID entityGuid;
    
    // Comparison and hashing
    bool operator==(const StableNodeId& other) const;
    bool operator!=(const StableNodeId& other) const { return !(*this == other); }
    
    /// Match confidence when comparing two IDs
    enum class MatchConfidence {
        None = 0,       ///< No match possible
        FuzzyName,      ///< Only normalized name matched (low confidence)
        ContentHash,    ///< Content hash matched (medium confidence)  
        StructuralHash, ///< Structural hash matched (medium-high confidence)
        NormalizedPath, ///< Normalized path matched (high confidence)
        ExactPath,      ///< Exact path matched (highest confidence)
        ExactGuid       ///< Entity GUID matched (perfect for user-added)
    };
    
    /// Calculate match confidence against another ID
    MatchConfidence GetMatchConfidence(const StableNodeId& other) const;
    
    /// Serialize to JSON
    nlohmann::json ToJson() const;
    
    /// Deserialize from JSON
    static StableNodeId FromJson(const nlohmann::json& j);
    
    /// Compute normalized path from raw path
    static std::string ComputeNormalizedPath(const std::string& path);
    
    /// Compute normalized name from raw name  
    static std::string ComputeNormalizedName(const std::string& name);
    
    /// Compute content hash from mesh reference and transform
    static uint64_t ComputeContentHash(int meshFileId, const glm::mat4& localMatrix, int vertexCountHint = 0);
    
    /// Compute structural hash
    static uint64_t ComputeStructuralHash(const std::string& parentNormalizedPath, int siblingIndex, NodeType type);
};

// =============================================================================
// COMPONENT DELTAS
// =============================================================================

/// @brief Transform override delta
struct TransformDelta {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1, 0, 0, 0);
    glm::vec3 scale = glm::vec3(1.0f);
    bool positionModified = false;
    bool rotationModified = false;
    bool scaleModified = false;
    
    bool HasModifications() const { return positionModified || rotationModified || scaleModified; }
    
    nlohmann::json ToJson() const;
    static TransformDelta FromJson(const nlohmann::json& j);
};

/// @brief Mesh material override delta
struct MeshMaterialDelta {
    uint32_t slotIndex = 0;
    std::string materialAssetPath;      ///< Path to .mat file (empty = use default)
    MaterialPropertyBlock propertyBlock;
    std::unordered_map<std::string, std::string> texturePaths;
    bool alphaBlendEnabled = false;
    
    nlohmann::json ToJson() const;
    static MeshMaterialDelta FromJson(const nlohmann::json& j);
};

/// @brief Complete mesh override delta
struct MeshDelta {
    std::vector<MeshMaterialDelta> materialOverrides;
    bool showBackfaces = false;
    bool renderOnTop = false;
    int renderOrder = 0;
    float boundsPadding = 1.0f;
    bool uniqueMaterial = false;
    
    bool HasModifications() const { return !materialOverrides.empty() || showBackfaces || renderOnTop || renderOrder != 0; }
    
    nlohmann::json ToJson() const;
    static MeshDelta FromJson(const nlohmann::json& j);
};

/// @brief Render overrides delta
struct RenderOverridesDelta {
    bool alphaBlendEnabled = false;
    bool useAlphaCutout = false;
    float alphaCutoutThreshold = 0.5f;
    bool depthWriteEnabled = true;
    bool castShadows = true;
    bool receiveShadows = true;
    bool visible = true;
    int sortingOrder = 0;
    
    nlohmann::json ToJson() const;
    static RenderOverridesDelta FromJson(const nlohmann::json& j);
};

/// @brief Entity reference that can survive ID changes
struct StableEntityRef {
    /// Target entity's GUID
    ClaymoreGUID targetGuid;
    
    /// Path within the same model (if target is in same model)
    std::string modelNodePath;
    
    /// Model GUID (if target is in a different model)
    ClaymoreGUID modelGuid;
    
    /// Scene path (absolute path from scene root)
    std::string scenePath;
    
    /// Runtime resolved ID (INVALID_ENTITY_ID if not resolved)
    mutable EntityID resolvedId = INVALID_ENTITY_ID;
    
    bool IsValid() const { return !(targetGuid.high == 0 && targetGuid.low == 0) || !modelNodePath.empty() || !scenePath.empty(); }
    
    nlohmann::json ToJson() const;
    static StableEntityRef FromJson(const nlohmann::json& j);
};

/// @brief LookAt constraint delta (stored with entity reference)
struct LookAtConstraintDelta {
    bool enabled = true;
    float weight = 1.0f;
    uint8_t mode = 0;           ///< LookAtMode enum
    uint8_t axes = 0;           ///< AxisMask enum
    uint8_t space = 0;          ///< LookAtSpace enum
    StableEntityRef target;
    float smoothingSpeed = 10.0f;
    float maxYawDeg = 90.0f;
    float maxPitchDeg = 60.0f;
    std::vector<int32_t> boneChain;
    
    nlohmann::json ToJson() const;
    static LookAtConstraintDelta FromJson(const nlohmann::json& j);
};

/// @brief IK constraint delta
struct IKConstraintDelta {
    bool enabled = true;
    float weight = 1.0f;
    StableEntityRef target;
    StableEntityRef pole;
    bool useTwoBone = true;
    float maxIterations = 12.0f;
    float tolerance = 0.001f;
    float damping = 0.2f;
    bool lockAxisX = false;
    bool lockAxisY = false;
    bool lockAxisZ = false;
    std::vector<int32_t> chain;
    int32_t chainRootHint = -1;
    int32_t chainEffectorHint = -1;
    
    nlohmann::json ToJson() const;
    static IKConstraintDelta FromJson(const nlohmann::json& j);
};

/// @brief Bone attachment delta
struct BoneAttachmentDelta {
    std::string targetBoneName;
    StableEntityRef skeletonRef;    ///< Stable reference to skeleton entity
    glm::vec3 localPosition = glm::vec3(0.0f);
    glm::vec3 localRotation = glm::vec3(0.0f);
    glm::vec3 localScale = glm::vec3(1.0f);
    bool inheritRotation = true;
    bool inheritScale = false;
    bool enabled = true;
    
    nlohmann::json ToJson() const;
    static BoneAttachmentDelta FromJson(const nlohmann::json& j);
};

/// @brief Script instance delta
struct ScriptDelta {
    std::string className;
    std::unordered_map<std::string, nlohmann::json> properties;  ///< Serialized property values
    
    nlohmann::json ToJson() const;
    static ScriptDelta FromJson(const nlohmann::json& j);
};

/// @brief Blend shape weight overrides
struct BlendShapeDelta {
    std::unordered_map<std::string, float> weights;
    
    nlohmann::json ToJson() const;
    static BlendShapeDelta FromJson(const nlohmann::json& j);
};

// =============================================================================
// ADDED COMPONENTS (User-added components that don't exist in base model)
// =============================================================================

/// @brief Light component added by user
struct AddedLightDelta {
    int type = 0;  ///< LightType enum
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    
    nlohmann::json ToJson() const;
    static AddedLightDelta FromJson(const nlohmann::json& j);
};

/// @brief Collider component added by user
struct AddedColliderDelta {
    int shapeType = 0;  ///< ColliderShape enum
    glm::vec3 offset = glm::vec3(0.0f);
    glm::vec3 size = glm::vec3(1.0f);
    float radius = 0.5f;
    float height = 1.0f;
    bool isTrigger = false;
    std::string physicsLayerName = "Default";
    
    nlohmann::json ToJson() const;
    static AddedColliderDelta FromJson(const nlohmann::json& j);
};

/// @brief Camera component added by user
struct AddedCameraDelta {
    float fieldOfView = 60.0f;
    float nearClip = 0.1f;
    float farClip = 1000.0f;
    bool isPerspective = true;
    bool active = false;
    int priority = 0;
    
    nlohmann::json ToJson() const;
    static AddedCameraDelta FromJson(const nlohmann::json& j);
};

/// @brief Particle emitter added by user (simplified - full data is large)
struct AddedEmitterDelta {
    std::string spritePath;
    uint32_t maxParticles = 1024;
    float emissionRate = 100.0f;
    bool enabled = true;
    nlohmann::json fullData;  ///< Complete emitter config as JSON
    
    nlohmann::json ToJson() const;
    static AddedEmitterDelta FromJson(const nlohmann::json& j);
};

// =============================================================================
// NODE DELTA (Per-node modifications)
// =============================================================================

/// @brief Complete set of modifications to a single model node
struct NodeDelta {
    /// Identity of the node this delta applies to
    StableNodeId nodeId;
    
    /// Entity-level properties
    std::string name;               ///< Renamed name (empty = keep original)
    std::optional<int> layer;       ///< Modified layer
    std::optional<std::string> tag; ///< Modified tag  
    std::optional<bool> visible;    ///< Modified visibility
    std::optional<bool> active;     ///< Modified active state
    
    /// Transform modifications
    std::optional<TransformDelta> transform;
    
    /// Mesh modifications (only for mesh nodes)
    std::optional<MeshDelta> mesh;
    
    /// Render override modifications
    std::optional<RenderOverridesDelta> renderOverrides;
    
    /// Blend shape weight modifications
    std::optional<BlendShapeDelta> blendShapes;
    
    /// Bone attachment (can be added to any node)
    std::optional<BoneAttachmentDelta> boneAttachment;
    
    /// Animation constraints
    std::vector<LookAtConstraintDelta> lookAtConstraints;
    std::vector<IKConstraintDelta> ikConstraints;
    
    /// User-added scripts
    std::vector<ScriptDelta> scripts;
    
    /// User-added components (nullable - only present if user added them)
    std::optional<AddedLightDelta> addedLight;
    std::optional<AddedColliderDelta> addedCollider;
    std::optional<AddedCameraDelta> addedCamera;
    std::optional<AddedEmitterDelta> addedEmitter;
    
    /// TintController component (stored as JSON with entityPath for target resolution)
    std::optional<nlohmann::json> tintController;
    
    /// Extra JSON data for forward compatibility
    nlohmann::json extra;
    
    /// Check if this delta has any modifications
    bool HasModifications() const;
    
    /// Serialize to JSON
    nlohmann::json ToJson() const;
    
    /// Deserialize from JSON
    static NodeDelta FromJson(const nlohmann::json& j);
};

// =============================================================================
// ADDED CHILD ENTITY (User-created entities under model)
// =============================================================================

/// @brief A user-created entity added under a model node
struct AddedChildDelta {
    /// Identity of this added entity
    StableNodeId childId;
    
    /// Parent node within the model (where this child is attached)
    StableNodeId parentNodeId;
    
    /// Complete entity data serialized as JSON
    /// (This includes all components, transform, etc.)
    nlohmann::json entityData;
    
    /// For nested models: the model asset reference
    std::optional<AssetReference> nestedModelAsset;
    
    /// Is this a nested model root?
    bool isNestedModel = false;
    
    nlohmann::json ToJson() const;
    static AddedChildDelta FromJson(const nlohmann::json& j);
};

// =============================================================================
// MODEL DELTA (Complete delta for a model instance)
// =============================================================================

/// @brief Complete set of modifications to a model instance
/// 
/// This is the top-level structure that captures ALL user modifications to a
/// model that should survive hot reload and scene save/load. It is designed
/// to be stored separately from entity data, enabling:
/// 1. Clear ownership (delta belongs to a specific model instance)
/// 2. Version tracking (detect obsolete deltas after model structure changes)
/// 3. Atomic operations (apply all or nothing)
struct ModelDelta {
    /// Model asset GUID this delta was created for
    ClaymoreGUID modelAssetGuid;
    
    /// Model asset path (for display/debugging)
    std::string modelAssetPath;
    
    /// Version of the model structure when this delta was created
    /// Incremented when model is re-imported with structural changes
    uint32_t modelStructureVersion = 0;
    
    /// Timestamp when delta was created (for debugging)
    uint64_t createdTimestamp = 0;
    
    /// Per-node modifications (indexed by normalized path for quick lookup)
    std::vector<NodeDelta> nodeDeltas;
    
    /// Nodes explicitly deleted by user (paths relative to model root)
    std::vector<std::string> deletedNodePaths;
    
    /// User-added child entities (not part of original model)
    std::vector<AddedChildDelta> addedChildren;
    
    /// Model root entity modifications (name, transform, layer, etc.)
    std::optional<NodeDelta> rootDelta;
    
    // ==== Methods ====
    
    /// Check if this delta has any modifications
    bool IsEmpty() const;
    
    /// Check if delta is potentially stale (model version mismatch)
    bool IsPotentiallyStale(uint32_t currentModelVersion) const;
    
    /// Find node delta by path (returns nullptr if not found)
    const NodeDelta* FindNodeDelta(const std::string& nodePath) const;
    NodeDelta* FindNodeDelta(const std::string& nodePath);
    
    /// Find node delta by stable ID with fallback matching
    const NodeDelta* FindNodeDeltaByStableId(const StableNodeId& id) const;
    NodeDelta* FindNodeDeltaByStableId(const StableNodeId& id);
    
    /// Add or update a node delta
    NodeDelta& GetOrCreateNodeDelta(const StableNodeId& nodeId);
    
    /// Serialize to JSON
    nlohmann::json ToJson() const;
    
    /// Deserialize from JSON
    static ModelDelta FromJson(const nlohmann::json& j);
    
    /// Serialize to binary format
    std::vector<uint8_t> ToBinary() const;
    
    /// Deserialize from binary format
    static ModelDelta FromBinary(const uint8_t* data, size_t size);
    
    // ==== Version tracking ====
    static constexpr uint32_t DELTA_FORMAT_VERSION = 1;
    static constexpr uint32_t DELTA_BINARY_MAGIC = 'M' | ('D' << 8) | ('L' << 16) | ('T' << 24);  // 'MDLT'
};

// =============================================================================
// DELTA MATCH RESULT (Result of matching delta to model)
// =============================================================================

/// @brief Result of attempting to match a delta to a model node
struct DeltaMatchResult {
    /// The original node delta
    const NodeDelta* delta = nullptr;
    
    /// Matched entity ID (INVALID_ENTITY_ID if not matched)
    EntityID matchedEntity = INVALID_ENTITY_ID;
    
    /// Match confidence
    StableNodeId::MatchConfidence confidence = StableNodeId::MatchConfidence::None;
    
    /// Warning message if match was uncertain
    std::string warning;
    
    /// Was this node reparented (matched by fallback, not path)?
    bool reparented = false;
    
    /// New intended parent path (if reparenting needed)
    std::string intendedParentPath;
};

/// @brief Summary of delta application results  
struct DeltaApplicationResult {
    /// Number of deltas successfully applied
    uint32_t appliedCount = 0;
    
    /// Number of deltas that couldn't be matched
    uint32_t unmatchedCount = 0;
    
    /// Number of user-added children restored
    uint32_t addedChildrenCount = 0;
    
    /// Number of deleted nodes applied
    uint32_t deletedNodesCount = 0;
    
    /// Detailed results per node
    std::vector<DeltaMatchResult> nodeResults;
    
    /// Warnings generated during application
    std::vector<std::string> warnings;
    
    /// Errors (fatal issues that prevented application)
    std::vector<std::string> errors;
    
    bool HasErrors() const { return !errors.empty(); }
    bool HasWarnings() const { return !warnings.empty(); }
    bool IsComplete() const { return unmatchedCount == 0 && !HasErrors(); }
};

} // namespace cm::model

