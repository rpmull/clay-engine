#pragma once
#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabInstanceComponent.h"
#include "core/ecs/Scene.h"
#include "core/assets/AssetReference.h"

//==============================================================================
// Prefab Editor API
//==============================================================================
// Editor-only functions for creating, editing, and managing prefabs.
// These functions require AssetLibrary and other editor infrastructure.

namespace prefab_editor {

//------------------------------------------------------------------------------
// Prefab Building
//------------------------------------------------------------------------------

// Build a PrefabAsset from a scene hierarchy using Serializer::SerializeEntity
// This creates a prefab with the exact same serialization format as scenes.
bool BuildPrefabAssetFromScene(Scene& scene, EntityID root, PrefabAsset& out);

// Save a prefab asset to disk
bool SavePrefab(const std::string& path, const PrefabAsset& prefab);

// Save prefab by GUID (path resolved from AssetLibrary)
bool SavePrefabByGuid(const ClaymoreGUID& prefabGuid, const PrefabAsset& prefab);

// If a prefab already exists at this path, keep its asset GUID when overwriting it.
bool AdoptExistingPrefabAssetGuid(const std::string& prefabPath, PrefabAsset& prefab);

// Register a just-saved prefab asset and link the source scene root as a prefab instance.
// This is the shared editor path used after saving a scene hierarchy to disk.
bool FinalizeSavedPrefabFromScene(
    Scene& scene,
    EntityID root,
    const std::string& prefabPath,
    const PrefabAsset& prefab,
    std::string* outVirtualPath = nullptr
);

//------------------------------------------------------------------------------
// Override Computation
//------------------------------------------------------------------------------

// Compute property overrides between a base prefab and an edited instance
// Returns overrides that can be stored in PrefabInstanceComponent
std::vector<prefab::PropertyOverride> ComputeOverrides(
    const PrefabAsset& basePrefab, 
    Scene& instanceScene, 
    EntityID instanceRoot
);

// Detect which entities in an instance have been modified from the base prefab
std::vector<ClaymoreGUID> GetModifiedEntityGuids(
    const PrefabAsset& basePrefab,
    Scene& instanceScene,
    EntityID instanceRoot
);

// Detect which entities were added to the instance (not in base prefab)
std::vector<prefab::AddedEntity> GetAddedEntities(
    const PrefabAsset& basePrefab,
    Scene& instanceScene,
    EntityID instanceRoot
);

// Detect which entities were removed from the instance
std::vector<prefab::RemovedEntity> GetRemovedEntities(
    const PrefabAsset& basePrefab,
    Scene& instanceScene,
    EntityID instanceRoot
);

//------------------------------------------------------------------------------
// Validation
//------------------------------------------------------------------------------

struct PrefabDiagnostics {
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
    
    bool HasErrors() const { return !Errors.empty(); }
    bool IsValid() const { return Errors.empty(); }
};

PrefabDiagnostics ValidatePrefab(const PrefabAsset& prefab);
PrefabDiagnostics ValidatePrefabFile(const std::string& path);

//------------------------------------------------------------------------------
// Model Root Detection
//------------------------------------------------------------------------------

// Check if an entity is an imported model root
bool IsImportedModelRoot(Scene& scene, EntityID id, std::string& outModelPath, ClaymoreGUID& outGuid);

// Check if an entity is part of an imported model (descendant of model root)
bool IsModelDescendant(Scene& scene, EntityID id, EntityID& outModelRoot);

//------------------------------------------------------------------------------
// Apply Changes to Prefab Asset
//------------------------------------------------------------------------------

// Apply instance modifications back to the prefab asset (like Unity's "Apply" button)
bool ApplyInstanceToPrefab(
    Scene& instanceScene,
    EntityID instanceRoot,
    PrefabAsset& prefabAsset
);

// Apply only selected overrides back to prefab
bool ApplySelectedOverridesToPrefab(
    Scene& instanceScene,
    EntityID instanceRoot,
    const std::vector<prefab::PropertyOverride>& overridesToApply,
    PrefabAsset& prefabAsset
);

} // namespace prefab_editor

