#pragma once

#include "ResourceLayerTypes.h"
#include "core/assets/IAssetResolver.h"
#include <unordered_map>
#include <vector>
#include <utility>

// Forward declarations
class Scene;
struct PrefabAsset;
class Material;

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// ImposterCache - Cached GPU data for instanced imposter rendering
//------------------------------------------------------------------------------
struct ImposterCache {
    bgfx::VertexBufferHandle VBH = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle IBH = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle AlbedoTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle NormalTexture = BGFX_INVALID_HANDLE;
    std::shared_ptr<Material> ImposterMaterial;
    uint32_t IndexCount = 0;
    glm::vec3 BoundsMin{0.0f};
    glm::vec3 BoundsMax{0.0f};
    bool Valid = false;
    
    void Release();
    ~ImposterCache() { Release(); }
    
    // Non-copyable but movable
    ImposterCache() = default;
    ImposterCache(const ImposterCache&) = delete;
    ImposterCache& operator=(const ImposterCache&) = delete;
    ImposterCache(ImposterCache&& other) noexcept;
    ImposterCache& operator=(ImposterCache&& other) noexcept;
};

//------------------------------------------------------------------------------
// ImposterManager - Bakes and caches imposter meshes from prefabs
//------------------------------------------------------------------------------
class ImposterManager {
public:
    static ImposterManager& Instance();
    
    // Bake an imposter from a prefab (extracts and combines meshes)
    // Returns false if prefab has no meshes or baking failed
    bool BakeImposter(const ClaymoreGUID& prefabGuid, ImposterCache& outCache);
    
    // Get cached imposter (loads/bakes if needed)
    ImposterCache* GetImposter(const ClaymoreGUID& prefabGuid);
    
    // Check if an imposter is cached
    bool HasImposter(const ClaymoreGUID& prefabGuid) const;
    
    // Clear all cached imposters
    void ClearCache();
    
    // Clear a specific imposter
    void ClearImposter(const ClaymoreGUID& prefabGuid);
    
    // Render all visible imposters for resource layers using GPU instancing
    void RenderImposters(
        uint16_t viewId,
        ResourceLayerComponent& comp,
        const glm::mat4& view,
        const glm::mat4& proj,
        const glm::vec3& cameraPos,
        uint64_t stateFlags
    );
    
private:
    ImposterManager() = default;
    
    // Helper to collect meshes from prefab hierarchy
    bool CollectPrefabMeshes(
        const PrefabAsset& prefab,
        std::vector<glm::vec3>& outPositions,
        std::vector<glm::vec3>& outNormals,
        std::vector<glm::vec2>& outUVs,
        std::vector<glm::vec4>& outTangents,
        std::vector<uint32_t>& outIndices,
        glm::vec3& outBoundsMin,
        glm::vec3& outBoundsMax
    );
    
    // Render debug markers for instances without valid imposters
    void RenderDebugMarkers(
        const std::vector<std::pair<glm::vec3, glm::vec3>>& points,
        uint16_t viewId
    );
    
    // Cache of baked imposters by prefab GUID
    std::unordered_map<ClaymoreGUID, ImposterCache, ClaymoreGUIDHasher> m_Cache;
};

//------------------------------------------------------------------------------
// ProximitySwapSystem - Manages LOD transitions between imposters and prefabs
//------------------------------------------------------------------------------
class ProximitySwapSystem {
public:
    // Update swap state for all instances
    void Update(
        ResourceLayerComponent& comp,
        Scene& scene,
        const glm::vec3& cameraPos,
        float deltaTime
    );
    
    // Set the prefab budget (max active prefabs)
    void SetMaxActivePrefabs(uint32_t max) { m_MaxActivePrefabs = max; }
    
    // Get current active prefab count
    uint32_t GetActivePrefabCount() const { return m_CurrentActivePrefabs; }
    
private:
    // Swap imposter to full prefab
    EntityID SwapToActive(
        ResourceLayerComponent& comp,
        Scene& scene,
        uint32_t instanceIndex
    );
    
    // Swap full prefab back to imposter
    void SwapToImposter(
        ResourceLayerComponent& comp,
        Scene& scene,
        uint32_t instanceIndex
    );
    
    // Check if an active prefab entity was modified from its original state
    bool IsPrefabModified(Scene& scene, EntityID prefabRoot);
    
    // Check if an active prefab entity was destroyed
    bool IsPrefabDestroyed(Scene& scene, EntityID prefabRoot);
    
    uint32_t m_MaxActivePrefabs = 256;
    uint32_t m_CurrentActivePrefabs = 0;
};

//------------------------------------------------------------------------------
// ResourceLayerRenderer - Integrates resource layer rendering with main renderer
//------------------------------------------------------------------------------
class ResourceLayerRenderer {
public:
    // Render all resource layer imposters
    void Render(
        uint16_t viewId,
        ResourceLayerComponent& comp,
        const glm::mat4& view,
        const glm::mat4& proj,
        const glm::vec3& cameraPos,
        uint64_t stateFlags
    );
    
    // Update visibility and distance for all instances (call before Render)
    void UpdateVisibility(
        ResourceLayerComponent& comp,
        const glm::vec3& cameraPos,
        const glm::mat4& viewProj
    );
    
private:
    // Frustum culling helper
    bool IsVisible(const glm::vec3& position, float radius, const glm::mat4& viewProj) const;
};

} // namespace resourcelayer
} // namespace cm

