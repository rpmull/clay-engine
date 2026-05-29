#pragma once

#include "PrefabAsset.h"
#include "core/ecs/Entity.h"
#include "core/assets/IAssetResolver.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

class Scene;

/**
 * Remap entity references in all components (including script fields) after prefab instantiation.
 * This handles:
 * - Skeleton BoneEntities
 * - Skinning SkeletonRoot
 * - BoneAttachment SkeletonEntity
 * - IK TargetEntity/PoleEntity
 * - LookAtConstraint TargetEntity
 * - NavAgent NavMeshEntity
 * - MeshProxy SerializedTarget
 * - Script entity-type field values (PropertyType::Entity, ComponentRef, ScriptRef)
 * 
 * Entity references that can't be remapped (external to prefab) are cleared to -1.
 */
void RemapPrefabEntityReferences(Scene& scene, const std::vector<EntityID>& createdEntityIds,
                                  const std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                  const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* prefabToInstanceGuid = nullptr,
                                  const std::unordered_map<ClaymoreGUID, EntityID>* instanceGuidToId = nullptr);

/**
 * Resolve script entity references within a prefab hierarchy using GUID remapping.
 * This is called after prefab instantiation to fix entity references that couldn't be
 * resolved during initial deserialization (because entity GUIDs change on instantiation).
 * 
 * For each script property that is an entity reference (-1 unresolved or stale ID):
 * 1. Looks up the original reference JSON from the prefab asset
 * 2. Maps any original GUIDs through prefabToInstanceGuid to find new instance GUIDs
 * 3. Resolves modelGuid + modelNodePath paths within the prefab hierarchy
 * 
 * @param scene The scene containing the prefab instance
 * @param prefabRoot The root entity of the prefab instance
 * @param prefabJson The original prefab JSON entities array
 * @param prefabToInstanceGuid Mapping from original prefab GUIDs to new instance GUIDs
 * @param instanceGuidToId Mapping from new instance GUIDs to EntityIDs
 */
void ResolvePrefabScriptEntityReferences(
    Scene& scene,
    EntityID prefabRoot,
    const nlohmann::json& prefabJson,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& prefabToInstanceGuid,
    const std::unordered_map<ClaymoreGUID, EntityID>& instanceGuidToId,
    const std::vector<EntityID>* createdEntityIds = nullptr);

namespace runtime {

enum class PrefabAsyncStatus : uint8_t {
    Pending = 0,
    Ready = 1,
    Failed = 2,
    NotFound = 3
};

/**
 * Runtime Prefab Instantiator
 * 
 * Creates entities in a scene from binary prefab data (.prefabb files).
 * Uses the same Serializer::Deserialize* functions as PrefabAPI.cpp to ensure
 * exact parity between editor and runtime instantiation.
 * 
 * The binary prefab format stores component data as embedded JSON, which
 * is then processed using the same deserialization functions as the editor.
 */
class RuntimePrefabInstantiator {
public:
    /**
     * Instantiate a prefab from a .prefabb file path
     * @param prefabPath Path to the .prefabb file (resolved via VFS)
     * @param scene Target scene to instantiate into
     * @return Root entity ID, or INVALID_ENTITY_ID on failure
     */
    static EntityID Instantiate(const std::string& prefabPath, Scene& scene,
                                EntityID existingRoot = INVALID_ENTITY_ID,
                                bool useExistingRoot = false);
    
    /**
     * Instantiate a prefab asynchronously (non-blocking).
     * Returns a placeholder root entity immediately; full hierarchy is built
     * over subsequent frames via UpdateAsync().
     */
    static EntityID InstantiateAsync(const std::string& prefabPath, Scene& scene,
                                     EntityID existingRoot = INVALID_ENTITY_ID,
                                     bool useExistingRoot = false);
    
    /**
     * Blocking/synchronous prefab instantiation (legacy behavior).
     */
    static EntityID InstantiateBlocking(const std::string& prefabPath, Scene& scene,
                                        EntityID existingRoot = INVALID_ENTITY_ID,
                                        bool useExistingRoot = false);
    
    /**
     * Instantiate a prefab from a loaded PrefabAsset
     * @param asset Loaded binary prefab asset
     * @param scene Target scene to instantiate into
     * @return Root entity ID, or INVALID_ENTITY_ID on failure
     */
    static EntityID InstantiateFromAsset(const PrefabAsset& asset, Scene& scene,
                                         EntityID existingRoot = INVALID_ENTITY_ID,
                                         bool useExistingRoot = false);
    
    /**
     * Instantiate a prefab by GUID (resolves via asset manifest)
     * @param prefabGuid GUID of the prefab asset
     * @param scene Target scene to instantiate into
     * @return Root entity ID, or INVALID_ENTITY_ID on failure
     */
    static EntityID InstantiateByGuid(const ClaymoreGUID& prefabGuid, Scene& scene,
                                      EntityID existingRoot = INVALID_ENTITY_ID,
                                      bool useExistingRoot = false);
    
    /**
     * Process queued async prefab instantiations with a time budget (ms).
     * Call once per frame on the main thread.
     */
    static void UpdateAsync(double budgetMs = 2.0);

    /**
     * Query async prefab status by placeholder/root entity.
     */
    static PrefabAsyncStatus GetAsyncStatus(EntityID rootEntity, Scene& scene);

    /**
     * Clear prefab runtime caches (binary bytes, model deltas, prefab JSON fallback).
     * Call on play-mode/session boundaries to avoid stale prefab data reuse.
     */
    static void ResetRuntimeCaches();

    /**
     * Cancel queued async prefab requests targeting a specific scene.
     * Optionally removes placeholder roots that were created for those requests.
     */
    static void CancelAsyncForScene(Scene& scene, bool removePlaceholders = true);
};

} // namespace runtime

