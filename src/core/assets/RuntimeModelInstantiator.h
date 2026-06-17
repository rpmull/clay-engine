#pragma once

#include "RuntimeModelManifest.h"
#include "core/ecs/Entity.h"
#include <glm/glm.hpp>
#include <string>

class Scene;
struct EntityData;
class IAssetResolver;

namespace cm {

/**
 * Runtime Model Instantiator
 * 
 * Creates full model hierarchies at runtime using compiled .modelrt manifests.
 * This is the runtime equivalent of Scene::InstantiateModel() which requires
 * FBX loading and .meta files.
 * 
 * Use cases:
 * - Scripts calling Mesh.Instantiate() with hierarchy=true
 * - Prefabs referencing model assets
 * - Dynamic model spawning (enemies, items, props)
 */
class RuntimeModelInstantiator {
public:
    /**
     * Instantiate a model from a .modelrt manifest path
     * @param manifestPath Virtual path to .modelrt file
     * @param scene Target scene
     * @param position World position for root
     * @return Root entity ID, or -1 on failure
     */
    static EntityID Instantiate(const std::string& manifestPath, Scene& scene, 
                                 const glm::vec3& position = glm::vec3(0.0f),
                                 EntityID existingRoot = INVALID_ENTITY_ID);
    
    /**
     * Instantiate from a model GUID
     * Resolves GUID → .modelrt path using asset resolver
     */
    static EntityID InstantiateByGuid(const ClaymoreGUID& modelGuid, Scene& scene,
                                       const glm::vec3& position = glm::vec3(0.0f),
                                       EntityID existingRoot = INVALID_ENTITY_ID);
    
    /**
     * Instantiate from an already-loaded manifest
     */
    static EntityID InstantiateFromManifest(const RuntimeModelManifest& manifest, Scene& scene,
                                             const glm::vec3& position = glm::vec3(0.0f),
                                             EntityID existingRoot = INVALID_ENTITY_ID);
    
private:
    static EntityID CreateHierarchy(const RuntimeModelManifest& manifest, Scene& scene,
                                     const glm::vec3& position,
                                     EntityID existingRoot);
    
    // Apply materials from manifest to entity - creates actual Material objects
    static void ApplyMaterials(Scene& scene,
                               EntityData* data, 
                               const std::vector<RuntimeMaterialSlot>& materials,
                               IAssetResolver* resolver,
                               bool skinned);
};

} // namespace cm

