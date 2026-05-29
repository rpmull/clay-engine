#pragma once

#include "InstancerComponent.h"
#include <glm/glm.hpp>
#include <array>

// Forward declarations
class Scene;
struct TerrainComponent;
struct TransformComponent;

namespace cm {
namespace instancer {

constexpr int kShadowCascadeCount = 4;
constexpr int kPointShadowLightCount = 4;
constexpr int kPointShadowFaceCount = 6;

//------------------------------------------------------------------------------
// InstancerDistributor - Handles procedural placement
//------------------------------------------------------------------------------
class InstancerDistributor {
public:
    struct DistributionParams {
        uint32_t Seed = 0;
        const TerrainComponent* Terrain = nullptr;
        glm::mat4 TerrainTransform = glm::mat4(1.0f);
        glm::mat4 InstancerTransform = glm::mat4(1.0f);
        
        // Area bounds (world space, relative to instancer)
        glm::vec2 AreaMin{-100.0f};
        glm::vec2 AreaMax{100.0f};
        bool UseRadius = true;
        float Radius = 100.0f;
        glm::vec3 CenterPosition{0.0f};
    };
    
    // Generate instances using Poisson disk sampling + rejection sampling
    void GenerateInstances(
        InstancerComponent& comp,
        const DistributionParams& params,
        std::vector<InstanceData>& outInstances);
    
    // Generate stable instance ID from position (public for manual point usage)
    uint32_t GenerateInstanceID(const glm::vec3& position, uint32_t seed);
    
private:
    // Poisson disk sampling for minimum spacing
    void PoissonDiskSample(
        const glm::vec2& areaMin,
        const glm::vec2& areaMax,
        float minDistance,
        float density,
        uint32_t seed,
        std::vector<glm::vec2>& outPositions);
    
    // Sample terrain height at position (returns false if outside terrain)
    bool SampleTerrainHeight(
        const TerrainComponent* terrain,
        const glm::mat4& terrainTransform,
        const glm::vec2& xz,
        float& outHeight);
    
    // Sample terrain slope at position
    bool SampleTerrainSlope(
        const TerrainComponent* terrain,
        const glm::mat4& terrainTransform,
        const glm::vec2& xz,
        float& outSlopeDegrees,
        glm::vec3& outNormal);
    
    // Apply random transform variation
    void ApplyRandomVariation(
        InstanceData& inst,
        const DistributionSettings& settings,
        const glm::vec3& terrainNormal,
        std::mt19937& rng);
};

//------------------------------------------------------------------------------
// InstancerSwapSystem - Manages LOD transitions between instances and prefabs
//------------------------------------------------------------------------------
class InstancerSwapSystem {
public:
    // Update swap state for all instances
    void Update(
        InstancerComponent& comp,
        Scene& scene,
        const glm::vec3& cameraPos,
        float deltaTime);
    
    // Get current active prefab count
    uint32_t GetActivePrefabCount() const { return m_CurrentActivePrefabs; }
    
private:
    // Swap instanced mesh to full prefab
    EntityID SwapToPrefab(
        InstancerComponent& comp,
        Scene& scene,
        uint32_t instanceIndex);
    
    // Swap full prefab back to instanced mesh
    void SwapToInstance(
        InstancerComponent& comp,
        Scene& scene,
        uint32_t instanceIndex);
    
    // Check if an active prefab entity was modified
    bool IsPrefabModified(Scene& scene, EntityID prefabRoot);
    
    // Check if an active prefab entity was destroyed
    bool IsPrefabDestroyed(Scene& scene, EntityID prefabRoot);
    
    uint32_t m_CurrentActivePrefabs = 0;
};

//------------------------------------------------------------------------------
// Shadow rendering contracts
//------------------------------------------------------------------------------
struct PointShadowFaceRender {
    uint16_t ViewId = 0;
    glm::mat4 LightViewProj{1.0f};
};

struct PointShadowLightRender {
    glm::vec3 Position{0.0f};
    float Range = 0.0f;
    std::array<PointShadowFaceRender, kPointShadowFaceCount> Faces{};
};

struct ShadowRenderParams {
    glm::vec3 CameraPosition{0.0f};
    float CameraNear = 0.1f;
    float ShadowDistance = 0.0f;
    int CascadeCount = 0;
    std::array<float, kShadowCascadeCount> CascadeSplits{};
    uint16_t FirstCascadeViewId = 0;
    std::array<glm::mat4, kShadowCascadeCount> CascadeMatrices{};
    uint64_t DirectionalStateFlags = 0;
    uint64_t PointStateFlags = 0;
    int PointLightCount = 0;
    std::array<PointShadowLightRender, kPointShadowLightCount> PointLights{};
};

struct ShadowRenderStats {
    uint32_t DirectionalSubmits = 0;
    uint32_t PointSubmits = 0;
    std::array<uint32_t, kShadowCascadeCount> CascadeSubmits{};
};

//------------------------------------------------------------------------------
// InstancerRenderer - GPU instanced rendering of mesh batches
//------------------------------------------------------------------------------
class InstancerRenderer {
public:
    static InstancerRenderer& Instance();
    
    // Load mesh data from asset into the component's cache
    bool LoadMeshData(InstancerComponent& comp);
    
    // Render all visible instances using GPU instancing
    void Render(
        uint16_t viewId,
        InstancerComponent& comp,
        const glm::mat4& view,
        const glm::mat4& proj,
        const glm::vec3& cameraPos,
        uint64_t stateFlags);
    
    // Render debug visualization
    void RenderDebug(
        uint16_t viewId,
        InstancerComponent& comp,
        const glm::mat4& view,
        const glm::mat4& proj);
    
private:
    friend class InstancerSystem;

    struct ShadowCandidate {
        glm::mat4 Transform{1.0f};
        glm::vec3 CenterWS{0.0f};
        float RadiusWS = 0.0f;
        uint8_t CascadeMask = 0;
    };

    InstancerRenderer();
    
    // Initialize static shader resources
    void EnsureInitialized();
    
    // Submit an instanced draw call for a batch
    void SubmitInstancedBatch(
        uint16_t viewId,
        const MeshInstanceBatch& batch,
        const std::vector<glm::mat4>& transforms,
        const glm::vec3& cameraPos,
        uint64_t stateFlags,
        bool useAlphaCutout = false,
        float alphaCutoutThreshold = 0.5f);

    bool PrepareShadowCandidates(
        InstancerComponent& comp,
        const glm::vec3& cameraPos,
        float shadowDistance,
        float cameraNear,
        const std::array<float, kShadowCascadeCount>& cascadeSplits,
        int cascadeCount);

    bool HasPointShadowCasterInRange(
        InstancerComponent& comp,
        const glm::vec3& lightPos,
        float lightRange,
        const glm::vec3& cameraPos,
        float shadowDistance);

    uint32_t RenderDirectionalShadows(
        InstancerComponent& comp,
        const ShadowRenderParams& params,
        ShadowRenderStats* stats);

    uint32_t RenderPointShadows(
        InstancerComponent& comp,
        const PointShadowLightRender& light,
        uint64_t stateFlags);

    uint32_t SubmitShadowBatch(
        uint16_t viewId,
        const MeshInstanceBatch& batch,
        const std::vector<uint32_t>& candidateIndices,
        bgfx::ProgramHandle program,
        uint64_t stateFlags,
        const glm::mat4* lightViewProj,
        const glm::vec4* pointLightPosRangeDepth,
        bool useAlphaCutout,
        float alphaCutoutThreshold);

    bool ResolveShadowBatchState(
        const InstancerComponent& comp,
        const MeshInstanceBatch& batch,
        bgfx::ProgramHandle defaultProgram,
        bgfx::ProgramHandle cutoutProgram,
        bgfx::ProgramHandle& outProgram,
        bool& outUseAlphaCutout,
        float& outAlphaCutoutThreshold,
        uint64_t& inOutStateFlags) const;
    
    bool m_Initialized = false;
    bgfx::ProgramHandle m_InstancedProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_ShadowDepthProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_ShadowDepthCutoutProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_PointShadowDepthProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_PointShadowDepthCutoutProgram = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout m_InstanceLayout;
    bgfx::UniformHandle m_u_cameraPos = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_lightViewProj = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_PBRScalar1 = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_pointShadowLightPosRangeDepth = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_UVTransform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_s_albedo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_s_normal = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_s_metalRoughness = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_WhiteTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_DefaultNormal = BGFX_INVALID_HANDLE;
    std::vector<ShadowCandidate> m_ShadowCandidates;
    std::array<std::vector<uint32_t>, kShadowCascadeCount> m_ShadowCascadeCandidates;
    std::vector<uint32_t> m_PointShadowCandidates;
};

//------------------------------------------------------------------------------
// InstancerSystem - High-level system coordinating all instancer operations
//------------------------------------------------------------------------------
class InstancerSystem {
public:
    static InstancerSystem& Instance();
    
    // Update all instancers in the scene
    void Update(Scene& scene, float deltaTime);
    
    // Render all instancers (called from Renderer)
    // view/proj are float[16] arrays to match bgfx convention
    void Render(
        uint16_t viewId,
        Scene& scene,
        const float* view,
        const glm::mat4& proj,
        const glm::vec3& cameraPos,
        uint64_t stateFlags);

    bool HasPointShadowCaster(
        Scene& scene,
        const glm::vec3& lightPos,
        float lightRange,
        const glm::vec3& cameraPos,
        float shadowDistance);

    void RenderShadows(
        Scene& scene,
        const ShadowRenderParams& params,
        ShadowRenderStats* outStats = nullptr);
    
    // Force regeneration of a specific instancer
    void RegenerateInstancer(EntityID entityId, Scene& scene);
    
private:
    InstancerSystem() = default;
    
    InstancerDistributor m_Distributor;
    InstancerSwapSystem m_SwapSystem;
};

} // namespace instancer
} // namespace cm

