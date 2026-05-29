#pragma once
#include "core/assets/AssetReference.h"
#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabInstanceComponent.h"
#include "core/ecs/Scene.h"

//==============================================================================
// Prefab Runtime API
//==============================================================================
// Clean, unified prefab system that uses the same serialization as scenes.
// Prefabs are "packed scenes" - self-contained entity hierarchies that can
// be instantiated into any scene.
//
// Key concepts:
// - PrefabAsset: The asset definition (stored in .prefab files)
// - PrefabInstanceComponent: Attached to instance roots, tracks overrides
// - PropertyOverride: Per-instance modifications to prefab properties
//==============================================================================

//------------------------------------------------------------------------------
// Instantiation
//------------------------------------------------------------------------------

// Instantiate a prefab by GUID into the target scene.
// Returns the root entity ID of the instantiated prefab, or INVALID_ENTITY_ID on failure.
// The root entity will have a PrefabInstanceComponent attached.
EntityID InstantiatePrefab(const ClaymoreGUID& prefabGuid, Scene& dst,
                           EntityID existingRoot = INVALID_ENTITY_ID,
                           bool useExistingRoot = false);

// Instantiate a prefab by GUID using blocking/synchronous behavior (even in play mode).
EntityID InstantiatePrefabBlocking(const ClaymoreGUID& prefabGuid, Scene& dst,
                                   EntityID existingRoot = INVALID_ENTITY_ID,
                                   bool useExistingRoot = false);

// Instantiate from in-memory PrefabAsset. Single code path for JSON prefab -> scene;
// used by InstantiatePrefabFromPath (editor/runtime load) and BinaryAssetCache (binary build).
// prefabPathForInstance: if non-null, stored on PrefabInstanceComponent; pass null when building binary.
EntityID InstantiatePrefabAsset(const PrefabAsset& asset, Scene& dst,
                                EntityID existingRoot = INVALID_ENTITY_ID,
                                bool useExistingRoot = false,
                                const char* prefabPathForInstance = nullptr);

// Instantiate a prefab from a file path.
EntityID InstantiatePrefabFromPath(const std::string& prefabPath, Scene& dst,
                                   EntityID existingRoot = INVALID_ENTITY_ID,
                                   bool useExistingRoot = false);

// Instantiate a prefab from a file path with blocking/synchronous behavior.
EntityID InstantiatePrefabFromPathBlocking(const std::string& prefabPath, Scene& dst,
                                           EntityID existingRoot = INVALID_ENTITY_ID,
                                           bool useExistingRoot = false);

//------------------------------------------------------------------------------
// Instance Queries
//------------------------------------------------------------------------------

// Check if an entity is a prefab instance root
bool IsPrefabInstanceRoot(EntityID entity, Scene& scene);

// Check if an entity belongs to a prefab instance (is owned by an instance root)
bool IsPartOfPrefabInstance(EntityID entity, Scene& scene);

// Find the prefab instance root for an entity (returns INVALID_ENTITY_ID if not in a prefab)
EntityID GetPrefabInstanceRoot(EntityID entity, Scene& scene);

// Get the PrefabInstanceComponent for an entity (returns nullptr if not a root)
PrefabInstanceComponent* GetPrefabInstance(EntityID entity, Scene& scene);

//------------------------------------------------------------------------------
// Override Management
//------------------------------------------------------------------------------

// Apply all overrides from a PrefabInstanceComponent to the instance entities
void ApplyPrefabOverrides(EntityID instanceRoot, Scene& scene);

// Apply a list of overrides to an instance (used during hot-reload)
void ApplyPrefabOverrides(EntityID instanceRoot, Scene& scene, const std::vector<prefab::PropertyOverride>& overrides);

// Revert an entity to prefab defaults (remove its overrides)
void RevertEntityToPrefab(EntityID entity, Scene& scene);

// Revert a specific component to prefab defaults
void RevertComponentToPrefab(EntityID entity, const std::string& componentKey, Scene& scene);

// Revert entire instance to prefab defaults
void RevertInstanceToPrefab(EntityID instanceRoot, Scene& scene);

//------------------------------------------------------------------------------
// Prefab Updates (propagate changes from prefab asset to instances)
//------------------------------------------------------------------------------

// Reload and re-apply a prefab asset to all its instances in the scene
void RefreshPrefabInstances(const ClaymoreGUID& prefabGuid, Scene& scene);

// Clear the spam protection cache (call when entering play mode to retry failed prefabs)
void ClearPrefabFailedCache();
