#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "core/assets/AssetReference.h"

//==============================================================================
// PrefabInstanceComponent - SIMPLIFIED
//==============================================================================
// Marks an entity as the root of an instantiated prefab. This component tracks:
// - Which prefab asset this instance came from
// - GUID remapping (prefab GUID -> instance GUID) for entity resolution
//
// NOTE: Per-instance overrides (property modifications, added/removed entities)
// are now stored in the scene file's 'children' array on the prefab instance root,
// using the same pattern as model roots. This simplifies the prefab system by
// unifying the serialization path with scenes and eliminating complex baseline
// comparison logic.
//
// The override tracking below is DEPRECATED and kept only for backward
// compatibility during migration. New code should not use these fields.
//==============================================================================

namespace prefab {

// DEPRECATED: These structs are kept for backward compatibility only.
// Override data is now stored in scene's 'children' array.

//------------------------------------------------------------------------------
// PropertyOverride - Unified override format
//------------------------------------------------------------------------------
// Represents a single property modification on a prefab instance.
// This replaces the separate ModelDelta and PrefabOverrides systems.

struct PropertyOverride {
    // Target entity within the prefab instance (by GUID for stability)
    ClaymoreGUID TargetEntityGuid;
    
    // Component being overridden (matches Serializer keys):
    // "transform", "mesh", "light", "collider", "scripts", "animator", etc.
    // Special values:
    //   "name" - Entity name override
    //   "active" - Entity active state
    //   "visible" - Entity visibility
    //   "layer" - Entity layer
    //   "tag" - Entity tag
    std::string ComponentKey;
    
    // The override value. For components, this is the full serialized component
    // (matching Serializer::SerializeXxx output). For simple properties like
    // "name", this is a scalar value.
    nlohmann::json Value;
    
    // Resolution hints for fuzzy matching (used when model re-exports change node names)
    // These are populated during delta computation and used if GUID resolution fails.
    struct ResolutionHints {
        std::string NodePath;           // Hierarchical path from instance root
        std::string NormalizedPath;     // Path with numeric suffixes stripped
        std::string NormalizedName;     // Name with numeric suffixes stripped
        std::string StableMeshName;     // MeshComponent::MeshName for mesh entities
        std::string ParentNormalizedName;
        uint64_t ContentHash = 0;       // Hash of mesh reference + transform
        int MeshFileId = -1;            // MeshReference file ID
    };
    ResolutionHints Hints;
    
    bool operator==(const PropertyOverride& other) const {
        return TargetEntityGuid == other.TargetEntityGuid &&
               ComponentKey == other.ComponentKey &&
               Value == other.Value;
    }
};

//------------------------------------------------------------------------------
// AddedEntity - Entity added to instance (not in base prefab)
//------------------------------------------------------------------------------

struct AddedEntity {
    ClaymoreGUID Guid;              // GUID of the added entity
    std::string Name;               // Entity name
    ClaymoreGUID ParentGuid;        // Parent entity GUID (within prefab instance)
    std::string ParentNodePath;     // Parent path relative to prefab root
    std::string ParentNormalizedPath;
    std::string ParentNormalizedName;
    std::string ParentStableMeshName;
    uint64_t ParentContentHash = 0;
    int ParentMeshFileId = -1;
    nlohmann::json Components;      // Full serialized entity (Serializer format)
    std::vector<AddedEntity> Children; // Recursive children
};

//------------------------------------------------------------------------------
// RemovedEntity - Entity removed from instance (was in base prefab)
//------------------------------------------------------------------------------

struct RemovedEntity {
    ClaymoreGUID Guid;              // GUID of removed entity
    
    // Resolution hints (same as PropertyOverride) for matching after re-export
    std::string NodePath;
    std::string NormalizedPath;
    std::string NormalizedName;
    std::string StableMeshName;
    std::string ParentNormalizedName;
    uint64_t ContentHash = 0;
    int MeshFileId = -1;
};

} // namespace prefab

//==============================================================================
// PrefabInstanceComponent
//==============================================================================

struct PrefabInstanceComponent {
    //--------------------------------------------------------------------------
    // Source prefab identity
    //--------------------------------------------------------------------------
    
    ClaymoreGUID PrefabAssetGuid;   // GUID of the source prefab asset
    std::string PrefabPath;          // Virtual path to prefab (for display/debugging)
    
    //--------------------------------------------------------------------------
    // Instance membership tracking
    //--------------------------------------------------------------------------
    
    // GUIDs of all entities that belong to this prefab instance.
    // This includes the root entity itself and all descendants that came from
    // the prefab (not user-added children of non-instance entities).
    // NOTE: These are the NEW instance-specific GUIDs, not the original prefab GUIDs.
    std::vector<ClaymoreGUID> OwnedEntityGuids;
    
    //--------------------------------------------------------------------------
    // GUID Remapping (prefab GUID → instance GUID)
    //--------------------------------------------------------------------------
    
    // When a prefab is instantiated, each entity gets a new unique GUID to avoid
    // collisions with other instances of the same prefab. This map tracks:
    //   Key: Original GUID from the prefab asset
    //   Value: New unique GUID assigned to this instance
    // This is needed for:
    //   - Resolving override targets (overrides reference original prefab GUIDs)
    //   - IK/LookAt target resolution within the instance
    //   - Re-serializing overrides with original prefab GUIDs for portability
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> PrefabToInstanceGuid;
    
    // Reverse map for fast lookup (instance GUID → prefab GUID)
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> InstanceToPrefabGuid;
    
    //--------------------------------------------------------------------------
    // Per-instance modifications - DEPRECATED
    // These fields are kept for backward compatibility during migration.
    // New scenes store overrides in the 'children' array on the entity JSON.
    //--------------------------------------------------------------------------
    
    // DEPRECATED: Property overrides now in scene's 'children' array
    std::vector<prefab::PropertyOverride> Overrides;
    
    // DEPRECATED: Added entities now in scene's 'children' array with _added flag
    std::vector<prefab::AddedEntity> AddedEntities;
    
    // DEPRECATED: Removed entities now tracked in scene's deletedModelNodes array
    std::vector<prefab::RemovedEntity> RemovedEntities;
    
    //--------------------------------------------------------------------------
    // Model asset tracking (for model-based prefabs)
    //--------------------------------------------------------------------------
    
    // If this prefab contains an imported model, track it for hot-reload
    ClaymoreGUID ModelAssetGuid;
    std::string ModelAssetPath;
    
    //--------------------------------------------------------------------------
    // Helper methods
    //--------------------------------------------------------------------------
    
    // Check if an entity GUID is owned by this instance
    bool OwnsEntity(const ClaymoreGUID& guid) const {
        for (const auto& g : OwnedEntityGuids) {
            if (g == guid) return true;
        }
        return false;
    }
    
    //--------------------------------------------------------------------------
    // DEPRECATED Helper methods - kept for backward compatibility only
    // Override data is now in scene's 'children' array
    //--------------------------------------------------------------------------
    
    // DEPRECATED: Check if an entity has any overrides
    bool HasOverrides(const ClaymoreGUID& entityGuid) const {
        for (const auto& ov : Overrides) {
            if (ov.TargetEntityGuid == entityGuid) return true;
        }
        return false;
    }
    
    // DEPRECATED: Get overrides for a specific entity
    std::vector<const prefab::PropertyOverride*> GetOverridesFor(const ClaymoreGUID& entityGuid) const {
        std::vector<const prefab::PropertyOverride*> result;
        for (const auto& ov : Overrides) {
            if (ov.TargetEntityGuid == entityGuid) {
                result.push_back(&ov);
            }
        }
        return result;
    }
    
    // DEPRECATED: Check if an entity was added (not in base prefab)
    bool IsAddedEntity(const ClaymoreGUID& guid) const {
        std::function<bool(const std::vector<prefab::AddedEntity>&)> search = 
            [&](const std::vector<prefab::AddedEntity>& list) -> bool {
            for (const auto& added : list) {
                if (added.Guid == guid) return true;
                if (search(added.Children)) return true;
            }
            return false;
        };
        return search(AddedEntities);
    }
    
    // DEPRECATED: Check if an entity was removed
    bool IsRemovedEntity(const ClaymoreGUID& guid) const {
        for (const auto& removed : RemovedEntities) {
            if (removed.Guid == guid) return true;
        }
        return false;
    }
    
    // DEPRECATED: Remove all overrides for an entity (revert to prefab default)
    void RevertEntity(const ClaymoreGUID& entityGuid) {
        Overrides.erase(
            std::remove_if(Overrides.begin(), Overrides.end(),
                [&](const prefab::PropertyOverride& ov) {
                    return ov.TargetEntityGuid == entityGuid;
                }),
            Overrides.end()
        );
    }
    
    // DEPRECATED: Remove a specific component override
    void RevertComponent(const ClaymoreGUID& entityGuid, const std::string& componentKey) {
        Overrides.erase(
            std::remove_if(Overrides.begin(), Overrides.end(),
                [&](const prefab::PropertyOverride& ov) {
                    return ov.TargetEntityGuid == entityGuid && 
                           ov.ComponentKey == componentKey;
                }),
            Overrides.end()
        );
    }
    
    // DEPRECATED: Clear all modifications (full revert)
    void RevertAll() {
        Overrides.clear();
        AddedEntities.clear();
        RemovedEntities.clear();
    }
};


